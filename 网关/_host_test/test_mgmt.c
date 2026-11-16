/* ============================================================================
 * test_mgmt.c —— app/core/app_mgmt.c 的主机护栏(2026-11-12 任务三 T3-3/T3-4)
 * ----------------------------------------------------------------------------
 * 为什么要单独一套: 管理面 cmd 与业务面 cmd 的【后缀都叫 /cmd】。分流只要退化
 * 成后缀判断, 管理命令就会被当成业务命令、应答回错通道 —— 而且功能看起来"还
 * 能用", 只有平台侧收不到应答才发现。这套测试把判据钉死在完整 topic 上。
 *
 * 覆盖:
 *   [1] classify: 两面精确匹配 / 前缀不同不串面 / 伪造后缀 / 应答 topic 不当命令
 *       / #json 控制台 / NULL / 本地档位(只注入业务面)
 *   [2] app_mgmt_handle: 确实复用 cloud_proto 命令表, 不另起一套协议
 *   [3] checkin: 独立管理面 topic / QoS1 / 不 retained / 正文不含业务数据
 *       / topic 缓冲 >= 48(26 位 gwId + ota.progress 最长 47 字符)
 *       / 本地档位不可用时不发布
 *   [4] online_poll: 上升沿只发一次 / 持续在线不刷 / 掉线重连再发
 *       / 发布失败最多重试 3 拍
 *
 * 本文件自带 app_mgmt.c 需要的全部底层替身(host 日志/时基/profile/发布)。
 * ⚠ 刻意【不】链 host_stub.c: 那会拖进 ring_buf 与 USART2 替身, 与本层无关。
 * ========================================================================== */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

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

static int g_pass;
static int g_fail;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) { g_pass++; printf("  ok   : "); }             \
        else      { g_fail++; printf("  FAIL : "); }             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    } while (0)

/* ==================== 可控的假环境 ==================== */
#define HOST_GWID   "11360811504139433230092D2C"    /* 现场实测的 26 位 gwId */

static bool     s_doc_mode = true;
static uint32_t s_now_unix = 1735689600u;
static uint32_t s_tick_ms  = 12345000u;
static uint32_t s_cfg_ver  = 106u;
static sys_err_t s_pub_ret = SYS_OK;

static int      s_pub_cnt;
static char     s_pub_topic[128];
static char     s_pub_payload[1024];
static uint16_t s_pub_len;
static uint8_t  s_pub_qos;
static bool     s_pub_retain;

static int      s_handle_cnt;
static char     s_handle_json[256];

static char     s_uplink_event[32];
static char     s_uplink_data[512];

static cloud_topic_t s_last_topic;
static uint16_t      s_last_cap;

static const char *const s_biz_suffix[6]  = { "cmd", "cmdresp", "telemetry",
                                             "event", "status", "lwt" };
static const char *const s_mgmt_suffix[4] = { "cmd", "cmdresp", "checkin",
                                             "ota.progress" };

/* ---- host 日志: 默认静默, 别把测试输出冲掉 ---- */
void host_log_printf(const char *fmt, ...) { (void)fmt; }
void host_log_enable(int on)               { (void)on;  }

/* ---- 时基 / 状态 ---- */
uint32_t get_tick_ms(void)          { return s_tick_ms;        }
uint32_t app_now_unix(void)         { return s_now_unix;       }
uint32_t config_store_version(void) { return s_cfg_ver;        }

bool utils_gen_msg_id(char *out, uint16_t cap)
{
    if ((out == NULL) || (cap < 9u)) { return false; }
    (void)memcpy(out, "MGMT0001", 9u);
    return true;
}

/* ---- cloud_profile: 只保留 app_mgmt.c 真正用到的五个接口 ---- */
const cloud_profile_t *cloud_profile_get(void)
{
    static cloud_profile_t p;

    (void)memset(&p, 0, sizeof(p));
    p.mode = (uint8_t)(s_doc_mode ? CLOUD_MODE_DOC : CLOUD_MODE_LOCAL);
    (void)memcpy(p.gw_id, HOST_GWID, sizeof(HOST_GWID));
    (void)memcpy(p.topic_prefix, "rt/gw", sizeof("rt/gw"));
    return &p;
}

