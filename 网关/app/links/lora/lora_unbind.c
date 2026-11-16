/**
 * @file  lora_unbind.c
 * @brief 设备解绑（LoRa 0x05）：两阶段状态机 + 异步收尾
 *
 * 时序（用户 2026-09-30 最终口径）：
 *   阶段1(30s): 每 3s 发 0x05，收到 ack 立即停发 → 等 15s
 *     30s 没收到 ack → 上报 UNBIND_FAILED（从机记录保留）
 *   等 15s: 给设备清自己保存的网关值
 *   阶段2(10s, 最多循环 3 次): 每 3s 发 0x05
 *     10s 没收到 ack → 解绑成功 → 删 config_store + 从机表 → 上报 UNBOUND
 *     收到 ack → 设备没清 → 循环阶段2（不再等 15s）
 *     3 次都收到 ack → 设备故障 → 上报 UNBIND_FAULT（从机记录保留）
 *
 * 0x05 帧【不进 data_cache】：走从机的 pending_func=0x05 直接发，与正常下发隔离；
 * 解绑期间目标从机是 WAIT_ACK，lora_downlink.c 看到 pending_func==0x05 会跳过。
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "lora_business.h"
#include "lora_internal.h"
#include "lora_proto.h"
#include "config_store.h"
#include "cloud_proto.h"      /* cloud_proto_get_now_ts */
#include "bsp_tim.h"
#include "sensor_offline.h"   /* 液位解绑的阶段超时按上报周期算 */
#include "log.h"

/* ========== 解绑两阶段状态机(2026-09-30 用户最终口径) ==========
 *
 * 【为什么 g_unbind 完全独立、不复用从机 pending_func 重传逻辑】
 *   原来的解绑走的是从机表的 pending_func=0x05 + lora_downlink_logic 5s 重试/3 次
 *   耗尽强制删, 那套口径是"等不到 ACK 就强制删设备" —— 与用户最终口径冲突:
 *     - 阶段1 等不到 ack 是【失败上报】, 不删设备
 *     - 阶段2 收不到 ack 才是【成功】, 收到 ack 才是失败
 *   而且阶段切换(等15s / 阶段2 循环)用从机 pending_func 表达不了。
 *   所以 g_unbind 单独维护状态机, 只把从机表的 state/pending_func 当"锁":
 *   启动时把目标 slave 置 WAIT_ACK+pending_func=0x05, lora_downlink_logic 看到
 *   pending_func==0x05 就 continue 跳过(不干扰); 结束时清回 IDLE。
 *
 * 时序(详见 lora_business.h 上方常量注释):
 *   阶段1(30s): 每 3s 发 0x05, 收到 ack 立即停发 → 等15s
 *     30s 没收到 ack → 上报 UNBIND_FAILED(从机记录保留)
 *   等15s: 给设备清自己保存的网关值
 *   阶段2(10s, 最多循环3次): 每 3s 发 0x05
 *     10s 没收到 ack → 解绑成功 → 删 config_store + 从机表 → 上报 UNBOUND
 *     收到 ack → 设备没清 → 循环阶段2 的 10s(不再等15s)
 *     3 次都收到 ack → 设备故障 → 上报 UNBIND_FAULT(从机记录保留不删)
 *
 * ★ tick 比较一律用 (uint32_t)(now - start) >= DURATION 无符号差值(自动处理回绕) */
typedef enum
{
    UNBIND_IDLE        = 0,   /* 未在解绑 */
    UNBIND_PHASE1      = 1,   /* 阶段1: 30s 内每 3s 发 0x05, 收 ack 立即停 */
    UNBIND_WAIT_CLEAR  = 2,   /* 等 15s 给设备清网关值 */
    UNBIND_PHASE2      = 3,   /* 阶段2: 10s 内每 3s 发 0x05, 收 ack 循环/故障 */
} unbind_state_t;

