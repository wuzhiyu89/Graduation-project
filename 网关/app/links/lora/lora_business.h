/**
 * \file  lora_business.h
 * \brief LoRa 业务层对外接口
 *
 * 分层（2026-10-18 拆分后，实现按职责分文件）:
 *   lora_proto.c     —— radio 收发 + 帧编解码 + CRC16 (协议层)
 *   lora_slave.c     —— 从机表（hex 工具 / 装载 / 增删 / 在线状态）
 *   lora_bind.c      —— 0x04 绑定窗口状态机
 *   lora_unbind.c    —— 0x05 解绑两阶段状态机
 *   lora_downlink.c  —— 下发队列（投递 / 空口发送 / ACK 重传 / MQTT 下行）
 *   lora_field.c     —— 出厂字段 0x06/0x07/0x08
 *   lora_business.c  —— 帧分发 / 0x02 上报 / 失联检测 / init / poll
 *   data_cache.c     —— 上行/下发缓存 (独立)
 *   g4_at.c          —— 4G MQTT 收发 (独立)
 *
 * 跨文件的内部接口在 lora_internal.h，不对外。
 *
 * 数据流:
 *   LoRa上行 → radio RxDone → lora_proto_poll → parse → on_frame(func分发)
 *     ├─ 0x02 sensor_report → data_cache_up_push + ACK(0x80)
 *     ├─ 0x80 ACK_OK        → data_cache_down_del_by_uid(state=IDLE)
 *     ├─ 0x81 ACK_ERR       → state=IDLE 等下次重试
 *     └─ 其他               → ...
 *
 *   MQTT下行 → g4_urc URC → lora_mqtt_recv_cb → 校验/解帧 → data_cache_down_push
 *   LoRa下发 → lora_business_poll → lora_reply_logic
 *               对每台 SLAVE_STATE_IDLE 从机: fetch_by_uid → lora_proto_send_frame → state=WAIT_ACK
 */
#ifndef __LORA_BUSINESS_H__
#define __LORA_BUSINESS_H__

#include <stdint.h>
#include <stdbool.h>
#include "lora_proto.h"
#include "data_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 可配置常量 ========== */
/* 【从机表是动态的】(2026-09-29 改造, P5): 从机表不再写死, 改为从 config_store
 * 的 sensors[]/devices[] 装载, 并支持运行期绑定/解绑(LoRa 0x04/0x05)。
 * - 运行期台数由 lora_business_get_slave_count() 返回(不再有编译期 LORA_SLAVE_CNT) */
#define LORA_ACK_TIMEOUT_MS     10000U  /* ACK 应答超时 ms */
#define LORA_MAX_RETRY          3U      /* 最大重传次数(耗尽后【丢弃该条下发缓存】, 由云端重新下发) */
#define LORA_POLL_PERIOD_MS     1000U   /* 下行重传/超时轮询周期 */
#define LORA_BIND_ACK_INTERVAL_MS   5000U   /* 绑定 ACK 重发间隔(用户口径 5s) */

/* ========== 解绑两阶段状态机参数(2026-09-30 用户最终口径) ==========
 * 【为什么这些常量放 .h】让上层(云端协议层)能查到解绑最坏耗时是多少,
 *   用于决定云端命令的重试/超时策略。
 *
 * 阶段1(默认 30s): 网关发 0x05 解绑帧, 每 3s 一次其余时间接收。
 *   - 收到设备 ack(0x80) → 立即停发, 等 15s 让设备清自己保存的网关值
 *   - 30s 没收到 ack → 上报云端 UNBIND_FAILED(设备可能离线)
 *
 * 等 15s(默认): 给设备时间把自己的网关值清成 FFFFFF。
 *   设备收到 0x05 后【立即回 ack 毫秒级】, 然后才清, 15s 余量足够。
 *
 * 阶段2(默认 10s, 最多循环 3 次): 再发 0x05, 每 3s 一次持续 10s。
 *   设备已清网关值=FFFFFF → 收到 0x05(源网关=旧≠FFFFFF)判定"不是发给我的" → 不回 ack
 *     → 收不到 ack = 解绑成功 → 删 config_store + 从机表 + 上报 UNBOUND
 *   设备没清(网关值还是旧的) → 收到 0x05 → 回 ack
 *     → 收到 ack = 设备没清 → 失败 → 循环阶段2 的 10s(不再等 15s)
 *     → 循环 3 次都收到 ack = 设备故障 → 上报 UNBIND_FAULT(从机记录保留不删)
 */