uint16_t cloud_profile_build_topic(cloud_topic_t t, char *out, uint16_t cap)
{
    int n;

    s_last_topic = t;
    s_last_cap   = cap;
    if ((out == NULL) || (cap == 0u) || (t >= CLOUD_TOPIC_COUNT))
    {
        return 0u;
    }
    out[0] = '\0';

    if (t >= (cloud_topic_t)CLOUD_TOPIC_BIZ_COUNT)
    {
        /* 本地/历史档位【没有】管理面: 返回 0 = 不可用(与真实实现一致) */
        if (!s_doc_mode) { return 0u; }
        n = snprintf(out, cap, "rt/mgmt/%s/%s", HOST_GWID,
                     s_mgmt_suffix[(unsigned)t - CLOUD_TOPIC_BIZ_COUNT]);
    }
    else if (s_doc_mode)
    {
        n = snprintf(out, cap, "rt/gw/%s/%s", HOST_GWID, s_biz_suffix[(unsigned)t]);
    }
    else
    {
        n = snprintf(out, cap, "mod/%s", s_biz_suffix[(unsigned)t]);
    }

    /* 与真实 app_str() 同口径: 装不下就是拼装失败, 绝不返回截断串 */
    if ((n < 0) || ((uint16_t)n >= cap))
    {
        out[0] = '\0';
        return 0u;
    }
    return (uint16_t)n;
}

uint8_t cloud_profile_topic_qos(cloud_topic_t t)      { (void)t; return 1u;    }
bool    cloud_profile_topic_retained(cloud_topic_t t) { (void)t; return false; }

/* ---- 协议层: 只验证"有没有被复用", 不在这里实现真实命令 ---- */
int cloud_proto_handle(const char *json, uint32_t now_ts,
                       char *resp, uint16_t resp_cap, uint16_t *resp_len)
{
    int n;

    (void)now_ts;
    s_handle_cnt++;
    (void)snprintf(s_handle_json, sizeof(s_handle_json), "%s", json);
    n = snprintf(resp, resp_cap, "{\"code\":0}");
    *resp_len = (n > 0) ? (uint16_t)n : 0u;
    return 0;
}

uint16_t cloud_proto_build_uplink(char *out, uint16_t cap, const char *event,
                                  const char *msg_id, uint32_t ts,
                                  const char *data_json)
{
    int n;

    (void)msg_id;
    (void)snprintf(s_uplink_event, sizeof(s_uplink_event), "%s", event);
    (void)snprintf(s_uplink_data, sizeof(s_uplink_data), "%s",
                   (data_json != NULL) ? data_json : "{}");
    n = snprintf(out, cap, "{\"event\":\"%s\",\"ts\":%lu,\"data\":%s}",
                 event, (unsigned long)ts, (data_json != NULL) ? data_json : "{}");
    if ((n < 0) || ((uint16_t)n >= cap)) { return 0u; }
    return (uint16_t)n;
}

/* ---- 发布: 记录最后一次发布的一切, 返回值可注入失败 ---- */
sys_err_t g4_at_mqtt_publish_ex(const char *topic, const uint8_t *payload,
                                uint16_t payload_len, uint8_t qos, bool retain)
{
    uint16_t n = (payload_len < (uint16_t)(sizeof(s_pub_payload) - 1u))
                   ? payload_len : (uint16_t)(sizeof(s_pub_payload) - 1u);

    s_pub_cnt++;
    (void)snprintf(s_pub_topic, sizeof(s_pub_topic), "%s",
                   (topic != NULL) ? topic : "(null)");
    if ((payload != NULL) && (n > 0u))
    {
        (void)memcpy(s_pub_payload, payload, n);
    }
    s_pub_payload[n] = '\0';
    s_pub_len    = payload_len;
    s_pub_qos    = qos;
    s_pub_retain = retain;
    return s_pub_ret;
}

static void pub_reset(void)
{
    s_pub_cnt       = 0;
    s_pub_topic[0]  = '\0';
    s_pub_payload[0] = '\0';
    s_pub_len       = 0u;
    s_pub_qos       = 0u;
    s_pub_retain    = true;
    s_pub_ret       = SYS_OK;
    s_doc_mode      = true;
}

