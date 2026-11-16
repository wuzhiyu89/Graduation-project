/**
 * @file  app_mgmt.c
 * @brief 管理面 topic 分流判定 / 开机报到 —— 2026-11-12 任务三 T3-3 + T3-4
 *
 * 见 app_mgmt.h 的边界说明: 这里【不解析业务命令】, 只判"这条下行从哪条通道进来"。
 * ★应答不在这里发★ —— app_downlink.c 按分类结果二选一选 topic, 全工程只有
 * 那一处发布路径(还要负责失败时的错误应答)。管理面上行这里只有 checkin 一条;
 * ota.progress 仍在 app_uplink.c 组包, 只是 topic 换到管理面。
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "app_mgmt.h"
#include "app_config.h"
#include "app_state.h"
#include "bsp_tim.h"
#include "cloud_profile.h"
#include "cloud_proto.h"
#include "config_store.h"
#include "g4_at.h"
#include "log.h"
#include "utils.h"

#define MGMT_TOPIC_MAX   64u

/* 两条完整的下行 topic。由 app_boot.c 建链前注入 —— 与 g4_at 的订阅表同源。
 * 用【完整 topic 精确匹配】而不是后缀: 业务面 cmd 与管理面 cmd 后缀都是 "/cmd",
 * 只看后缀必然把管理命令当业务命令处理(任务书 4.3 T3-3 的核心陷阱)。 */
static char s_topic_biz[MGMT_TOPIC_MAX];
static char s_topic_mgmt[MGMT_TOPIC_MAX];
static bool s_topics_ready;

static void mgmt_copy_topic(char *dst, const char *src)
{
    if ((src == NULL) || (src[0] == '\0'))
    {
        dst[0] = '\0';
        return;
    }
    (void)strncpy(dst, src, MGMT_TOPIC_MAX - 1u);
    dst[MGMT_TOPIC_MAX - 1u] = '\0';
}

void app_mgmt_set_cmd_topics(const char *biz_cmd, const char *mgmt_cmd)
{
    mgmt_copy_topic(s_topic_biz,  biz_cmd);
    mgmt_copy_topic(s_topic_mgmt, mgmt_cmd);
    s_topics_ready = ((s_topic_biz[0] != '\0') && (s_topic_mgmt[0] != '\0'));

    log_info("[MGMT] 下行 topic: biz=%s | mgmt=%s%s\r\n",
             (s_topic_biz[0]  != '\0') ? s_topic_biz  : "(未注入)",
             (s_topic_mgmt[0] != '\0') ? s_topic_mgmt : "(未启用)",
             s_topics_ready ? "" : "  [管理面不可用: 只走业务面]");
}

app_mgmt_cmd_src_t app_mgmt_classify_cmd_topic(const char *topic)
{
    if ((topic == NULL) || (topic[0] == '\0'))
    {
        return APP_MGMT_SRC_OTHER;
    }
    /* 先判管理面: 两面后缀都是 /cmd, 只有完整串能区分。
     * 两条各自判空, 【不】用 s_topics_ready —— 本地档位只注入业务面时也要能
     * 精确认出业务面(否则日志会把正常业务命令标成"兼容/其它")。 */
    if ((s_topic_mgmt[0] != '\0') && (strcmp(topic, s_topic_mgmt) == 0))
    {
        return APP_MGMT_SRC_MGMT;
    }
    if ((s_topic_biz[0] != '\0') && (strcmp(topic, s_topic_biz) == 0))
    {
        return APP_MGMT_SRC_BIZ;
    }
    /* #json 控制台传的是 "(console)"; 平台若往别的 topic 发了 JSON, 也落这里。
     * ★不拦截、不丢弃★ —— 保持改动前的业务面行为, 老链路一字不变。 */
    return APP_MGMT_SRC_OTHER;
}

bool app_mgmt_is_cmd_topic(const char *topic)
{
    return (app_mgmt_classify_cmd_topic(topic) == APP_MGMT_SRC_MGMT);
}