#define LORA_UNBIND_PHASE1_MS            30000U  /* 阶段1 持续 30s */
#define LORA_UNBIND_PHASE1_INTERVAL_MS    3000U  /* 阶段1 每 3s 发一次 0x05 */
#define LORA_UNBIND_WAIT_CLEAR_MS         15000U  /* 等 15s 让设备清网关值 */
#define LORA_UNBIND_PHASE2_MS            10000U  /* 阶段2 持续 10s */
#define LORA_UNBIND_PHASE2_INTERVAL_MS    3000U  /* 阶段2 每 3s 发一次 0x05 */
#define LORA_UNBIND_PHASE2_MAX_RETRY      3U      /* 阶段2 最多循环 3 次(都收到 ack = 设备故障) */

/* ========== 液位(休眠设备)专用的"窗口驱动"解绑参数(2026-11-03) ==========
 *
 * 【为什么液位要单独一套】
 *   上面那套 3s 墙钟节拍是按【常驻 RX 的声光】设计的。液位是休眠设备:
 *   只在每次 0x02 上报后开窗(5s 等 ACK + 10s 等指令), 其余时间在睡。
 *   盲发 0x05 绝大多数落在睡眠期, 30s 阶段1 里若无上报(默认 300s 周期 ⇒ 约 90%),
 *   10 帧全部打空 ⇒ 平台点一次解绑九成失败。
 *
 * 【窗口驱动的做法】
 *   · 解绑期间 slave 仍锁成 WAIT_ACK, 但【不按秒盲发】;
 *   · 收到该设备的 0x02 -> 网关照常回 0x80 -> 3s 后在它的"指令窗口"里发 0x05
 *     (复用 lora_reply_logic() 的两步时序, 见 lora_unbind_window_hit());
 *   · 阶段超时按【上报周期】算, 不再按固定秒。
 *
 * 【设备侧实测行为(2026-11-03 液位日志)】
 *   收到 0x05 -> 立刻回 ACK#1 -> 5s 后再回 ACK#2(第一帧大概率落在网关 794ms 的
 *   发射期内, 收不到) -> gw_addr 清成 13×0xFF -> 重启进工厂流程 -> 之后【不再上报】。
 *   所以判据是: 拿到 ACK(它一定会清) + ACK 后若干周期内不再响应 = 成功。
 *   清完的设备不会再来上报, 用"下一条上报的 gw 字段"去验证是行不通的。
 */
#define LORA_UNBIND_SENSOR_PHASE1_PERIODS  3U    /* 阶段1: 3 个上报周期内要拿到第一个 ACK */
#define LORA_UNBIND_SENSOR_PHASE2_PERIODS  2U    /* 阶段2: ACK 后 2 个周期内不再响应 = 已清 */
#define LORA_UNBIND_SENSOR_MAX_MS      3600000U  /* 兜底上限 1h, 防上报周期配得极大时卡死 */

/* ========== 从机状态 ========== */
typedef enum
{
    SLAVE_STATE_IDLE     = 0,   /* 空闲, 可以接收下发指令 */
    SLAVE_STATE_WAIT_ACK = 1,   /* 已发送下行, 等待 ACK */
} lora_slave_state_t;

/* ========== 从机固定配置(只读, 定义在 lora_business.c) ========== */
typedef struct
{
    uint8_t dev_uid[LORA_UID_BYTES];    /* 13 字节地址(类型 + 96 位芯片 UID), hex 串 26 字符 */
} lora_slave_cfg_t;

/* ========== LoRa 从机信息 ========== */
typedef struct
{
    uint8_t            dev_uid[LORA_UID_BYTES];       /* 设备唯一 UID (13B) */
    bool               is_online;        /* 是否在线(收到过帧) */
    uint32_t           last_recv_tick;   /* 上次收到上行帧的 tick ms */
    lora_slave_state_t state;
    uint8_t            retry_cnt;
    uint8_t            pending_func;     /* 下发帧的 func (超时重发时用) */
    uint8_t            pending_data[LORA_FRAME_DATA_MAX];
    uint8_t            pending_data_len;
    uint32_t           send_req_tick;     /* 发送时刻 tick */

    /* ---- 液位失联检测状态(2026-09-27 用户补充要求第 3 条) ----
     * 纯逻辑在 app/service/sensor_offline.c; 这里只放状态, 算法一行都不在这里。
     *
     * 【last_recv_tick 与 last_report_tick 是两回事, 千万别混用】:
     *   last_recv_tick   收到【任何帧】(含 0x80/0x81 ACK)都会刷新 —— 只说明"它还在说话";
     *   last_report_tick 只有 0x02 液位【上报】才刷新 —— 用户口径要的是"上次上报时间"。
     * 用 last_recv_tick 去判失联会把"只回 ACK 不上报"的设备永远算成在线。 */
    uint32_t           last_report_tick;  /* 上次 0x02 上报的 tick ms */
    bool               offline_reported;  /* 已上报过"失联"、且还没等到新的 0x02 上报 */
    uint32_t           offline_event_tick; /* 上次上报失联事件的 tick(每 3xperiod 重复) */

    /* ★2026-10-22 新增, 2026-10-29 作废★ 低电量事件的去重位。
     *   协议 V0.5 定稿"低电期间每次上报均重发、告警收敛归平台",
     *   固件不再去抖, 所以这个字段已无使用者, 保留位置只为不破坏结构体布局
     *   (从机表在 RAM 里, 没有持久化依赖)。下次重构可以删掉。 */
    bool               low_bat_reported;

} lora_slave_t;