static struct
{
    unbind_state_t state;
    uint8_t        uid[CFG_UID_BYTES];            /* 解绑目标设备 UID(13B) */
    bool           is_sensor;         /* 设备类型(sensor=true / alarm=false), 用于事件回调 */
    uint8_t        slave_idx;         /* 从机表索引(0xFF = 无效); 解绑期间作"锁"用 */
    uint32_t       phase_start_tick;  /* 当前阶段开始 tick */
    uint32_t       last_send_tick;    /* 上次发 0x05 的 tick */
    uint8_t        phase2_retry;      /* 阶段2 已收 ack 的次数(>=3 = 设备故障) */
} g_unbind =
{
    .state        = UNBIND_IDLE,
    .slave_idx    = 0xFFU,
    .phase2_retry = 0U,
};
_Static_assert(sizeof(g_unbind.uid) == CFG_UID_BYTES,
               "g_unbind.uid 必须 13 字节");


/* ========== 内部: 解绑收尾(P5, LoRa 0x05) ==========
 *
 * 在两处被调用:
 *   1) handle_ack_from_device: 设备回 ACK, 正常解绑成功
 *   2) lora_downlink_logic 重试耗尽: 用户口径"重试耗尽也强制删除"
 *      (设备可能已经离线/掉电, 不能因为它不回 ACK 就永远删不掉)
 *
 * 【调用方必须在调本函数前清掉自己的 s->state/pending_func】,
 * 本函数只负责 config_store + 运行期表 + 事件, 不碰 s->state(因为
 * lora_slave_del 会把末尾从机搬到当前格, s 已经不一定指向原来了)。
 *
 * 返回 true = 已删除; false = uid 解 hex 失败(运行期表仍删, 但 config_store 没动) */
static bool _unbind_finalize(const uint8_t uid[CFG_UID_BYTES], bool is_sensor)
{
    char  uid_hex[CFG_UID_STR_MAX];
    /* cfg_uid_from_bytes 把 13 字节转 hex, 不会失败(非全 0 即有效) */
    (void)cfg_uid_from_bytes(uid, uid_hex);
    /* ★2026-10-27: 解绑成功后清掉该 UID 遗留的下发缓存★
     * 典型场景: 曾用本地 #bind(默认液位)把声光误绑成液位, 排过一条 0x01(带校时);
     * 解绑/重绑成声光后缓存还在, 声光 IDLE 时会被 lora_try_send_alarm_idle() 发出去
     * -> "声光又被校时"。这里一并清掉。 */
    (void)data_cache_down_del_by_uid((uint8_t *)uid);
    (void)config_store_unbind_device(uid_hex, cloud_proto_get_now_ts());
    log_info("[UNBIND] %02X%02X%02X(%s) 已删除 config_store, 触发 UNBOUND 事件\r\n",
             uid[0], uid[1], uid[2], uid_hex);
    (void)lora_slave_del(uid);
    lora_bind_fire_event(uid_hex, is_sensor, LORA_BIND_EVENT_UNBOUND);
    return true;
}


/* ========== 解绑两阶段状态机 helpers(2026-09-30 用户最终口径) ==========
 *
 * 6 个 helper 配合 g_unbind 状态机:
 *   _unbind_send_frame()        —— 用从机表里目标 slave 的 pending_func=0x05 发一帧
 *   _unbind_fire_event()        —— 把 g_unbind.uid 转 hex + 调 lora_bind_fire_event 上报事件
 *   _unbind_clear_slave_state() —— 清从机表里目标格的 state/pending_func(不删条目)
 *   _unbind_reset()             —— 清 g_unbind 回 IDLE(释放锁)
 *   lora_unbind_on_ack()            —— 收到设备 ack 时按 state 处理
 *   lora_unbind_logic()             —— 周期驱动(挂在 lora_business_poll)
 *
 * 【锁的语义】解绑期间目标 slave 一直是 WAIT_ACK+pending_func=0x05:
 *   - lora_downlink_logic 看到 pending_func==0x05 直接 continue 跳过(不干扰)
 *   - handle_ack_from_device 顶部拦截 g_unbind.state!=IDLE 且 slave_idx 匹配的 ack,
 *     路由到 lora_unbind_on_ack, 不走旧逻辑
 *   - 结束(成功/失败/故障)都清回 IDLE+pending_func=0 释放锁
 *
 * ★ 成功路径(_unbind_finalize 调 lora_slave_del 会让 slave_idx 失效)必须
 *   先拷贝 uid/is_sensor 到本地, 再 _unbind_reset, 最后 _unbind_finalize。
 *   失败/故障路径不删从机记录, 只清 slave 状态 + _unbind_reset + 上报事件。 */

