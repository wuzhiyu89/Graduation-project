/**
 * @file  app_mgmt.h
 * @brief 管理面(rt/mgmt/{gwId}/...) 的下行分流与上行报到 —— 2026-11-12 任务三
 *
 * 【边界】本模块【不实现第二套命令协议】。管理面 cmd 与业务面 cmd 共用
 * cloud_proto 的命令表与信封(msgId/ts), 差别只有两处:
 *   1. 订阅/发布的 topic 前缀不同(rt/mgmt vs rt/gw);
 *   2. 应答回到 rt/mgmt/{gwId}/cmdresp, 不回业务面 cmdresp。
 * 权限隔离由 broker 侧账号 + ACL 承担(管理账号仅 rt/mgmt/#), 固件不做命令白名单
 * —— 这样 cfg.set / ota.start / sys.reboot 等已经上板验证过的命令一行都不用重写。
 *
 * 【为什么单独一个 .c】cloud_proto.c 是业务协议层, 已经很大; 把 topic 分面、
 * 报到、应答路由放进它等于把"传输拓扑"混进"业务语义"。放在 app/core 这一层
 * 与 app_downlink.c 的原有分流同层, 依赖方向 core -> cloud / links 不变。
 */
#ifndef APP_MGMT_H
#define APP_MGMT_H

#include <stdint.h>
#include <stdbool.h>

#include "g4_at.h"        /* sys_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注入两条完整下行 topic(app_boot.c 在建链前调用)
 * @param biz_cmd   业务面 rt/gw/{gwId}/cmd
 * @param mgmt_cmd  管理面 rt/mgmt/{gwId}/cmd; 本地档位下传 NULL/空串 = 不启用
 * @note  两条与 g4_at 的订阅表【同源】。注入后 app_downlink.c 才能按完整 topic
 *        把两面分开; 没注入时 classify 一律返回 OTHER, 走原有业务面路径。
 */
void app_mgmt_set_cmd_topics(const char *biz_cmd, const char *mgmt_cmd);

/** 下行 topic 的归属。★判据是完整 topic 的 strcmp, 不是 "/cmd" 后缀★
 *  (业务面 cmd 与管理面 cmd 的后缀都叫 /cmd, 只看后缀必然串面) */
typedef enum
{
    APP_MGMT_SRC_BIZ = 0,   /* 精确命中业务面 cmd */
    APP_MGMT_SRC_MGMT,      /* 精确命中管理面 cmd -> 应答回 rt/mgmt/.../cmdresp */
    APP_MGMT_SRC_OTHER      /* 未注入 / 本地档位 / #json 控制台 / 其它 topic
                             * -> 保持改动前的业务面行为(兼容, 不丢命令) */
} app_mgmt_cmd_src_t;

app_mgmt_cmd_src_t app_mgmt_classify_cmd_topic(const char *topic);

/** topic 是不是管理面 cmd(【精确】匹配; 未注入或本地档位恒 false) */
bool app_mgmt_is_cmd_topic(const char *topic);

/** 处理一条管理面下行 JSON: 复用 cloud_proto_handle(), 只是通道不同 */
int app_mgmt_handle(const char *json, uint32_t now_ts,
                    char *resp, uint16_t resp_cap, uint16_t *resp_len);

/* ★应答【不】在这里发★: app_downlink.c 有且只有一处发布路径, 它按
 * classify 结果选 CLOUD_TOPIC_CMDRESP / CLOUD_TOPIC_MGMT_CMDRESP, 失败时还要
 * 往【同一个】topic 补一条小错误应答。若这里再开一个 publish helper, 就会
 * 出现两条应答通道, 日后必然分叉。 */

/**
 * @brief 组一条开机报到, 发到 rt/mgmt/{gwId}/checkin
 * @note  不含任何业务数据(传感器/液位/档位); 只有身份、版本、在线状态。
 */
sys_err_t app_mgmt_checkin(void);

/** 每秒调用一次: MQTT online 上升沿自动发 checkin, 失败最多重试 3 拍 */
void app_mgmt_online_poll(bool online);

#ifdef __cplusplus
}
#endif

#endif /* APP_MGMT_H */