/* ========== 液位传感器在线状态查询 ==========
 * 直接查从机表的 is_online（由 sensor_offline 状态机维护）。
 * 不在从机表里 ⇒ UNKNOWN（调用方自己决定怎么兜底）。 */
typedef enum
{
    LORA_DEV_STATE_UNKNOWN = 0,     /* 从机表里没有这台(可能刚解绑/还没绑) */
    LORA_DEV_STATE_ONLINE  = 1,     /* 在线 */
    LORA_DEV_STATE_OFFLINE = 2      /* 已判定离线 */
} lora_dev_state_t;

/**
 * \brief 按 uid(26 位 hex 串) 查这台设备在网关眼里是在线还是离线
 * \param uid_hex 26 位 hex 串(大写); NULL / 非法 -> UNKNOWN
 * \note  不阻塞、不碰网络, 只读从机表。
 */
lora_dev_state_t lora_business_device_state(const char *uid_hex);

/**
 * \brief 同上, 但直接给【字节形式】的 uid —— 给已经在遍历从机表的调用方用
 *        (例如 main.c 的 main_fill_status: 它手里就是 lora_slave_t.dev_uid)
 * \param uid 13 字节 uid; NULL -> UNKNOWN
 * \note  ★2026-10-18: status.report 的 sensorsOnline/devicesOnline 与本函数
 *        统一口径, 避免"dev.query 说离线、status.report 说在线"的自相矛盾。
 */
lora_dev_state_t lora_business_device_state_bytes(const uint8_t uid[LORA_UID_BYTES]);

/* ==========================================================================
 * ★★2026-10-18 新增: 出厂字段的写入 / 读取(只给产线/开发者, 与云端无关)★★
 * ==========================================================================
 * 字段 = 10 字节 ASCII(例 "YA26420001" / "WANGGUAN00"), 由产线在本机写进设备。
 * 用户口径:
 *   · 写(0x06): 每 5 秒发一次, 最多 3 次; 期间收到设备 0x80 就停止并判成功;
 *               3 次都没收到 -> 判失败。0x81 = 设备明确拒绝, 立即判失败。
 *   · 读(0x07): 网关【只发一次】, 然后等约 12 秒 —— 设备侧自己会重发 0x08
 *               两次(各带 5 秒窗口), 网关不必再重发 0x07。
 *   · 收到 0x08(带 10 字节字段) => 【必须回 0x80】, 并把字段交给结果回调。
 *   · 目标设备: 从机表里【唯一】那一台(产线一次只接一台);
 *               0 台或多台 -> begin 返回 false, 调用方负责提示先 #bind / 先解绑。
 * ⚠️ 这套东西【不入云端、不入 event_cache、不进 data_cache】—— 用户明确要求
 *    "只有我们开发者知道", 所以它只走空口 + 串口。
 * ========================================================================== */
#define LORA_FIELD_LEN   10U

/**
 * \brief 字段操作结果回调
 * \param ok       true = 写成功 / 读成功
 * \param field10  读成功时是设备回的 10 字节字段(本模块会补 '\0', 可当 C 串用);
 *                 写操作或失败时是空串
 */
typedef void (*lora_field_result_fn)(bool ok, const char *field10);

/** 注册字段操作结果回调; 传 NULL 取消 */
void lora_business_set_field_result_cb(lora_field_result_fn fn);

/**
 * \brief 开始"写字段": 目标 = 从机表里唯一那台设备
 * \param field10 必须【恰好 10 字节】, 字符集不校验(用户要求)
 * \return false = 参数不对 / 从机表不是正好 1 台 / 已有字段操作在进行
 * \note  返回 true 后立即发第一条 0x06; 之后由 lora_business_field_poll() 推进
 */
bool lora_business_field_write_begin(const char *field10);

/**
 * \brief 开始"读字段": 目标 = 从机表里唯一那台设备
 * \return false = 从机表不是正好 1 台 / 已有字段操作在进行
 */
bool lora_business_field_read_begin(void);