/* 用从机表里目标 slave 的 pending_func=0x05 发一帧 0x05。
 * pending_func/pending_data_len 在 lora_business_unbind 启动时已设好,
 * 这里只负责发送 + 刷新时间戳。失败(TX 忙)也更新 last_send_tick, 3s 后再试。 */
static void _unbind_send_frame(void)
{
    if (g_unbind.slave_idx >= g_slave_count) return;
    lora_slave_t *s = &g_slave_list[g_unbind.slave_idx];
    (void)lora_tx_downlink(g_unbind.slave_idx);
    uint32_t now              = get_tick_ms();
    s->send_req_tick          = now;
    g_unbind.last_send_tick   = now;
}

/* 把 g_unbind.uid 转 hex + 调 lora_bind_fire_event 上报事件 */
static void _unbind_fire_event(lora_bind_event_t event)
{
    char uid_hex[CFG_UID_STR_MAX];
    /* cfg_uid_from_bytes 把 13 字节转 hex, 不会失败(非全 0 即有效) */
    (void)cfg_uid_from_bytes(g_unbind.uid, uid_hex);
    lora_bind_fire_event(uid_hex, g_unbind.is_sensor, event);
}

/* 清从机表里目标 slave 的 state/pending_func —— 不删条目(失败/故障路径用,
 * 从机记录保留); 成功路径走 _unbind_finalize 的 lora_slave_del 删条目。
 * 校验 uid 匹配: 防止 lora_slave_del/add 改了布局后索引错位误清别的从机。 */
static void _unbind_clear_slave_state(void)
{
    if (g_unbind.slave_idx < g_slave_count)
    {
        lora_slave_t *s = &g_slave_list[g_unbind.slave_idx];
        if (memcmp(s->dev_uid, g_unbind.uid, CFG_UID_BYTES) == 0)
        {
            s->state            = SLAVE_STATE_IDLE;
            s->retry_cnt        = 0U;
            s->pending_func     = 0U;
            s->pending_data_len = 0U;
        }
    }
}

/* 清 g_unbind 回 IDLE(释放锁) */
static void _unbind_reset(void)
{
    g_unbind.state        = UNBIND_IDLE;
    g_unbind.slave_idx    = 0xFFU;
    g_unbind.phase2_retry = 0U;
    memset(g_unbind.uid, 0, sizeof(g_unbind.uid));
}

/* 收到设备 ack(0x80) 时由 handle_ack_from_device 路由进来。
 * - PHASE1: 立即停发(本函数本身就是收到 ack 的瞬间, 不会再发) → 等 15s
 * - PHASE2: phase2_retry++; >=3 → 设备故障(上报 UNBIND_FAULT, 从机记录保留);
 *           否则循环阶段2 的 10s(不再等 15s): 重置 phase_start_tick + 立即发一帧 */
