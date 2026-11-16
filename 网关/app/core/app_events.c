/**
 * @file  app_events.c
 * @brief 事件类上行：event.device（毕设版只有液位 sensor 事件）
 *
 * 与 app_uplink.c 的分工：
 *   app_uplink.c —— 周期性 status.report 与 telemetry.report（含逐条补报）
 *   app_events.c —— 液位状态跳变事件（失联/恢复/绑定/低电量）
 *   event.action / event.status_change 联动事件已随一体机删除
 *
 * 事件的上行策略（文档 7.x）：能发就直发；4G 不在线或发布登记失败就写进
 * event_cache，重连后由 app_backfill_poll() 按原始时间戳补报（不改成重连时刻）。
 */
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "app_config.h"
#include "app_core.h"
#include "app_state.h"
#include "bsp_tim.h"
#include "cloud_profile.h"
#include "cloud_proto.h"
#include "config_store.h"
#include "event_cache.h"
#include "g4_at.h"
#include "lora_business.h"
#include "sensor_offline.h"
#include "utils.h"

/**
 * @brief 组一条 event.device(文档 7.4) 发到 event topic —— 液位失联/恢复
 *
 * 文档 7.4 原文:
 *   { "event": "event.device", "msgId": "v001", "ts": 1735689600,
 *     "data": { "addr": 1, "devType": "sensor", "event": "offline" } }
 *   event 取值: bound / bind_failed / unbound / online / offline / low_voltage。
 *
 * 【字段口径】: 本工程子设备寻址是 13 字节 UID 的 26 位 hex 串(偏差 D-8), 文档那个
 * uint16 的 "addr" 装不下, 所以 data 里用 "uid":"21360611303239363131017C42";
 * "devType" 与 "event" 两个字段名与取值【严格照文档】。
 * (历史上这里曾用 3 字节地址 "210001", 2026-10-18 起全面换成 26 位 hex, 旧格式已废弃。)
 *
 * 【这个函数是被 lora_business 回调进来的】: 判定与去重都在
 * lora_business.c + sensor_offline.c 里, 这里只管"组包 + 登记发布"。
 *
 * 【弱网】: 发布登记失败只打日志 —— g4_at_mqtt_publish_ex() 本身是非阻塞登记队列,
 * 而且事件不像配置应答那样必须让平台知道, 绝不能因为它去阻塞主循环。
 *
 * 4G 不在线时【入事件缓存】(EC_KIND_DEVICE) 等重连补报, 按原始时间戳补发。
 * 毕设版只有液位 sensor 事件; 声光/联动的 device 事件已删除。
 */
/* ★ 第 12 条(2026-09-30): 事件 msgId 的【唯一序号】—— 在线发送与"断线入缓存"
 * 两条路【共用同一个计数器】。
 * 【为什么必须共用】以前两边各有一个 static seq: 同一个事件在线时叫 `s001`,
 * 断线入缓存、之后补报时被重新分配成 `b001` —— 平台按 msgId 做幂等/去重时对不上。
 * ★2026-10-18: 定义上移到这里(uplink_publish_device_event 也要用它)。 */
static uint16_t s_evt_seq;

/* 设备事件码 -> 线上字符串(与 cloud_proto.h 的 cloud_device_event_t.event 一致) */
static const char *s_dev_ev_name(uint8_t ev)
{
    switch (ev)
    {
        case (uint8_t)EC_DEV_EV_BOUND:       return "bound";
        case (uint8_t)EC_DEV_EV_BIND_FAILED: return "bind_failed";
        case (uint8_t)EC_DEV_EV_UNBOUND:     return "unbound";
        case (uint8_t)EC_DEV_EV_ONLINE:      return "online";
        case (uint8_t)EC_DEV_EV_OFFLINE:     return "offline";
        case (uint8_t)EC_DEV_EV_LOW_VOLTAGE: return "low_voltage";
        default:                             return "unknown";
    }
}

/* ★2026-10-22: online(bool) 改成事件码(ev), 以支持 low_voltage。
 *   事件码与缓存里 rec.a 用的是同一套 EC_DEV_EV_*, 在线/补报两条路不会分叉。 */