/**
 * \brief 推进字段操作(重发 / 超时判定)。主循环高频调用即可, 内部自己按 5s 节流。
 */
void lora_business_field_poll(void);

/** 取消进行中的字段操作(不发任何帧, 只清状态) */
void lora_business_field_cancel(void);

/* ========== 对外 API ========== */

/**
 * \brief 业务层初始化: 协议层 init + 注册 on_frame 回调 + 从机表清零 + data_cache init + MQTT 下行注册
 */
void lora_business_init(void);

/**
 * \brief 按当前 config_store 的 sensors[]/devices[] 【补登记】缺失的从机
 *
 * lora_slave_load() 只在启动时跑一次; 平台在运行期用 cfg.set 新增设备时,
 * 从机表不会自动更新 —— 之后发指令/解绑都会报"不在从机表"。
 * 本函数只做"加", 不清表、不改已有条目的状态(不掉 WAIT_ACK / 绑定窗口)。
 */
void lora_business_ensure_slaves(void);

/**
 * \brief 业务层轮询 (main while(1) 持续调):
 *          协议层 poll → 帧分发 → ACK 超时重传检测
 */
void lora_business_poll(void);

/**
 * \brief 主动发送应答帧 (0x80 OK / 0x81 错误)
 * \param gw_uid   网关地址 (本板的)
 * \param dev_uid  目标设备地址
 * \param func     要应答的原功能码
 * \param ok       true → 0x80 OK; false → 0x81 错误
 * \param err_code 错误码 (仅 ok=false 时有效)
 */
bool lora_business_send_ack(const uint8_t gw_uid[LORA_UID_BYTES], const uint8_t dev_uid[LORA_UID_BYTES],
                            uint8_t func, bool ok, uint8_t err_code);

/**
 * \brief 上层业务调用: 存入下发缓存, 不直接发送
 *        等待设备下次上报上行帧时, poll 会自动取出并发送
 * \param uid      目标从机 UID (13B)
 * \param func     功能码 (毕设版只允许 0x01 设置传感器参数)
 * \param data     载荷 (不含 CRC, 协议层会自动加)
 * \param data_len 载荷长度
 * \return 0 成功; -1 载荷过长; -2 找不到从机
 */
int lora_business_send_req(const uint8_t uid[LORA_UID_BYTES],
                           uint8_t func, const uint8_t *data, uint8_t data_len);

/* ==================== 液位 0x01 的默认参数(单一来源) ====================
 * 平台只下发上报周期, 其余项由网关填固定值。绑定成功后的"首次配置
 * 同步"与 sen.set/cfg.set 必须用同一组值, 所以只在这里定义, 不要各写一份。
 * ★2026-10-22: 协议取消"检测间隔"字段; "相同液位上报"固定为 1。
 * ⚠ 单位: 秒 / mm; 设备端按秒解析, 网关不换算。 */
#define LORA_SENSOR_REPORT_DEFAULT_S    300u    /* 上报间隔默认 300 秒(与绑定默认一致) */
#define LORA_SENSOR_ALARM_DEFAULT_MM    50u     /* 液位告警阈值 50 mm = 5 cm(云端不下发) */

/**
 * \brief 平台 JSON(sen.set) 改液位参数 —— 网关负责组 0x01 帧并投递
 * \param uid_hex          13 字节 UID 的 26 位 hex 串(该传感器必须已在从机表里)
 * \param report_period_s  上报间隔, 单位【秒】(= 平台 sen.set 的 reportPeriodSeconds)
 * \param alarm_thresh_mm  液位告警阈值 mm
 * \return true  = 已投入下发缓存(等传感器下次上报时搭车下发)
 *         false = uid 非法 / 间隔越界 / 投递失败(原因已打日志)
 *
 * ★【单位是秒】—— 2026-10-05 用户口径 ★
 *   平台侧配置(reportPeriodSeconds)本来就是秒, 所以【网关不做任何换算】,
 *   平台给什么就原样填进帧里; 设备端按秒解析(设备侧由用户对接)。
 *   ⚠ 这条与 2026-09 的"分钟"版本【不兼容】: 旧版网关原样转发分钟值、设备端自己
 *     ×60 换成秒。现在两端都按秒, 谁都不换算 —— 网关/设备必须同一次改版。
 *
 * ★2026-10-22 载荷改版★ 0x01 共 18 字节:
 *   [0-2] 上报间隔(小端 24 位) [3] 相同液位上报(恒 1)
 *   [4-5] 告警阈值 mm(小端)    [6] 测量模式(0 液位 1 距离)
 *   [7-8] 安装高度 cm(小端)    [9-10] 校准偏移 mm(int16 小端)
 *   [11] 低电量阈值 %(0=禁用)  [12-17] 年 月 日 周 时 分
 *   取消了原 [0-1] 的检测间隔; 星期由日期算出, 不再固定周五。
 *   ⚠️ 测量模式/安装高度/校准偏移/低电量阈值由【液位设备自己应用】:
 *      本函数从已落盘的 config_store 里取这几个值一起下发,
 *      平台通过 cfg.set.sensors[] 改了它们即可, 调用方无需逐个透传。
 *
 * \note 与"云端直接下发 0x01 原始帧"走的是【同一段实现】(_sensor_params_commit):
 *       范围检查 / 补 6 字节校时 / 日志完全一致,
 *       不会出现"两条下行的规则不一样"这种最难查的问题。
 *       两条路都是【异步】的: 只入队, 真正发送在传感器下次上报的应答窗口里。
 */