void lora_unbind_on_ack(void)
{
    uint32_t now = get_tick_ms();

    if (g_unbind.state == UNBIND_PHASE1)
    {
        log_info("[UNBIND] 阶段1 收到 %02X%02X%02X 的 ack, 立即停发, 等 15s 让设备清网关值\r\n",
                 g_unbind.uid[0], g_unbind.uid[1], g_unbind.uid[2]);
        g_unbind.state            = UNBIND_WAIT_CLEAR;
        g_unbind.phase_start_tick = now;
    }
    else if (g_unbind.state == UNBIND_PHASE2)
    {
        g_unbind.phase2_retry++;
        if (g_unbind.phase2_retry >= LORA_UNBIND_PHASE2_MAX_RETRY)
        {
            /* 3 次都收到 ack = 设备没清网关值 = 设备故障 */
            log_info("[UNBIND] !! 阶段2 第 %u 次收到 ack(>= %u), 设备故障, "
                     "上报 UNBIND_FAULT(从机记录保留)\r\n",
                     (unsigned)g_unbind.phase2_retry,
                     (unsigned)LORA_UNBIND_PHASE2_MAX_RETRY);
            _unbind_fire_event(LORA_BIND_EVENT_UNBIND_FAULT);
            _unbind_clear_slave_state();
            _unbind_reset();
        }
        else
        {
            /* 设备没清 → 循环阶段2 的 10s(不再等 15s): 重置阶段计时 + 立即发一帧 */
            log_info("[UNBIND] 阶段2 第 %u 次收到 ack(< %u), 设备没清, "
                     "循环阶段2(不再等 15s)\r\n",
                     (unsigned)g_unbind.phase2_retry,
                     (unsigned)LORA_UNBIND_PHASE2_MAX_RETRY);
            g_unbind.phase_start_tick = now;
            g_unbind.last_send_tick   = now;
            _unbind_send_frame();
        }
    }
    /* WAIT_CLEAR / IDLE 状态收到 ack 不可能(没发帧), 忽略 */
}

/* 目标设备的上报周期(秒) —— 液位解绑的阶段超时按它算 */
static uint16_t _unbind_period_s(void)
{
    char uid_hex[CFG_UID_STR_MAX];

    (void)cfg_uid_from_bytes(g_unbind.uid, uid_hex);
    return sensor_offline_period_for_uid(config_store_get(), uid_hex);
}

/* 液位的阶段超时: periods × 上报周期, 封顶 LORA_UNBIND_SENSOR_MAX_MS */
static uint32_t _unbind_phase_ms(uint8_t periods)
{
    uint32_t ms = (uint32_t)_unbind_period_s() * (uint32_t)periods * 1000U;

    return (ms > LORA_UNBIND_SENSOR_MAX_MS) ? LORA_UNBIND_SENSOR_MAX_MS : ms;
}

/* 周期驱动(挂在 lora_business_poll, 每轮调):
 * - PHASE1: 30s 超时未收 ack → 失败上报(从机记录保留); 3s 间隔发 0x05
 * - WAIT_CLEAR: 15s 到 → 进 PHASE2(立即发)
 * - PHASE2: 10s 超时未收 ack → 成功(删表 + 上报 UNBOUND); 3s 间隔发 0x05
 *
 * ★ tick 比较一律用 (uint32_t)(now - start) >= DURATION 无符号差值(自动处理回绕) */