int app_mgmt_handle(const char *json, uint32_t now_ts,
                    char *resp, uint16_t resp_cap, uint16_t *resp_len)
{
    int rc;

    /* 正文由 app_downlink_try() 在入口统一打过([4G DOWN]), 这里不重复刷屏。 */
    log_info("[MGMT] 管理面命令 -> 复用 cloud_proto 命令表(ACL 由 broker 侧负责)\r\n");
    rc = cloud_proto_handle(json, now_ts, resp, resp_cap, resp_len);
    return rc;                       /* 统一的 "处理完成" 日志由 app_downlink 打 */
}

sys_err_t app_mgmt_checkin(void)
{
    static char payload[CLOUD_RESP_MAX];
    static char data[256];
    char        topic[MGMT_TOPIC_MAX];
    char        msg_id[20];
    uint16_t    plen;
    uint16_t    tlen;
    const cloud_profile_t *p = cloud_profile_get();

    tlen = cloud_profile_build_topic(CLOUD_TOPIC_MGMT_CHECKIN,
                                     topic, (uint16_t)sizeof(topic));
    if (tlen == 0u)
    {
        /* 本地/历史档位没有管理面: 这是【正常】情况, 不是故障, 只留一行证据 */
        log_info("[MGMT] checkin 跳过: 当前档位没有管理面 topic\r\n");
        return SYS_ERR;
    }

    /* ★规范: 不含任何业务数据★ —— 只有身份 / 版本 / 运行时长 / 在线标志。
     * gwId 与 clientID、两条 cmd topic 里的 {gwId} 同源(cloud_profile)。 */
    (void)snprintf(data, sizeof(data),
                   "{\"gwId\":\"%s\",\"fwVer\":\"%s\",\"cfgVer\":%lu,"
                   "\"uptime\":%lu,\"online\":true}",
                   p->gw_id, MAIN_FW_VERSION,
                   (unsigned long)config_store_version(),
                   (unsigned long)(get_tick_ms() / 1000u));

    (void)utils_gen_msg_id(msg_id, (uint16_t)sizeof(msg_id));
    plen = cloud_proto_build_uplink(payload, (uint16_t)sizeof(payload),
                                    "checkin", msg_id, app_now_unix(), data);
    if (plen == 0u)
    {
        log_info("[MGMT] !! checkin 信封组装失败\r\n");
        return SYS_ERR;
    }

    log_info("[MGMT] 上报 %s (%u 字节): %s\r\n", topic, (unsigned)plen, payload);
    return g4_at_mqtt_publish_ex(topic, (const uint8_t *)payload, plen,
                                 cloud_profile_topic_qos(CLOUD_TOPIC_MGMT_CHECKIN),
                                 cloud_profile_topic_retained(CLOUD_TOPIC_MGMT_CHECKIN));
}

void app_mgmt_online_poll(bool online)
{
    static bool    s_was_online;
    static uint8_t s_pending;

    /* 上升沿: 每次重新建链都重新报到一次(平台据此比对 configVersion)。
     * ★2026-11-12 按实际代码复核★ g4_at_is_online() = g4_urc_is_online() &&
     * g_state == G4_STATE_ONLINE; 而 g_state 要等 g4_st_mqtt_sub() 把【全部订阅
     * 逐条订完】-> G4_STATE_REG_ARM -> 才切到 ONLINE(见 g4_at.c:4933/4979/5093)。
     * 所以 checkin 发出去时管理面订阅已生效, 平台收得到应答通道。
     * (只盯 g4_urc_is_online() 是不够的: +MQTTURC "conn",0 会更早置位。) */
    if (online && (!s_was_online)) { s_pending = 3u; }
    if (!online)                   { s_pending = 0u; }
    s_was_online = online;

    if ((s_pending == 0u) || (!online)) { return; }

    if (app_mgmt_checkin() == SYS_OK)
    {
        s_pending = 0u;
    }
    else
    {
        s_pending--;
        log_info("[MGMT] !! checkin 发布登记失败, 剩 %u 次重试\r\n",
                 (unsigned)s_pending);
    }
}