/* ==================== [1] 下行分流判定 ==================== */
static void test_classify(void)
{
    char biz[64];
    char mgmt[64];

    printf("\n[1] 完整 topic 精确分流(★两面 cmd 后缀都是 /cmd★)\n");

    /* 未注入: 任何 topic 都只能走兼容路径 */
    app_mgmt_set_cmd_topics(NULL, NULL);
    CHECK(app_mgmt_classify_cmd_topic("rt/mgmt/" HOST_GWID "/cmd") == APP_MGMT_SRC_OTHER,
          "未注入时管理面 topic 也判 OTHER(不会误分流)");
    CHECK(!app_mgmt_is_cmd_topic("rt/mgmt/" HOST_GWID "/cmd"),
          "未注入时 is_cmd_topic 恒 false");

    (void)snprintf(biz,  sizeof(biz),  "rt/gw/%s/cmd",   HOST_GWID);
    (void)snprintf(mgmt, sizeof(mgmt), "rt/mgmt/%s/cmd", HOST_GWID);
    app_mgmt_set_cmd_topics(biz, mgmt);

    CHECK(app_mgmt_classify_cmd_topic(biz) == APP_MGMT_SRC_BIZ,
          "业务面 %s -> BIZ", biz);
    CHECK(app_mgmt_classify_cmd_topic(mgmt) == APP_MGMT_SRC_MGMT,
          "管理面 %s -> MGMT", mgmt);
    CHECK(app_mgmt_is_cmd_topic(mgmt) && (!app_mgmt_is_cmd_topic(biz)),
          "is_cmd_topic 只认管理面, 业务面恒 false");
    CHECK(strcmp(biz + strlen(biz) - 4u, mgmt + strlen(mgmt) - 4u) == 0,
          "★两条 topic 的后缀确实完全相同(证明后缀判据必然串面)★");

    CHECK(app_mgmt_classify_cmd_topic("rt/mgmt/" HOST_GWID "/cmdresp") == APP_MGMT_SRC_OTHER,
          "管理面【应答】topic 不是下行命令 -> OTHER");
    CHECK(app_mgmt_classify_cmd_topic("rt/mgmt/" HOST_GWID "/checkin") == APP_MGMT_SRC_OTHER,
          "管理面【报到】topic 不是下行命令 -> OTHER");
    CHECK(app_mgmt_classify_cmd_topic("rt/mgmt/" HOST_GWID "/cmdX") == APP_MGMT_SRC_OTHER,
          "伪造的 /cmdX 不命中(必须是完整串相等)");
    CHECK(app_mgmt_classify_cmd_topic("rt/mgmtX/" HOST_GWID "/cmd") == APP_MGMT_SRC_OTHER,
          "前缀多一个字符也不命中");
    CHECK(app_mgmt_classify_cmd_topic("(console)") == APP_MGMT_SRC_OTHER,
          "#json 控制台注入 -> OTHER(保持改动前的业务面行为)");
    CHECK(app_mgmt_classify_cmd_topic(NULL) == APP_MGMT_SRC_OTHER,
          "NULL topic -> OTHER");
    CHECK(app_mgmt_classify_cmd_topic("") == APP_MGMT_SRC_OTHER,
          "空 topic -> OTHER");

    /* 本地/历史档位: 只注入业务面 */
    app_mgmt_set_cmd_topics("mod/cmd", NULL);
    CHECK(app_mgmt_classify_cmd_topic("mod/cmd") == APP_MGMT_SRC_BIZ,
          "本地档位: 业务面仍精确命中 BIZ");
    CHECK(app_mgmt_classify_cmd_topic("rt/mgmt/" HOST_GWID "/cmd") == APP_MGMT_SRC_OTHER,
          "本地档位: 管理面 topic 判 OTHER(管理面已关闭)");
    CHECK(!app_mgmt_is_cmd_topic("rt/mgmt/" HOST_GWID "/cmd"),
          "本地档位: is_cmd_topic 恒 false");
}

/* ==================== [2] 管理面不另起一套协议 ==================== */
static void test_handle(void)
{
    char resp[256];
    uint16_t rlen = 0u;
    int rc;

    printf("\n[2] 管理面命令复用 cloud_proto 命令表\n");

    s_handle_cnt = 0;
    rc = app_mgmt_handle("{\"cmd\":\"sys.query_status\",\"msgId\":\"A1\"}",
                         s_now_unix, resp, (uint16_t)sizeof(resp), &rlen);
    CHECK((rc == 0) && (rlen > 0u), "app_mgmt_handle 返回正常应答(%u 字节)", (unsigned)rlen);
    CHECK(s_handle_cnt == 1, "★确实调了 cloud_proto_handle 一次★ (实际 %d)", s_handle_cnt);
    CHECK(strstr(s_handle_json, "sys.query_status") != NULL,
          "原样把 JSON 交给命令表, 没有二次解析/白名单");
}