void lora_unbind_logic(void)
{
    if (g_unbind.state == UNBIND_IDLE) return;

    uint32_t now = get_tick_ms();

    if (g_unbind.state == UNBIND_PHASE1)
    {
        /* 阶段1 超时: 声光按固定 30s(常驻 RX); 液位按【上报周期】算 ——
         * 休眠设备只有上报后的窗口能收包, 按秒算没有意义(见 lora_business.h 的说明)。 */
        uint32_t limit = g_unbind.is_sensor
                             ? _unbind_phase_ms(LORA_UNBIND_SENSOR_PHASE1_PERIODS)
                             : (uint32_t)LORA_UNBIND_PHASE1_MS;

        if ((uint32_t)(now - g_unbind.phase_start_tick) >= limit)
        {
            log_info("[UNBIND] !! 阶段1 %lus 没收到 ack, 上报 UNBIND_FAILED(从机记录保留)\r\n",
                     (unsigned long)(limit / 1000U));
            _unbind_fire_event(LORA_BIND_EVENT_UNBIND_FAILED);
            _unbind_clear_slave_state();
            _unbind_reset();
            return;
        }
        /* 声光: 3s 间隔盲发; 液位: 不发墙钟帧, 由 lora_unbind_window_hit()
         *   在它上报后的指令窗口里发(那才是唯一能收到的时候)。 */
        if ((!g_unbind.is_sensor) &&
            ((uint32_t)(now - g_unbind.last_send_tick) >= LORA_UNBIND_PHASE1_INTERVAL_MS))
        {
            _unbind_send_frame();
        }
    }
    else if (g_unbind.state == UNBIND_WAIT_CLEAR)
    {
        /* 等 15s 给设备清网关值, 到了进阶段2 */
        if ((uint32_t)(now - g_unbind.phase_start_tick) >= LORA_UNBIND_WAIT_CLEAR_MS)
        {
            log_info("[UNBIND] 等 15s 完成, 进入阶段2(%s)\r\n",
                     g_unbind.is_sensor ? "液位: 观察上报周期内是否还响应"
                                        : "10s, 最多循环 3 次");
            g_unbind.state            = UNBIND_PHASE2;
            g_unbind.phase_start_tick = now;
            g_unbind.last_send_tick   = now;
            if (!g_unbind.is_sensor)      /* 液位没有窗口, 不能盲发 */
            {
                _unbind_send_frame();
            }
        }
    }
    else if (g_unbind.state == UNBIND_PHASE2)
    {
        /* 超时没收到 ack = 设备已清网关值 = 解绑成功
         *   声光: 固定 10s;
         *   液位: ACK 之后【2 个上报周期内不再响应】—— 设备清完会重启进工厂流程、
         *         不再上报, 所以这段时间里既不会有 0x02 也不会有 ack。 */
        uint32_t limit = g_unbind.is_sensor
                             ? _unbind_phase_ms(LORA_UNBIND_SENSOR_PHASE2_PERIODS)
                             : (uint32_t)LORA_UNBIND_PHASE2_MS;

        if ((uint32_t)(now - g_unbind.phase_start_tick) >= limit)
        {
            /* 成功路径: 先拷 uid/is_sensor, 再 _unbind_reset(清 g_unbind 释放锁),
             * 最后 _unbind_finalize —— 它会 lora_slave_del 让 slave_idx 失效,
             * 所以必须先 reset 释放索引引用, 再用 uid_copy 找条目删 */
            uint8_t uid_copy[CFG_UID_BYTES];
            bool    is_sensor = g_unbind.is_sensor;
            memcpy(uid_copy, g_unbind.uid, CFG_UID_BYTES);
            log_info("[UNBIND] 阶段2 %lus 没收到 ack, 设备已清网关值, 解绑成功\r\n",
                     (unsigned long)(limit / 1000U));
            _unbind_reset();
            (void)_unbind_finalize(uid_copy, is_sensor);
            return;
        }
        if ((!g_unbind.is_sensor) &&
            ((uint32_t)(now - g_unbind.last_send_tick) >= LORA_UNBIND_PHASE2_INTERVAL_MS))
        {
            _unbind_send_frame();
        }
    }
}

/* 液位解绑的"窗口机会" —— 由 lora_reply_logic() 在回完 0x80、再等 3s 后调用。
 * 那 3s 正好让设备的"5s 等 ACK 窗口"结束、进入"10s 等指令窗口", 这是它唯一
 * 能收到 0x05 的时候。只对【液位】且【正在解绑该设备】生效。 */