bool lora_business_set_sensor_params(const char *uid_hex,
                                     uint32_t report_period_s,
                                     uint16_t alarm_thresh_mm);

/**
 * \brief 获取从机列表指针 (业务层可遍历, 长度 = lora_business_get_slave_count())
 */
lora_slave_t *lora_business_get_slave_list(void);

/**
 * \brief 取运行期从机表的实际台数(动态, 绑定/解绑会变)
 * \note  遍历 lora_business_get_slave_list() 时用它当上界, 不要写死数字。
 */
uint8_t lora_business_get_slave_count(void);

/* ==================== 液位失联检测(2026-09-27 用户补充要求第 3 条) ====================
 *
 * 用户原话:
 *   "液位传感器最多三个；不用断言还在，只用记录上报时间；每次一个液位上报数据，
 *    就把所有液位从机的上次上报时间拿出来对比，如果有很久没上报的（比如 3 倍
 *    上报周期没上报），就向云端发送传感器失联。"
 *
 * 【判定与去重的纯逻辑在 app/service/sensor_offline.c】, 本模块只做两件事:
 *   1) 维护每个液位从机的 last_report_tick / offline_reported;
 *   2) 到"该报"的时候调用注册进来的回调(真正的组 JSON + 发 MQTT 在 main.c)。
 *
 * 【为什么把"发"这一步交给主程序】: 本模块一直不依赖云协议层/4G 的 JSON 发布
 * (它只做 LoRa 空口与缓存), 保持这个边界, 主机测试也就不用为它造 4G 桩。
 * 风格与 lora_business_set_dl_prefilter() 的注入钩子一致。
 */

/**
 * \brief 传感器事件回调(失联/恢复)。由主程序注册, 负责组 event.device 并发到 event topic
 * \param uid_hex 26 位 hex 串（13 字节）(大写, 例 "21000000000000000000000001")
 * \param online  true = 恢复上报(对应文档 event 值 "online");
 *                false = 失联(对应 "offline")
 * \note  回调里【不要阻塞】(发布本身是非阻塞登记队列)。不注册时只打日志, 不影响主循环。
 */
typedef void (*lora_sensor_event_fn)(const char *uid_hex, bool online);

/**
 * \brief 注册失联/恢复事件回调; 传 NULL 取消
 */
void lora_business_set_sensor_event_cb(lora_sensor_event_fn fn);

/* ==========================================================================
 * ★2026-10-22 新增: 液位低电量事件回调(对应平台 lowBatteryPercent)
 * ==========================================================================
 * \param uid_hex 26 位 hex 串
 * \param bat_pct 触发时的电量百分比
 *
 * 【触发规则】(协议 V0.5 §7.4 / 配置 §3.3, 2026-10-29 定稿)
 *   · 阈值来自 config_store 的 sensors[].low_battery_pct; 0 = 禁用;
 *   · 每次 0x02 上报, 只要 bat_pct <= 阈值就触发 —— **不去抖、不看沿**;
 *     低电期间每包都报, 告警收敛(聚合/静默)由平台侧负责;
 *   · 网关离线时同一台设备只保留最新一条(见 event_cache_push_latest),
 *     那是缓存容量策略, 不影响在线时的"每包都报"。
 * 【为什么在 LoRa 层判】电量只出现在 0x02 帧里, 判定就地做最省事。
 * \note 回调里【不要阻塞】; 不注册时只打日志。
 */
typedef void (*lora_sensor_low_bat_fn)(const char *uid_hex, uint8_t bat_pct);

void lora_business_set_sensor_low_bat_cb(lora_sensor_low_bat_fn fn);