static void uplink_publish_device_event(const char *uid_hex, uint8_t ev,
                                      const char *dev_type)
{
    /* 一律 static: 栈只有 4KB(见 startup_n32h47x_48x.s 的 Stack_Size) */
    static char     data[96];
    static char     payload[CLOUD_RESP_MAX];
    static char     topic[64];
    static uint16_t seq;
    char            msg_id[20];
    cjw_t           w;
    uint16_t        plen;
    uint16_t        tlen;
    const char     *dt = ((dev_type != NULL) && (dev_type[0] != '\0'))
                             ? dev_type : "sensor";
    bool            is_sensor = (strcmp(dt, "sensor") == 0);

    /* ---- 4G 不在线: 写进事件缓存, 重连后由补报队列发出去 ---- */
    if (!g4_at_is_online())
    {
        ec_record_t     rec;
        uint8_t         uid_b[CFG_UID_BYTES];
        ec_push_result_t pushed;

        if (!cfg_uid_to_bytes(uid_hex, uid_b))
        {
            log_info("[CLOUD] !! %s 不是合法 uid, 设备事件无法入缓存\r\n",
                     (uid_hex != NULL) ? uid_hex : "(null)");
            return;
        }

        (void)memset(&rec, 0, sizeof(rec));
        rec.kind = (uint8_t)EC_KIND_DEVICE;
        (void)memcpy(rec.uid, uid_b, CFG_UID_BYTES);
        rec.a    = ev;
        rec.b    = is_sensor ? (uint8_t)EC_DEV_SENSOR : (uint8_t)EC_DEV_DEVICE;
        rec.ts   = app_now_unix();
        rec.seq  = 0U;                          /* 设备事件不按 seq 分组 */
        s_evt_seq = (uint16_t)((s_evt_seq + 1u) % 1000u);
        rec.msg_seq = s_evt_seq;                /* 与其它事件共用同一个序号计数器 */

        /* ★2026-10-28: 低电量走"同一设备只留最新一条"的入库方式★
         *   低电量是持续状态, 不是一次动作: 断线期间若同一台设备反复越限
         *   (充电/换电池/阈值被调高又调低), 逐条留住只会挤占补报窗口。
         *   其它事件码(bound/unbound/online/offline)仍逐条保留 —— 它们
         *   必须成对、成序, 合并会让平台看不出"掉线又恢复"。 */
        /* ★2026-11-02 (G2): offline 也走"同一设备只留最新一条"★
         *   离线事件按设计每 3×上报周期重复一次; 断线期间逐条入库的话,
         *   一次长断网就能用同一条 offline 把 256 条缓存塞满, 重连后平台
         *   收到几十条一模一样的补报(G2 现场问题)。现在只保留最新那一条,
         *   恢复后照常补一条 online; 在线时的重复 offline 仍逐条直发。 */
        pushed = ((ev == (uint8_t)EC_DEV_EV_LOW_VOLTAGE) ||
                  (ev == (uint8_t)EC_DEV_EV_OFFLINE))
                     ? event_cache_push_latest(&rec)
                     : (event_cache_push(&rec) ? EC_PUSH_ADDED : EC_PUSH_FAIL);

        if (pushed == EC_PUSH_MERGED)
        {
            log_info("[CLOUD] 4G 不在线 -> 设备事件(uid=%s type=%s %s)入补报缓存: "
                     "同一设备已有同类记录, 就地刷新为最新(不新增)\r\n",
                     (uid_hex != NULL) ? uid_hex : "?", dt, s_dev_ev_name(ev));
        }
        else if (pushed == EC_PUSH_ADDED)
        {
            log_info("[CLOUD] 4G 不在线 -> 设备事件(uid=%s type=%s %s)入补报缓存\r\n",
                     (uid_hex != NULL) ? uid_hex : "?", dt, s_dev_ev_name(ev));
        }
        else
        {
            log_info("[CLOUD] !! 设备事件(event_cache_push 拒绝) -> 这条丢了\r\n");
        }
        return;
    }

    cjw_init(&w, data, (uint16_t)sizeof(data));
    cjw_obj_begin(&w);
    cjw_kv_str(&w, "uid", (uid_hex != NULL) ? uid_hex : "");
    cjw_kv_str(&w, "devType", dt);
    cjw_kv_str(&w, "event", s_dev_ev_name(ev));
    cjw_obj_end(&w);
    if (!cjw_ok(&w))
    {
        log_info("[CLOUD] !! event.device 的 data 段组装失败(缓冲不足?)\r\n");
        return;
    }
    (void)cjw_len(&w);      /* v91: 本函数不再需要长度(信封自带) —— 原来算给 dlen 但没用到 */

    seq++;
    (void)utils_gen_msg_id_seq(msg_id, (uint16_t)sizeof(msg_id), (uint8_t)'v',
                              (uint16_t)(seq % 1000u));

    plen = cloud_proto_build_uplink(payload, (uint16_t)sizeof(payload),
                                    "event.device", msg_id, app_now_unix(), data);
    if (plen == 0u)
    {
        log_info("[CLOUD] !! event.device 信封组装失败\r\n");
        return;
    }

    tlen = cloud_profile_build_topic(CLOUD_TOPIC_EVENT, topic, (uint16_t)sizeof(topic));
    if (tlen == 0u)
    {
        log_info("[CLOUD] !! event topic 拼装失败, 事件丢弃\r\n");
        return;
    }

    /* 发不出去也要把正文打出来: 弱网下串口是唯一能确认"我们准备发什么"的地方
     * (与 app_publish_status_json / app_downlink_try 同一套做法) */
    log_info("[CLOUD] 上报 %s (%u 字节): %s\r\n", topic, (unsigned)plen, payload);

    if (g4_at_mqtt_publish_ex(topic, (const uint8_t *)payload, plen,
                              cloud_profile_topic_qos(CLOUD_TOPIC_EVENT),
                              cloud_profile_topic_retained(CLOUD_TOPIC_EVENT)) != SYS_OK)
    {
        log_info("[CLOUD] !! 事件发布登记失败(离线/队列满), 这条事件只记在本机\r\n");
    }
}