/* ★2026-11-03: 设备发来 0x04(广播寻网) = 它已经把网关值清成 FFFFFF 了★
 *
 * 【为什么需要】阶段2 的判据是"ACK 后 2 个上报周期内不再响应"(默认 300s 周期
 *   => 要等 10 分钟)。但设备清完会【立刻重启】并发 0x04 找新网关(实测 2~3s 后
 *   就开始)。这是确定的证据, 可以当场判成功, 不用干等。
 * 【只认 WAIT_CLEAR/PHASE2】阶段1 还没确认它收到过 0x05, 不拿这个当证据。
 * 【调用方】lora_business.c 的 on_frame: 0x04 + gw 全 FF + 目标是解绑对象。 */
void lora_unbind_dev_cleared(void)
{
    uint8_t uid_copy[CFG_UID_BYTES];
    bool    is_sensor;

    if ((g_unbind.state != UNBIND_WAIT_CLEAR) && (g_unbind.state != UNBIND_PHASE2))
    {
        return;
    }

    is_sensor = g_unbind.is_sensor;
    memcpy(uid_copy, g_unbind.uid, CFG_UID_BYTES);
    log_info("[UNBIND] 收到 %02X%02X%02X 的 0x04(gw=广播) = 它已清网关值, 立即判成功\r\n",
             uid_copy[0], uid_copy[1], uid_copy[2]);
    _unbind_reset();
    (void)_unbind_finalize(uid_copy, is_sensor);
}

bool lora_unbind_window_hit(uint8_t slave_idx)
{
    if ((g_unbind.state == UNBIND_IDLE) || (!g_unbind.is_sensor))
    {
        return false;
    }
    if (slave_idx != g_unbind.slave_idx)
    {
        return false;
    }
    if ((g_unbind.state != UNBIND_PHASE1) && (g_unbind.state != UNBIND_PHASE2))
    {
        return false;
    }

    log_info("[UNBIND] 液位上报窗口到了 -> 在窗口里发 0x05(阶段%u)\r\n",
             (g_unbind.state == UNBIND_PHASE1) ? 1u : 2u);
    _unbind_send_frame();
    return true;
}


/* ==================== 设备绑定/解绑(P5) 对外 API ====================
 *
 * 绑定窗口状态机的对外入口: lora_business_open_bind_window 由 cloud_proto 的
 * pair_hook 调用(经 main.c 注册); lora_business_set_bind_event_cb 由 main.c
 * 在 init 时注册, 回调里组 event.device JSON 发到云端。
 *
 * 解绑 lora_business_unbind / 解绑的 0x05 重试 + cloud_cmd_unbind 的异步化
 * 在步骤5实现, 这里先把绑定的两个 API 落地。
 */
/* ==================== 解绑(P5, LoRa 0x05, 两阶段状态机) ====================
 *
 * 【异步】接口: 收到调用立即进入阶段1, 不等结果。真正删 config_store + 从机表
 *   的动作在阶段2 成功路径由 lora_unbind_logic 调 _unbind_finalize 完成; 失败/故障
 *   路径只清从机状态(记录保留) + 上报事件, 由上层据此重试或人工介入。
 *
 * 【两阶段流程】(详见 g_unbind 上方注释 + lora_business.h 常量):
 *   阶段1(30s): 每 3s 发 0x05, 收到 ack 立即停发 → 等 15s → 阶段2
 *     30s 没收到 ack → 上报 UNBIND_FAILED(从机记录保留)
 *   阶段2(10s, 最多循环3次): 每 3s 发 0x05
 *     10s 没收到 ack → 解绑成功 → 删表 + 上报 UNBOUND
 *     收到 ack → 设备没清 → 循环阶段2(不再等 15s)
 *     3 次都收到 ack → 设备故障 → 上报 UNBIND_FAULT(从机记录保留)
 *
 * ★2026-11-03: 以上时长只适用于【声光(常驻 RX)】。液位是休眠设备, 走"窗口驱动":
 *   阶段1 = 3 个上报周期(等它上报才发 0x05), 阶段2 = ACK 后 2 个上报周期
 *   (它清完会重启进工厂流程、不再上报, 这段静默就是成功判据)。
 *   分界线就是 g_unbind.is_sensor; 详见 lora_business.h 的常量块。
 *
 * 0x05 帧【不进 data_cache】: 走 pending_func=0x05 直接发, 与正常下发隔离。
 *
 * 防重: g_unbind 同一时刻只跟踪一台设备, 已在解绑 → -3。
 *
 * 返回: 0=已启动; -1=uid 非法/找不到; -2=从机表无此设备; -3=已有解绑在进行中。 */