/* ==========================================================================
 * ★★2026-10-18 新增: 每次液位上报(0x02)的原始数据回调★★
 * ==========================================================================
 * \param uid      该设备的 13 字节 uid(不是 hex 串 —— 调用方常常直接要字节)
 * \param level_mm 本次上报的水位(mm, 设备原值, 网关不换算)
 * \param bat_pct  电量百分比
 * \param rssi_dbm 该帧的 LoRa RSSI
 *
 * 【为什么需要它】离线期间的水位数据以前是【直接丢掉的】:
 *   main_telemetry_step() 一开头就 `if (!g4_at_is_online()) return;`, 断线期间
 *   根本不生成遥测。而 LoRa 与 4G 独立, 液位照常上报 ⇒ 这些数据白丢。
 *   现在业务层每收到一条有效 0x02 就回调一次, 主程序在【离线时】把这条存进
 *   telemetry_cache, 重连后按原时间戳逐条补报。
 *
 * 【调用时机】在 handle_sensor_report() 里, 长度校验通过之后(残帧不算上报),
 *   与 data_cache_up_push() 相邻 —— 那条是 last_value 快照的漏斗, 这条是补报的漏斗。
 *
 * \note 回调里【不要阻塞】。不注册时什么也不做(不影响主循环)。
 */
typedef void (*lora_level_report_fn)(const uint8_t uid[LORA_UID_BYTES],
                                     uint16_t level_mm, uint8_t bat_pct,
                                     int8_t rssi_dbm);

/** 注册"每次液位上报"回调; 传 NULL 取消 */
void lora_business_set_level_report_cb(lora_level_report_fn fn);

/**
 * \brief 检查一遍全部液位从机是否失联, 该报的就调回调
 *
 * 【触发时机(用户口径)】: 每收到一路液位 0x02 上报后, 由 handle_sensor_report()
 * 调用一次 —— 不另起定时器。它运行在主循环调用的路径上(lora_proto_poll →
 * on_frame → handle_sensor_report), 【不在中断上下文】里做这些事。
 *
 * ⚠ 已知边界(如实写在这里, 不要在别处猜): 既然检查只由"收到某一路上报"驱动,
 *   当从机表里【只有一台】液位传感器时, 它一旦彻底不报, 就没有任何一帧能触发检查,
 *   失联事件不会发出。用户口径本身就是这个取舍(多台互为参照)。当前从机表里
 *   只绑了【一台】液位传感器时就是这种情形, 所以现场要真正用上这条, 需要
 *   (a) 表里配 2~3 台液位, 或 (b) 以后把本函数也挂到主循环的定时路径上。
 *   本函数是【可以直接重复调用】的(幂等), 挂到别处不会重复报。
 */
void lora_business_check_sensor_offline(void);

/* ==================== 下行前置过滤器(P1-3 过渡期) ====================
 *
 * 【为什么需要】同一条 MQTT 下行通道上现在要同时容纳两种协议:
 *   ① 云端 JSON 命令(RoadTwin 云协议层, 见 cloud_proto)
 *   ② 原有的二进制 LoRa 帧透传(本模块)
 * 本模块的下行回调 lora_mqtt_recv_cb() 是 static, 外部无法直接接管,
 * 所以开一个过滤器钩子: 主程序注册它, 返回 true = "这条我已经处理了"
 * (例如是 JSON 命令并且已经应答), 本模块就【不再】做 LoRa 透传。
 *
 * 默认(不注册)行为完全不变。
 */
typedef bool (*lora_dl_prefilter_t)(const char *topic, const uint8_t *data, uint16_t len);

/**
 * \brief 注册下行前置过滤器
 * \param fn 过滤器; 传 NULL 取消
 */
void lora_business_set_dl_prefilter(lora_dl_prefilter_t fn);

/* ==================== 设备绑定/解绑(P5, LoRa 0x04/0x05) ====================
 *
 * 【绑定流程】(用户 2026-09-29):
 *   云端下发 dev.bind{serial,name,pairWindowSeconds} → 经 cloud_proto 的 pair_hook
 *   调 lora_business_open_bind_window() 打开 N 秒窗口 → 期间收到设备广播 0x04 帧
 *   (目标地址=FFFFFF) → 网关每 5s 回一次 0x80 ACK → 设备回 0x80 确认 → 绑定成功,
 *   写 config_store + 触发事件回调(bound); 窗口到期未绑 → 触发 bind_failed。
 *
 * 【解绑流程】(用户 2026-09-30 两阶段最终口径):
 *   云端下发 dev.unbind{uid} → 经 cloud_proto 调 lora_business_unbind() →
 *   网关进入两阶段状态机:
 *     阶段1(30s): 每 3s 发一次 0x05, 其余时间接收。
 *       - 收到设备 ack(0x80) → 立即停发, 等 15s 让设备清网关值
 *       - 30s 没收到 ack → 触发 UNBIND_FAILED(从机记录保留, 设备可能离线)
 *     等15s: 给设备时间清自己保存的网关值成 FFFFFF
 *     阶段2(10s, 最多循环3次): 每 3s 发一次 0x05 持续 10s。
 *       设备已清网关值 → 收到 0x05(源网关≠FFFFFF)判定不是发给自己 → 不回 ack
 *         → 10s 没收到 ack = 解绑成功 → 删 config_store + 从机表 + 触发 UNBOUND
 *       设备没清(网关值还是旧的) → 回 ack
 *         → 收到 ack = 失败 → 循环阶段2 的 10s(不再等 15s)
 *         → 循环 3 次都收到 ack = 设备故障 → 触发 UNBIND_FAULT(从机记录保留不删)
 *
 * 【事件回调】由主程序注册, 在回调里组 event.device JSON 并发到 event topic。
 */