/* ==================== [3] checkin 上行 ==================== */
static void test_checkin(void)
{
    sys_err_t r;

    printf("\n[3] 开机报到 checkin: 独立管理面 topic / 不含业务数据\n");

    pub_reset();
    r = app_mgmt_checkin();
    CHECK(r == SYS_OK, "checkin 发布登记成功");
    CHECK(s_pub_cnt == 1, "只发一条(实际 %d)", s_pub_cnt);
    CHECK(strcmp(s_pub_topic, "rt/mgmt/" HOST_GWID "/checkin") == 0,
          "topic = rt/mgmt/{gwId}/checkin (实际 %s)", s_pub_topic);
    CHECK(s_last_topic == CLOUD_TOPIC_MGMT_CHECKIN,
          "确实按 CLOUD_TOPIC_MGMT_CHECKIN 拼装(不是 event)");
    CHECK(s_last_cap >= 48u,
          "topic 缓冲 cap=%u >= 48(26 位 gwId + ota.progress 最长 47 字符)",
          (unsigned)s_last_cap);
    CHECK((s_pub_qos == 1u) && (!s_pub_retain),
          "QoS1 且不 retained(实际 qos=%u retain=%d)",
          (unsigned)s_pub_qos, (int)s_pub_retain);
    CHECK(strcmp(s_uplink_event, "checkin") == 0,
          "信封 event = checkin (实际 %s)", s_uplink_event);
    CHECK((strstr(s_uplink_data, HOST_GWID) != NULL) &&
          (strstr(s_uplink_data, MAIN_FW_VERSION) != NULL) &&
          (strstr(s_uplink_data, "cfgVer") != NULL) &&
          (strstr(s_uplink_data, "uptime") != NULL) &&
          (strstr(s_uplink_data, "online") != NULL),
          "data 含 gwId/fwVer/cfgVer/uptime/online");
    CHECK((strstr(s_uplink_data, "sensor") == NULL) &&
          (strstr(s_uplink_data, "level") == NULL) &&
          (strstr(s_uplink_data, "gear") == NULL) &&
          (strstr(s_uplink_data, "telemetry") == NULL),
          "★data 不含任何业务数据(sensor/level/gear/telemetry)★");
    CHECK((strstr(s_pub_payload, "\"event\":\"checkin\"") != NULL) &&
          (s_pub_len == strlen(s_pub_payload)),
          "发布正文 = 完整信封, 长度 %u 字节与正文一致", (unsigned)s_pub_len);

    /* 本地档位没有管理面: 不发布, 但也不能当成崩溃 */
    pub_reset();
    s_doc_mode = false;
    r = app_mgmt_checkin();
    CHECK((r == SYS_ERR) && (s_pub_cnt == 0),
          "本地档位: checkin 明确跳过且【不】往业务面乱发");
}

/* ==================== [4] ONLINE 上升沿 ==================== */
static void test_online_poll(void)
{
    int i;

    printf("\n[4] MQTT ONLINE 上升沿报到 + 失败重试\n");

    /* ★s_was_online / s_pending 是 app_mgmt.c 的 static, 无法重置★
     * 所以这里按一个完整的状态序列走, 顺序本身就是用例的一部分。 */
    pub_reset();
    app_mgmt_online_poll(false);                     /* 明确离线: 清掉历史 */
    CHECK(s_pub_cnt == 0, "离线不发 checkin");

    app_mgmt_online_poll(true);                      /* 上升沿 */
    CHECK(s_pub_cnt == 1, "★上升沿只发一次★ (实际 %d)", s_pub_cnt);

    for (i = 0; i < 10; i++) { app_mgmt_online_poll(true); }
    CHECK(s_pub_cnt == 1, "持续在线 10 拍不重复刷(仍 %d 条)", s_pub_cnt);

    app_mgmt_online_poll(false);
    app_mgmt_online_poll(true);
    CHECK(s_pub_cnt == 2, "掉线重连 -> 新的上升沿再发一条(共 %d)", s_pub_cnt);

    /* 失败重试: 再来一轮新的上升沿, 让发布登记恒失败 */
    app_mgmt_online_poll(false);
    s_pub_cnt = 0;
    s_pub_ret = SYS_ERR;
    app_mgmt_online_poll(true);
    app_mgmt_online_poll(true);
    app_mgmt_online_poll(true);
    app_mgmt_online_poll(true);
    CHECK(s_pub_cnt == 3, "★发布失败最多重试 3 拍后停手★ (实际 %d)", s_pub_cnt);

    s_pub_ret = SYS_OK;
    app_mgmt_online_poll(true);
    CHECK(s_pub_cnt == 3, "重试预算耗尽后即使链路恢复也不补发(等下次建链)");
    app_mgmt_online_poll(false);
    app_mgmt_online_poll(true);
    CHECK(s_pub_cnt == 4, "下次建链的上升沿重新获得 3 次预算并成功(共 %d)", s_pub_cnt);
}

int main(void)
{
    printf("==== test_mgmt: 管理面 topic 分流 / checkin 报到 ====\n");

    test_classify();
    test_handle();
    test_checkin();
    test_online_poll();

    printf("\n==== %d passed, %d FAILED ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