int lora_business_unbind(const char *uid_hex)
{
    if (uid_hex == NULL) return -1;

    uint8_t uid[CFG_UID_BYTES];
    if (!cfg_uid_to_bytes(uid_hex, uid)) return -1;

    uint8_t idx = lora_slave_find(uid);

    if (idx == 0xFFU)
    {
        /* 运行期 cfg.set 刚写入设备 -> 先按配置补登记 */
        lora_business_ensure_slaves();
        idx = lora_slave_find(uid);
        if (idx == 0xFFU) return -2;
    }

    /* 防重: g_unbind 已在跟踪(同一台或另一台) → 拒绝, 避免空口噪声 */
    if (g_unbind.state != UNBIND_IDLE)
    {
        log_info("[UNBIND] !! 已有解绑在进行中(state=%u, %02X%02X%02X), 拒绝重复发起\r\n",
                 (unsigned)g_unbind.state,
                 g_unbind.uid[0], g_unbind.uid[1], g_unbind.uid[2]);
        return -3;
    }

    lora_slave_t *s = &g_slave_list[idx];
    bool    is_sensor = (s->dev_type == SLAVE_TYPE_SENSOR);
    uint32_t now      = get_tick_ms();

    /* 1) 把目标 slave 置 WAIT_ACK+pending_func=0x05 当"锁":
     *    lora_downlink_logic 看到 pending_func==0x05 直接 continue 跳过(不干扰),
     *    handle_ack_from_device 顶部把 ack 路由到 lora_unbind_on_ack。 */
    s->pending_func     = LORA_FUNC_UNBIND;
    s->pending_data_len = 0U;          /* 0x05 帧无数据域 */
    s->retry_cnt        = 0U;
    s->state            = SLAVE_STATE_WAIT_ACK;
    s->send_req_tick    = now;

    /* 2) 启动 g_unbind 状态机(阶段1); last_send_tick 下面 _unbind_send_frame
     *    会再刷新一次, 这里先置 now 防止首次 3s 间隔检查立刻又发一帧。 */
    g_unbind.state            = UNBIND_PHASE1;
    memcpy(g_unbind.uid, uid, CFG_UID_BYTES);
    g_unbind.is_sensor        = is_sensor;
    g_unbind.slave_idx        = idx;
    g_unbind.phase_start_tick = now;
    g_unbind.last_send_tick   = now;
    g_unbind.phase2_retry     = 0U;

    log_info("[UNBIND] 向 %s(%02X%02X%02X) 发起 0x05 解绑, 进入阶段1(%s, type=%s)\r\n",
             uid_hex, uid[0], uid[1], uid[2],
             is_sensor ? "按上报周期, 只在它的接收窗口里发" : "30s, 每3s发",
             is_sensor ? "sensor" : "device");

    /* 3) 先发一帧试一下(可能正好赶上设备还醒着); 失败/没响应也没关系:
     *    声光靠 3s 墙钟重试, 液位等它下一次上报的窗口(见 lora_unbind_window_hit)。 */
    _unbind_send_frame();
    return 0;
}

bool lora_unbind_owns_ack(uint8_t slave_idx)
{
    return ((g_unbind.state != UNBIND_IDLE) && (slave_idx == g_unbind.slave_idx));
}