/** 绑定/解绑事件类型 */
typedef enum
{
    LORA_BIND_EVENT_BOUND         = 0,  /* 绑定成功 */
    LORA_BIND_EVENT_BIND_FAILED   = 1,  /* 窗口到期未绑成 */
    LORA_BIND_EVENT_UNBOUND       = 2,  /* 解绑成功(删 config_store + 从机表) */
    LORA_BIND_EVENT_UNBIND_FAILED = 3,  /* 解绑失败: 阶段1 30s 没收到 ack(设备可能离线), 从机记录保留 */
    LORA_BIND_EVENT_UNBIND_FAULT  = 4,  /* 解绑故障: 阶段2 循环3次都收到 ack(设备没清网关值), 从机记录保留 */
} lora_bind_event_t;

/**
 * \brief 绑定/解绑事件回调签名
 * \param uid_hex 26 位 hex 串（13 字节）(绑定时是被绑设备的 uid; 解绑时是被删设备的 uid)
 * \param event 事件类型
 * \note  回调里【不要阻塞】(发布应是非阻塞登记)。不注册时只打日志。
 */
typedef void (*lora_bind_event_fn)(const char *uid_hex, lora_bind_event_t event);

/** 注册绑定/解绑事件回调; 传 NULL 取消 */
void lora_business_set_bind_event_cb(lora_bind_event_fn fn);

/**
 * \brief 打开绑定窗口(pair_hook 的实现, 由 main.c 注册给 cloud_proto)
 * \param serial  云端 sen.bind 携带的 serial(本工程不启用, 仅作元数据存 config_store)
 * \param name    云端 sen.bind 携带的 name(同上)
 * \param window_s 窗口秒数(文档默认 60)
 * \return true 成功打开; false 参数非法或已有窗口进行中
 *
 * ★★ 一个窗口只能配【一台】设备(2026-09-30 用户明确定下) ★★
 *   原因: 子设备的 0x04 配对请求帧里【只有 13 字节 UID, 没有 serial】——
 *   网关无法拿云端下发的 serial 去校验"来的到底是不是平台要的那台"。
 *   所以规则就是: 窗口打开期间, **第一个发 0x04 的设备被绑定**, 窗口随即结束;
 *   平台侧【一个窗口只申请一台】, 要配两台就开两次窗口。
 *   ⚠️ 若同时有多台设备在配对模式(都在 5s 循环发 0x04), 绑到的是先到的那台,
 *      与平台填的 serial/name 可能对不上 —— 这是协议本身的限制, 不是实现缺陷。
 *
 * \note  与 cloud_pair_open_fn 签名一致, 直接作为 pair_hook 注册
 */
bool lora_business_open_bind_window(const char *serial, const char *name,
                                     uint16_t window_s);

/**
 * \brief 发起解绑(由 cloud_proto 的 sen.unbind 调用) —— 启动两阶段状态机
 * \param uid_hex 26 位 hex 串（13 字节）, 必须已在从机表里
 * \return 0 成功启动解绑状态机(异步, 真正删除在阶段2 成功后);
 *         -1 uid 非法; -2 不在从机表; -3 已有解绑在进行
 * \note  立即返回, 不等 ACK。结果经事件回调上报(UNBOUND/UNBIND_FAILED/UNBIND_FAULT)。
 *        状态机由 lora_business_poll() 驱动, 最坏耗时约
 *        PHASE1(30s) + WAIT_CLEAR(15s) + PHASE2*3(10s*3) = 75s。
 */
int lora_business_unbind(const char *uid_hex);