/**
 * @brief 液位失联/恢复的【适配器】—— 固定 devType = "sensor"
 *
 * ★2026-10-18 (丙): uplink_publish_device_event 增加了 devType 参数, 而
 * lora_business 的传感器回调签名只有 (uid, online) ⇒ 用这一层适配。
 * 声光那边直接调 uplink_publish_device_event(..., "device"), 不走这里。
 */
void app_on_sensor_event(const char *uid_hex, bool online)
{
    uplink_publish_device_event(uid_hex,
                                online ? (uint8_t)EC_DEV_EV_ONLINE
                                       : (uint8_t)EC_DEV_EV_OFFLINE,
                                "sensor");
}

/**
 * @brief 液位低电量(适配器, 固定 devType="sensor")
 *
 * ★2026-10-22 新增: 平台 lowBatteryPercent 由网关判定, 越限上报
 *   event.device{"event":"low_voltage"}。判定在 lora_business.c
 *   (★2026-10-29: 已按协议 V0.5 去掉越限沿去抖, 低电期间每包都报),
 *   本函数只负责组包 —— 与失联事件同一条路径, 断线会入补报缓存
 *   (断线时由 event_cache_push_latest 合并成"每台只留最新一条")。
 */
void app_on_sensor_low_battery(const char *uid_hex, uint8_t bat_pct)
{
    log_info("[CLOUD] 液位 %s 低电量(%u%%), 上报 event.device low_voltage\r\n",
             (uid_hex != NULL) ? uid_hex : "?", (unsigned)bat_pct);
    uplink_publish_device_event(uid_hex, (uint8_t)EC_DEV_EV_LOW_VOLTAGE, "sensor");
}

/**
 * @brief 出厂字段操作的结果(写成功 / 读成功 / 失败) —— 只打串口, 【不上云】
 *
 * ★2026-10-18: 字段是产线/开发者的内部信息(用户明确"不要让云端知道"),
 *   所以这里既不组 JSON、也不进 event_cache, 只落一行日志。
 */
void app_on_field_result(bool ok, const char *field10)
{
    if (!ok)
    {
        log_info("[FIELD] ==> 结果: 失败"
                 "(写: 3 条都没收到 0x80 / 读: 12s 内没收到 0x08)\r\n");
        return;
    }

    if ((field10 != NULL) && (field10[0] != 0x00))
    {
        log_info("[FIELD] ==> 结果: 读取成功, 设备字段 = \"%s\"\r\n", field10);
    }
    else
    {
        log_info("[FIELD] ==> 结果: 写入成功(设备已回 0x80)\r\n");
    }
}