/* ==================== LoRa 射频健康状态 ====================
 *
 * 【为什么需要】: LLCC68 的 BUSY(PB3) 只要一直是高, 射频层每条 SPI 命令都会卡在
 * 等 BUSY —— 原来那里是死等, 结果连 4G 的初始化都走不到(见 LLCC68N32H482-board.c
 * 里 LLCC68WaitOnBusy 的注释)。现在超时后会置"模组不应答"标志, 用下面两个函数
 * 就能在状态行里一眼看出 LoRa 到底通没通, 不用去数 "wait busy pin timeout"。
 */
/**
 * \brief LoRa 模组是否已被判定为"不应答"(BUSY 一直高)
 * \return 1 = 不应答(查供电/EN/接线); 0 = 正常或还没判过
 */
uint8_t lora_business_radio_stuck(void);

/**
 * \brief 当前 BUSY(PB3) 电平
 * \return 1 = 还处于 busy(正常通信时应该几乎总是 0)
 */
uint8_t lora_business_radio_busy(void);

/* ==================== 云端 MQTT 上下行 ====================
 *
 * ★★2026-10-28 重大更正: "MQTT 载荷 = 协议帧 hex 文本"这套做法【已废止】★★
 *   现状(以代码为准):
 *     · 下行: 云端发 JSON 命令(见 cloud_proto 的命令表), 由 app_downlink 解析;
 *             若平台仍按旧约定下发【hex 文本帧】, 网关【不接收】——
 *             它不以 '{' 开头, 会被当非法下行丢弃, 也不会应答。
 *     · 上行: app_uplink 组 JSON(status / telemetry / event 等 7 类),
 *             早就不是协议帧 hex。
 *     · 二进制协议帧只在【网关 ↔ LoRa 从机】之间用, 不经 MQTT。
 *
 * ---- 以下为历史记录(2026-10 之前的做法), 仅供追溯, 不要按它对接 ----
 *   MQTT 载荷 = 一整个协议帧的 ASCII 十六进制文本, 例:
 *     AA5501110001210001020702005C08180A3A2ECB
 *   这是 20 字节帧, 注意它是【3 字节 UID 时代】的格式, 现在同样的内容要 40 字节:
 *     AA 55 | 01 | 11 00 01 | 21 00 01 | 02 | 07 | 02 00 5C 08 18 0A 3A | 2E CB
 *     └头┘  ver  └─ 网关 ─┘  └─ 设备 ─┘  fc  dlen  └── data 7B ──┘       └CRC┘
 * 网关解 hex 后按 lora_proto_parse_raw() 严格校验(帧头/版本/长度/CRC),
 * 再按功能码:
 *   0x01(设置液位参数): 云端 data 只有 12 字节【没有校时】,
 *        [0-2] 上报间隔(小端 24 位) [3] 相同液位(网关固定按 1 发) [4-5] 阈值 mm;
 *        [6] 测量模式 [7-8] 安装高度 cm [9-10] 校准偏移 mm [11] 低电量阈值%;
 *        网关补上 年 月 日 周 时 分(时间取自 AT+CCLK?, 星期按日期算真实值)后
 *        以 18 字节下发。
 *        ★2026-10-05 改版: 间隔字段单位 = 【秒】★ —— 云端发秒, 网关【原样转发
 *        不换算】, 设备端按秒直接用(设备侧由用户对接)。
 *        旧版是"分钟"(网关原样转发分钟, 设备端自己 ×60 换内部秒)——
 *        两版【不兼容】, 网关与设备必须同一次改版, 否则时间差 60 倍。
 *        另注: 现在取不到 4G 时间【不再直接丢弃】, 而是校时先填 0 并告警 ——
 *        真正发出去之前 lora_try_send_downlink() 会再取一次刷新(见 .c 的说明)。
 *   0x03(设置声光报警器): 一个字节都不改, 直接透传。
 *   0x80/0x81: 云端对我方上报的应答, 只记日志。
 *
 * 【上行】★2026-10-08 变更★
 *   原来这里有两条并行的上行:
 *     ① `lora_business_publish_report()` —— 0x02 整帧转 hex, 发到 MQTT 默认主题
 *        `mod/mod_upload`;
 *     ② `last_value` 快照 → main.c 组 JSON → `rt/gw/{gwId}/telemetry`。
 *   甲方要求不再使用 `mod/mod_upload`, 所以 **① 已整体删除**(连同它的
 *   `data_cache_poll()` 调用与 `G4_MQTT_PUB_TOPIC` 宏)。
 *   现在上行只有 ② 这一条: 0x02 → `data_cache_up_push()` → `last_value` →
 *   main.c 的 `main_publish_telemetry_json()` → `rt/gw/{gwId}/telemetry`。
 *   ⚠️ 所以 `data_cache_up_push()` 【不能删】—— 它是那条唯一上行路径的漏斗。
 */

#ifdef __cplusplus
}
#endif

#endif /* __LORA_BUSINESS_H__ */