/* ==================================================================
 * event.device: bound / bind_failed / unbound(P5, 设备绑定/解绑)
 * ==================================================================
 * 与上面的 uplink_publish_device_event(online/offline)【同 topic 同 data 结构】,
 * 只是 event 字段不同。由 lora_business_set_bind_event_cb 注册给业务层,
 * 在 0x04 绑定成功/失败、0x05 解绑完成时各调一次。
 *
 * 异步: 0x04/0x05 完成后才触发本回调, 那时云端命令响应(sen.bind/dev.unbind 的
 * 应答 code 0)早就发回去了 —— 云端靠这条事件确认"真绑/真删"完成。
 *
 * 与 uplink_publish_device_event 一样, 静态缓冲 + 发布失败只记本机不阻塞。
 * ★ 不进 event_cache(断线补报队列): 绑定/解绑是即时事件, 平台收不到时会再下发
 *   命令重试, 不需要补报(补报反而可能让平台收到"重复 bound"造成误判)。
 *
 * 【2026-09-29 补回】本函数与下面那两行注册在 14:06 的一次改动里被误删,
 *   导致 sen.bind/dev.bind 恒回 1006(pair_hook 为空)、解绑完成事件也发不出去。
 *   从 v105 归档原样取回。 */
void app_publish_bind_event(const char *uid_hex, lora_bind_event_t event)
{
    static char     data[160];      /* ★2026-11-02: 96 -> 160, 要装下 type 字段 */
    static char     payload[CLOUD_RESP_MAX];
    static char     topic[64];
    char            msg_id[20];
    cjw_t           w;
    uint16_t        plen;
    uint16_t        tlen;
    const char     *ev_str;

    ev_str =
        (event == LORA_BIND_EVENT_BOUND)          ? "bound"          :
        (event == LORA_BIND_EVENT_BIND_FAILED)    ? "bind_failed"    :
        (event == LORA_BIND_EVENT_UNBOUND)        ? "unbound"        :
        (event == LORA_BIND_EVENT_UNBIND_FAILED)  ? "unbind_failed"  :
        (event == LORA_BIND_EVENT_UNBIND_FAULT)   ? "unbind_fault"   : "unknown";

    cjw_init(&w, data, (uint16_t)sizeof(data));
    cjw_obj_begin(&w);
    cjw_kv_str(&w, "uid",     (uid_hex != NULL) ? uid_hex : "");
    /* 毕设版只有液位传感器: devType 固定 sensor, type 固定 1(液位) */
    cjw_kv_str(&w, "devType", "sensor");
    /* ★2026-11-02(G6): 只有 bound 带数字 type ★
     *   平台在 bound 那一刻要把台账写出来, 这是它唯一需要 type 的时机;
     *   offline/online/unbound 等事件平台台账里已经有条目了, 不必重复带。
     *   bind_failed 没有 uid(空串), 带 type 也没意义。 */
    if (event == LORA_BIND_EVENT_BOUND)
    {
        cjw_kv_uint(&w, "type", 1u);
    }
    cjw_kv_str(&w, "event",   ev_str);
    cjw_obj_end(&w);
    if (!cjw_ok(&w))
    {
        log_info("[CLOUD] !! event.device(bind) data 段组装失败\r\n");
        return;
    }

    (void)utils_gen_msg_id(msg_id, (uint16_t)sizeof(msg_id));

    plen = cloud_proto_build_uplink(payload, (uint16_t)sizeof(payload),
                                    "event.device", msg_id, app_now_unix(), data);
    if (plen == 0u)
    {
        log_info("[CLOUD] !! event.device(bind) 信封组装失败\r\n");
        return;
    }

    tlen = cloud_profile_build_topic(CLOUD_TOPIC_EVENT, topic, (uint16_t)sizeof(topic));
    if (tlen == 0u)
    {
        log_info("[CLOUD] !! event topic 拼装失败, 绑定事件丢弃\r\n");
        return;
    }

    log_info("[CLOUD] 上报 %s (%u 字节): %s\r\n", topic, (unsigned)plen, payload);

    if (g4_at_mqtt_publish_ex(topic, (const uint8_t *)payload, plen,
                              cloud_profile_topic_qos(CLOUD_TOPIC_EVENT),
                              cloud_profile_topic_retained(CLOUD_TOPIC_EVENT)) != SYS_OK)
    {
        log_info("[CLOUD] !! 绑定事件发布登记失败, 这条事件只记在本机\r\n");
    }
}
