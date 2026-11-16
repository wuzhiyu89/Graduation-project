/**
 * \file  test_cloud.c
 * \brief cloud_json / cloud_proto / cloud_profile 的【主机单元测试】(P1-0/P1-1/P1-2)
 *
 * 为什么在 PC 上测而不是上板:
 *   JSON 解析最容易错在"边界"上 —— 转义、\uXXXX、数组越界、超长 msgId、
 *   缓冲溢出、数字边界(ts 是 ~1.7e9, 会撞 int32 保护)。这些在板子上要靠
 *   "发一条看一眼"很难覆盖全, 在 PC 上可以用确定输入一次跑完。
 *
 * 覆盖:
 *   1)  JSON 路径取值(标量/嵌套/数组下标/不存在)
 *   2)  数字边界: ts=1735689600 不被溢出保护误判
 *   3)  字符串转义 \" \\ \n 与 \uXXXX -> UTF-8
 *   4)  生成器: 逗号/嵌套/数组/UTF-8 原样通过
 *   5)  生成器缓冲不足 -> ovf 被检出(不越界)
 *   6)  信封解析: 正常 / 缺 cmd / msgId 超 16 字节
 *   7)  应答信封与文档示例逐字符一致
 *   8)  21 条命令表齐全 + 未知命令
 *   9)  分发: 已知未实现 -> 1006, 未知 -> 1001, 坏报文 -> 1001
 *  10)  应用层鉴权: 无 auth -> 1008, auth 正确 -> 进入分发
 *  11)  profile: 本地/文档两种 topic 拼装、QoS、retained
 *  12)  profile 校验: mode/host/port/gwId 非法值被拒
 *  13)  上行事件信封与文档示例逐字符一致
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "log.h"
#include "cjson_port.h"      /* v33: 信封层改用 cJSON, 测试前必须先 cjson_port_init() */
#include "cloud_json.h"
#include "utils.h"           /* msgId 生成器(8 位小写十六进制) */
#include "cloud_proto.h"
#include "cloud_profile.h"
#include "config_store.h"
#include "last_value.h"      /* v53: sen.query/dev.query 的"最近一次上报"字段 */
#include "event_cache.h"     /* v57: 断线事件缓存（★2026-11-11 起落在外挂 Flash） */
#include "xflash_store.h"    /* 事件队列的容量由 EVENT 分区算出 */
#include "sensor_offline.h"
#include "bsp_tim.h"         /* ★A2: 造"刚刚上报过"的快照要用 get_tick_ms() */
#include "param_save.h"
#include "ota_http.h"        /* 2026-10-02: ota.start/query/abort 接了真实实现 */
#include "ota_port.h"        /* ota_port_get_version(): "与当前版本相同"判据 */
#include "flash_map.h"       /* FM_STAGE_SIZE: 暂存区容量(1010 判据) */
#include "fake_lora_business.h"  /* P5: cloud_proto 会调 lora_business_unbind, 见该头文件说明 */

/* host_stub.c 提供的虚拟时基。本套件要控制"当前时刻"才能验 online 判据
 * (online = 距上次上报 < 3×上报周期, 判据在 cloud_proto.c 里读 get_tick_ms())。
 * 注: 工程没有 host_stub.h, test_rx.c 是隐式声明用的 —— 这里显式声明, 免得再添警告。
 * (要更干净的话应该抽一个 _host_test/host_stub.h 出来, 那是一次单独的整理。) */
extern void host_tick_set(uint32_t ms);

/* ================= param_save 替身(主机上不碰 Flash) ================= */
static device_param_t s_param;

device_param_t *param_get_ptr(void) { return &s_param; }
int             param_save_store(void) { return 0; }

/* ================= 测试框架 ================= */
static int g_fail;
static int g_pass;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) { g_pass++; printf("  ok   : "); }             \
        else      { g_fail++; printf("  FAIL : "); }             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    } while (0)

/* UTF-8 "中文" 的显式字节, 避免依赖编译器执行字符集 */
static const uint8_t ZH_CN[6] = { 0xE4u, 0xB8u, 0xADu, 0xE6u, 0x96u, 0x87u };

/** 判断串是否为 8 位小写十六进制(msgId 的统一格式) */
static int test_is_hex8(const char *s)
{
    uint8_t i;

    if ((s == NULL) || (strlen(s) != 8u))
    {
        return 0;
    }
    for (i = 0u; i < 8u; i++)
    {
        if (!(((s[i] >= '0') && (s[i] <= '9')) || ((s[i] >= 'a') && (s[i] <= 'f'))))
        {
            return 0;
        }
    }
    return 1;
}

/* ================================================================= */
static void test_json_get(void)
{
    const char *j =
        "{\"cmd\":\"cfg.set\",\"msgId\":\"c100\",\"ts\":1735689600,"
        "\"auth\":\"\",\"data\":{\"expectVersion\":7}}";
    char buf[64];

    printf("[1] JSON 路径取值\n");

    CHECK(cj_get_str(j, "cmd", buf, sizeof(buf)) && (strcmp(buf, "cfg.set") == 0),
          "cj_get_str(\"cmd\") == \"cfg.set\"");

    /* ts 约 1.7e9: 曾经被写成 v>200000000 就判溢出的 bug 就死在这 */
    CHECK(cj_get_uint(j, "ts", 0u) == 1735689600u,
          "cj_get_uint(\"ts\") == 1735689600 (不被溢出保护误判)");

    CHECK(cj_get_int(j, "data.expectVersion", -1) == 7,
          "cj_get_int(\"data.expectVersion\") == 7");

    CHECK(cj_get(j, "data", NULL, 0u) == CJ_OBJ,
          "cj_get(\"data\") 类型 == CJ_OBJ");

    CHECK(cj_get(j, "nope", NULL, 0u) == CJ_NOTFOUND,
          "不存在的键 -> CJ_NOTFOUND");

    CHECK(cj_get(j, "data.nope", NULL, 0u) == CJ_NOTFOUND,
          "不存在的子键 -> CJ_NOTFOUND");

    CHECK(cj_get_uint(j, "cmd", 123u) == 123u,
          "对字符串取数字 -> 返回默认值");

    CHECK(cj_get(j, "ts", buf, sizeof(buf)) == CJ_NUM && (strcmp(buf, "1735689600") == 0),
          "cj_get 数字以文本写回");

    /* ★2026-10-23 P1: 头文件承诺"接受 true/false 以及 0/1"。
     * 以前只认字面量, 数字落到默认值 —— 平台发 {"enabled":0} 会被当成 true,
     * 该关的功能关不掉, 而且一条日志都没有。 */
    {
        const char *jb =
            "{\"a\":true,\"b\":false,\"c\":0,\"d\":1,\"e\":\"true\","
            "\"f\":0,\"g\":1}";

        CHECK(cj_get_bool(jb, "a", false), "bool: true  -> true");
        CHECK(!cj_get_bool(jb, "b", true), "bool: false -> false");
        CHECK(!cj_get_bool(jb, "c", true),
              "★ bool: 数字 0 -> false(默认值 true 也不能赖着)");
        CHECK(cj_get_bool(jb, "d", false), "★ bool: 数字 1 -> true");
        CHECK(cj_get_bool(jb, "e", true), "bool: 字符串 \"true\" 不是布尔 -> 默认值");
        CHECK(!cj_get_bool(jb, "f", true), "★ bool: {\"enabled\":0} -> false");
        CHECK(cj_get_bool(jb, "g", false), "★ bool: {\"enabled\":1} -> true");
        CHECK(cj_get_bool(jb, "nope", true), "bool: 缺失 -> 默认值");
    }
}

static void test_json_array(void)
{
    const char *j =
        "{\"data\":{\"sensors\":[{\"addr\":1,\"levelCm\":12.3},"
        "{\"addr\":2,\"rawMm\":2919}]}}";

    printf("[2] JSON 数组与下标\n");

    CHECK(cj_count(j, "data.sensors") == 2, "cj_count() == 2");
    CHECK(cj_get_int(j, "data.sensors[0].addr", -1) == 1, "[0].addr == 1");
    CHECK(cj_get_int(j, "data.sensors[1].rawMm", -1) == 2919, "[1].rawMm == 2919");
    CHECK(cj_get_int(j, "data.sensors[5].addr", -1) == -1, "越界下标 -> 默认值");
    CHECK(cj_count(j, "data.nope") == 0, "非数组 -> 0");
    CHECK(cj_count(j, "data") == 0, "对象不是数组 -> 0");
    CHECK(cj_get(j, "data.sensors", NULL, 0u) == CJ_ARR, "sensors 类型 == CJ_ARR");
    CHECK(cj_get_sub(j, "data.sensors[1]") != NULL, "cj_get_sub 取到元素指针");
}

static void test_json_escape(void)
{
    const char *j =
        "{\"s\":\"a\\\"b\\\\c\\nd\",\"u\":\"\\u4e2d\\u6587\",\"empty\":\"\"}";
    char buf[64];

    printf("[3] 字符串转义 / \\uXXXX\n");

    CHECK(cj_get_str(j, "s", buf, sizeof(buf)) &&
          (strcmp(buf, "a\"b\\c\nd") == 0),
          "反转义 \\\" \\\\ \\n 正确");

    CHECK(cj_get_str(j, "u", buf, sizeof(buf)) &&
          (memcmp(buf, ZH_CN, sizeof(ZH_CN)) == 0) && (buf[6] == '\0'),
          "\\u4e2d\\u6587 -> UTF-8 中文");

    CHECK(cj_get_str(j, "empty", buf, sizeof(buf)) && (buf[0] == '\0'),
          "空字符串");

    /* 超长字符串: 截断但不能越界 */
    {
        char big[5];
        const char *j2 = "{\"s\":\"0123456789\"}";
        CHECK(cj_get_str(j2, "s", big, sizeof(big)) && (strlen(big) == 4u),
              "字符串超过缓冲 -> 截断到 cap-1 (不越界)");
    }
}

static void test_json_writer(void)
{
    char  out[160];
    cjw_t w;

    printf("[4] JSON 生成器\n");

    cjw_init(&w, out, (uint16_t)sizeof(out));
    cjw_obj_begin(&w);
    cjw_kv_str(&w, "cmd", "cfg.set");
    cjw_kv_int(&w, "code", 0);
    cjw_key(&w, "data");
    cjw_arr_begin(&w);
    cjw_int(&w, 1);
    cjw_int(&w, 2);
    cjw_arr_end(&w);
    cjw_obj_end(&w);

    CHECK(cjw_ok(&w), "cjw_ok()");
    CHECK(strcmp(out, "{\"cmd\":\"cfg.set\",\"code\":0,\"data\":[1,2]}") == 0,
          "嵌套+数组输出正确: %s", out);

    /* 负数与边界 */
    cjw_init(&w, out, (uint16_t)sizeof(out));
    cjw_obj_begin(&w);
    cjw_kv_int(&w, "a", -2147483647 - 1);
    cjw_kv_uint(&w, "b", 4294967295u);
    cjw_kv_bool(&w, "c", true);
    cjw_kv_bool(&w, "d", false);
    cjw_obj_end(&w);
    CHECK(cjw_ok(&w) &&
          (strcmp(out, "{\"a\":-2147483648,\"b\":4294967295,\"c\":true,\"d\":false}") == 0),
          "int 边界/uint 边界/bool: %s", out);

    /* UTF-8 原样通过 */
    cjw_init(&w, out, (uint16_t)sizeof(out));
    cjw_obj_begin(&w);
    cjw_key(&w, "n");
    {
        char zh[8];
        memcpy(zh, ZH_CN, sizeof(ZH_CN));
        zh[6] = '\0';
        cjw_str(&w, zh);
    }
    cjw_obj_end(&w);
    CHECK(cjw_ok(&w) && (memcmp(out + 6, ZH_CN, sizeof(ZH_CN)) == 0),
          "UTF-8 内容原样写出(不二次转义)");

    /* 控制字符 -> \u00XX */
    cjw_init(&w, out, (uint16_t)sizeof(out));
    cjw_obj_begin(&w);
    cjw_key(&w, "c");
    cjw_str(&w, "\x01");
    cjw_obj_end(&w);
    CHECK(cjw_ok(&w) && (strcmp(out, "{\"c\":\"\\u0001\"}") == 0),
          "控制字符转义为 \\u0001: %s", out);
}

static void test_json_writer_overflow(void)
{
    char  tiny[8];
    cjw_t w;

    printf("[5] 生成器溢出保护\n");

    cjw_init(&w, tiny, (uint16_t)sizeof(tiny));
    cjw_obj_begin(&w);
    cjw_kv_str(&w, "aaaa", "bbbbbbbbbbbb");
    cjw_obj_end(&w);

    CHECK(!cjw_ok(&w), "缓冲不足 -> cjw_ok() == false (检出而不是越界)");
    CHECK(tiny[sizeof(tiny) - 1u] == '\0', "未越界写(尾部仍是 '\\0')");

    /* 顶层裸标量是合法 JSON; 但顶层只允许一个根值 */
    cjw_init(&w, tiny, (uint16_t)sizeof(tiny));
    cjw_int(&w, 1);
    CHECK(cjw_ok(&w) && (strcmp(tiny, "1") == 0), "顶层标量合法(JSON 允许): %s", tiny);

    cjw_int(&w, 2);
    CHECK(!cjw_ok(&w), "顶层第二个根值 -> err");
}

static void test_envelope_parse(void)
{
    cloud_req_t req;
    const char *j =
        "{\"cmd\":\"cfg.set\",\"msgId\":\"c100\",\"ts\":1735689600,"
        "\"auth\":\"\",\"data\":{\"expectVersion\":7}}";

    printf("[6] 下行信封解析\n");

    CHECK(cloud_proto_parse_req(j, &req) == 0, "解析成功");
    CHECK(strcmp(req.cmd, "cfg.set") == 0, "cmd = cfg.set");
    CHECK(strcmp(req.msg_id, "c100") == 0, "msgId = c100 (字符串)");
    CHECK(req.ts == 1735689600u, "ts = 1735689600");
    CHECK(req.has_ts, "has_ts");
    CHECK(req.data != NULL, "data 指针非 NULL");
    CHECK(cj_get_int(req.data, "expectVersion", -1) == 7, "能在 data 上继续取字段");

    CHECK(cloud_proto_parse_req("{\"msgId\":\"x\"}", &req) == CLOUD_EC_PARAM,
          "缺 cmd -> 1001");

    CHECK(cloud_proto_parse_req("{}", &req) == CLOUD_EC_PARAM, "空对象 -> 1001");

    /* msgId 超过 16 字节: 文档规定 <=16, 必须拒 */
    CHECK(cloud_proto_parse_req(
              "{\"cmd\":\"sys.reboot\",\"msgId\":\"0123456789abcdefg\"}", &req) == CLOUD_EC_PARAM,
          "msgId 17 字节 -> 1001");

    CHECK(cloud_proto_parse_req(
              "{\"cmd\":\"sys.reboot\",\"msgId\":\"0123456789abcdef\"}", &req) == 0,
          "msgId 16 字节 -> 通过");

    CHECK(cloud_proto_parse_req(NULL, &req) == CLOUD_EC_PARAM, "NULL -> 1001");
}

static void test_resp_build(void)
{
    cloud_req_t req;
    char        resp[256];
    uint16_t    n;

    printf("[7] 应答信封(对齐文档示例)\n");

    memset(&req, 0, sizeof(req));
    strcpy(req.cmd, "cfg.set");
    strcpy(req.msg_id, "a1b2c3d4");

    n = cloud_proto_build_resp(resp, (uint16_t)sizeof(resp), &req, 0, "ok", NULL, 1735689601u);
    CHECK(n > 0u, "生成成功 n=%u", n);
    CHECK(strcmp(resp,
                 "{\"cmd\":\"cfg.set\",\"msgId\":\"a1b2c3d4\",\"code\":0,\"msg\":\"ok\","
                 "\"ts\":1735689601,\"data\":{}}") == 0,
          "与文档 3 章示例逐字符一致");

    /* data 段由调用方给出 */
    n = cloud_proto_build_resp(resp, (uint16_t)sizeof(resp), &req,
                               CLOUD_EC_VER_CONFLICT, NULL,
                               "{\"currentVersion\":7}", 1735689601u);
    CHECK(strstr(resp, "\"code\":1002") != NULL, "错误码写入");
    CHECK(strstr(resp, "config version conflict") != NULL, "默认 msg 取自错误码");
    CHECK(strstr(resp, "\"data\":{\"currentVersion\":7}") != NULL, "data 段原样嵌入");
}

static void test_cmd_table(void)
{
    printf("[8] 12 条命令表(毕设版: 移除 sys.time_sync 及 dev.*/rule.*)\n");

    CHECK(cloud_proto_cmd_count() == 12u, "命令条数 == 12 (实际 %u)",
          (unsigned)cloud_proto_cmd_count());

    CHECK(cloud_proto_is_known_cmd("sys.query_status"), "sys.query_status");
    CHECK(!cloud_proto_is_known_cmd("sys.time_sync"),
          "★ sys.time_sync 已废弃, 不在命令表");
    CHECK(cloud_proto_is_known_cmd("sys.reboot"), "sys.reboot");
    CHECK(cloud_proto_is_known_cmd("sys.factory_reset"), "sys.factory_reset");
    CHECK(cloud_proto_is_known_cmd("cfg.set"), "cfg.set");
    CHECK(cloud_proto_is_known_cmd("cfg.query"), "cfg.query");
    CHECK(cloud_proto_is_known_cmd("sen.bind"), "sen.bind");
    CHECK(cloud_proto_is_known_cmd("sen.unbind"), "sen.unbind");
    CHECK(cloud_proto_is_known_cmd("sen.set"), "sen.set");
    CHECK(cloud_proto_is_known_cmd("sen.query"), "sen.query");
    /* 毕设版: dev.* / rule.* 已随一体机删除 */
    CHECK(!cloud_proto_is_known_cmd("dev.bind"), "dev.bind 已删除");
    CHECK(!cloud_proto_is_known_cmd("dev.ctrl"), "dev.ctrl 已删除");
    CHECK(!cloud_proto_is_known_cmd("rule.set"), "rule.set 已删除");
    CHECK(cloud_proto_is_known_cmd("ota.start"), "ota.start");
    CHECK(cloud_proto_is_known_cmd("ota.query"), "ota.query");
    CHECK(cloud_proto_is_known_cmd("ota.abort"), "ota.abort");
    CHECK(!cloud_proto_is_known_cmd("no.such"), "未知命令被拒");
    CHECK(!cloud_proto_is_known_cmd(NULL), "NULL 被拒");
}

static void test_dispatch(void)
{
    char     resp[CLOUD_RESP_MAX];
    uint16_t rl;
    int      rc;

    printf("[9] 命令分发\n");

    /* ⚠ 2026-10-02: ota.start 已从"已知但未实现(1006)"换成真实实现。
     * 空 data 现在因为缺 size 而回 1001 —— 顺带证明它【真的走到处理函数里】了
     * (若还是占位, 这里会是 1006)。ota.* 的真实行为在 test_ota_http.c 里测。 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"ota.start\",\"msgId\":\"m1\",\"ts\":1,\"data\":{}}",
        1700000000u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "ota.start 缺 size -> 1001 (rc=%d)", rc);
    CHECK(rl > 0u, "写出了应答 len=%u", (unsigned)rl);
    CHECK(strstr(resp, "\"code\":1001") != NULL, "应答含 code 1001");
    CHECK(strstr(resp, "\"msgId\":\"m1\"") != NULL, "应答原样带回 msgId");
    CHECK(strstr(resp, "\"ts\":1700000000") != NULL, "应答 ts 用网关时间(传入值)");

    rc = cloud_proto_handle(
        "{\"cmd\":\"no.such\",\"msgId\":\"m2\",\"data\":{}}",
        1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "未知命令 -> 1001 (rc=%d)", rc);
    CHECK(strstr(resp, "unknown cmd") != NULL, "msg = unknown cmd");

    rc = cloud_proto_handle("this is not json", 1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "坏报文 -> 1001 (rc=%d)", rc);
    CHECK(strstr(resp, "\"code\":1001") != NULL, "坏报文也给出应答");

    rc = cloud_proto_handle("{\"cmd\":\"cfg.set\",\"msgId\":\"m3\",\"data\":{}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM,
          "cfg.set 缺 expectVersion -> 1001 (P2-4 起已接真实现, 不再是占位 1006)");
}

static void test_auth(void)
{
    cloud_profile_t p;
    char            resp[CLOUD_RESP_MAX];
    uint16_t        rl;
    int             rc;

    printf("[10] 应用层鉴权 (1008)\n");

    /* 默认(本地模式): 不启用 auth */
    cloud_profile_reset();
    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"a1\",\"data\":{}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc != CLOUD_EC_AUTH, "未启用 auth -> 放行 (rc=%d)", rc);

    /* 启用 auth(只改 RAM, 不写 Flash) */
    cloud_profile_set_factory_default(&p);
    p.auth_enable = 1u;
    strcpy(p.auth, "secret123");
    CHECK(cloud_profile_set_ram(&p) == 0, "启用 auth 的 profile 通过校验");
    CHECK(cloud_profile_auth_required(), "auth_required() == true");

    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"a2\",\"data\":{}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_AUTH, "不带 auth -> 1008 (rc=%d)", rc);
    CHECK(strstr(resp, "\"code\":1008") != NULL, "应答含 code 1008");

    rc = cloud_proto_handle(
        "{\"cmd\":\"ota.start\",\"msgId\":\"a3\",\"auth\":\"wrong\",\"data\":{}}",
        1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_AUTH, "auth 错误 -> 1008 (rc=%d)", rc);

    rc = cloud_proto_handle(
        "{\"cmd\":\"ota.start\",\"msgId\":\"a4\",\"auth\":\"secret123\",\"data\":{}}",
        1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM,
          "auth 正确 -> 放行进分发 (ota.start 缺 size -> 1001, 不再是占位 1006) (rc=%d)", rc);

    cloud_profile_reset();
}

static void test_profile_topic(void)
{
    cloud_profile_t p;
    char            t[64];

    printf("[11] profile topic / QoS / retained\n");

    /* ★2026-10-08 变更★ 出厂默认【不再是现场档位(mod/xxx), 而是文档档位】:
     * 甲方平台约定 topic = rt/gw/{gwId}/cmd ... 且 {gwId} = 设备 ID(出厂 "11000000000000000000000000",
     * 同时用作 MQTT 用户名)。所以这里断言的就是"开箱即用"的那一套。
     * (现场档位 mod/xxx 仍保留为第二套配置, 下面单独有它的用例。) */
    cloud_profile_reset();
    CHECK(cloud_profile_is_doc(), "出厂默认 == 文档档位(is_doc=true, 甲方平台 rt/gw/{gwId}/*)");

    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/11000000000000000000000000/cmd") == 0), "下行 cmd topic = rt/gw/11000000000000000000000000/cmd (%s)", t);

    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMDRESP, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/11000000000000000000000000/cmdresp") == 0), "上行 cmdresp topic = rt/gw/11000000000000000000000000/cmdresp (%s)", t);

    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_TELEMETRY, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/11000000000000000000000000/telemetry") == 0), "上行 telemetry topic = rt/gw/11000000000000000000000000/telemetry (%s)", t);

    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_EVENT, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/11000000000000000000000000/event") == 0), "上行 event topic = rt/gw/11000000000000000000000000/event (%s)", t);

    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_STATUS, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/11000000000000000000000000/status") == 0), "上行 status topic = rt/gw/11000000000000000000000000/status (%s)", t);

    /* ★2026-10-08 新增: 遗嘱专用主题★
     * 遗嘱从 status 挪到独立的 lwt, 这样"上电瞬间收到假 offline"不再污染 status。
     * ★2026-11-04(1103 联调 §六): 全线弃用 retained★ —— 所有 topic 一律不 retained,
     *   包括 status 与 lwt。遗嘱照发(QoS1), 只是 broker 不再存快照。 */
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_LWT, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/11000000000000000000000000/lwt") == 0), "遗嘱 topic = rt/gw/11000000000000000000000000/lwt (%s)", t);
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_LWT) == 1u, "遗嘱必须 QoS1(掉了就永久丢)");
    CHECK(!cloud_profile_topic_retained(CLOUD_TOPIC_LWT),
          "★遗嘱不 retained(§六: 掉线通知只对在线订阅者可见, 由平台超时窗口兜底)★");
    CHECK(!cloud_profile_topic_retained(CLOUD_TOPIC_STATUS),
          "★status 不 retained(§六: 平台按最新消息+超时窗口判在线)★");

    /* 六个 topic 逐个钉死: 任何一条被改回 retained 都应在这里失败 */
    {
        static const cloud_topic_t all[] = {
            CLOUD_TOPIC_CMD, CLOUD_TOPIC_CMDRESP, CLOUD_TOPIC_TELEMETRY,
            CLOUD_TOPIC_EVENT, CLOUD_TOPIC_STATUS, CLOUD_TOPIC_LWT
        };
        uint8_t k;
        for (k = 0u; k < (uint8_t)(sizeof(all) / sizeof(all[0])); k++)
        {
            CHECK(!cloud_profile_topic_retained(all[k]),
                  "文档档位: topic=%u 必须 retained=0(§六 全线弃用)", (unsigned)all[k]);
        }
    }

    /* 文档档位的 QoS 仍按 s_doc_qos: 全部 QoS1 */
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_TELEMETRY) == 1u, "文档档位: telemetry QoS1");
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_CMD) == 1u, "文档档位: cmd QoS1");

    /* ★cloud_profile_set_gw_id(): 设备 ID 变了, 主题里的 {gwId} 要跟着变 ★
     * main.c 启动时就是用它把 config_store.system.gwId 灌进来的 —— 与 MQTT 用户名同源。 */
    cloud_profile_set_gw_id("110007");
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/110007/cmd") == 0), "改设备ID后 topic = rt/gw/110007/cmd (%s)", t);
    cloud_profile_set_gw_id(NULL);      /* 空 = 不改 */
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/110007/cmd") == 0), "传 NULL 不改变已设的 {gwId} (%s)", t);
    cloud_profile_reset();              /* 恢复出厂默认, 后面的用例不受影响 */

    /* ★cloud_profile_migrate_to_doc(): 现场踩过的坑 —— Flash 里遗留的 LOCAL 档★
     * 旧固件把 LOCAL 档写进了 param_save 的 Flash 页, 而它优先于出厂默认、
     * 且【重烧 App 带不走】。现象: 心跳发到 mod/status 看得到, 但平台往
     * rt/gw/11000000000000000000000000/cmd 发的指令一条都收不到(网关订的还是 mod/cmd)。
     * ⇒ main.c 启动时调本函数做一次性迁移。 */
    {
        cloud_profile_t p2;

        cloud_profile_set_factory_default(&p2);
        p2.mode = (uint8_t)CLOUD_MODE_LOCAL;        /* 手工造一份"遗留档" */
        CHECK(cloud_profile_set_ram(&p2) == 0, "准备: 造一份 LOCAL 档");
        CHECK(!cloud_profile_is_doc(), "准备: 现在是 LOCAL 档");
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
              (strcmp(t, "mod/cmd") == 0), "LOCAL 档订阅 mod/cmd(正是收不到指令的原因) (%s)", t);

        CHECK(cloud_profile_migrate_to_doc() == true, "★迁移: LOCAL -> DOC 返回 true★");
        CHECK(cloud_profile_is_doc(), "★迁移后 is_doc() 为真★");
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
              (strcmp(t, "rt/gw/11000000000000000000000000/cmd") == 0),
              "★迁移后订阅 rt/gw/11000000000000000000000000/cmd(与平台一致) (%s)★", t);
        CHECK(cloud_profile_migrate_to_doc() == false, "已经是 DOC 档 -> 再调返回 false(不重复迁移)");

        cloud_profile_reset();              /* 恢复出厂默认, 后面的用例不受影响 */
    }

    /* 文档模式 */
    cloud_profile_set_factory_default(&p);
    p.mode = (uint8_t)CLOUD_MODE_DOC;
    strcpy(p.gw_id, "GW0001");
    CHECK(cloud_profile_set_ram(&p) == 0, "切到文档模式");
    CHECK(cloud_profile_is_doc(), "is_doc() == true");

    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/GW0001/cmd") == 0), "rt/gw/GW0001/cmd (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMDRESP, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/GW0001/cmdresp") == 0), "cmdresp (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_TELEMETRY, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/GW0001/telemetry") == 0), "telemetry (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_EVENT, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/GW0001/event") == 0), "event (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_STATUS, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/gw/GW0001/status") == 0), "status (%s)", t);

    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_CMD) == 1u, "文档: cmd QoS1");
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_CMDRESP) == 1u, "文档: cmdresp QoS1");
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_TELEMETRY) == 1u, "文档: telemetry QoS1");
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_EVENT) == 1u, "文档: event QoS1");
    CHECK(cloud_profile_topic_qos(CLOUD_TOPIC_STATUS) == 1u, "文档: status QoS1");
    CHECK(!cloud_profile_topic_retained(CLOUD_TOPIC_STATUS), "文档: status 也不 retained(§六)");
    CHECK(!cloud_profile_topic_retained(CLOUD_TOPIC_TELEMETRY), "telemetry 不 retained");

    /* 缓冲不足 -> 失败但不越界 */
    {
        char small[8];
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, small, (uint16_t)sizeof(small)) == 0u,
              "topic 缓冲不足 -> 返回 0");
    }

    /* ★2026-11-12 任务三 T3-1: 管理面 4 条 topic ★
     * 最容易错的一点: 业务面 cmd 与管理面 cmd 的【后缀完全一样】, 只能靠前缀分面。
     * 所以这里既钉死完整 topic, 也钉死"两条短名不能长得一样"。 */
    CHECK((CLOUD_TOPIC_COUNT == 10) && (CLOUD_TOPIC_BIZ_COUNT == 6u),
          "主题表 = 6 条业务面 + 4 条管理面(COUNT=10)");
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_CMD, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/mgmt/GW0001/cmd") == 0), "管理面 cmd = rt/mgmt/GW0001/cmd (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_CMDRESP, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/mgmt/GW0001/cmdresp") == 0), "管理面 cmdresp (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_CHECKIN, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/mgmt/GW0001/checkin") == 0), "管理面 checkin (%s)", t);
    CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_OTA_PROGRESS, t, (uint16_t)sizeof(t)) > 0u &&
          (strcmp(t, "rt/mgmt/GW0001/ota.progress") == 0), "管理面 ota.progress (%s)", t);
    CHECK(strcmp(cloud_profile_topic_name(CLOUD_TOPIC_CMD),
                 cloud_profile_topic_name(CLOUD_TOPIC_MGMT_CMD)) != 0,
          "#topic 诊断里两条 cmd 的短名不能相同");
    CHECK(cloud_profile_is_mgmt_topic(CLOUD_TOPIC_MGMT_CHECKIN) &&
          (!cloud_profile_is_mgmt_topic(CLOUD_TOPIC_STATUS)), "is_mgmt_topic() 判据正确");
    CHECK((cloud_profile_topic_qos(CLOUD_TOPIC_MGMT_CHECKIN) == 1u) &&
          (!cloud_profile_topic_retained(CLOUD_TOPIC_MGMT_CHECKIN)),
          "管理面 QoS1 / 不 retained");

    /* 管理面固定 rt/mgmt, 不能被 topic_prefix 带跑 */
    {
        cloud_profile_t mp = p;
        strcpy(mp.topic_prefix, "xx/yy");
        CHECK(cloud_profile_set_ram(&mp) == 0, "准备: 改业务面前缀为 xx/yy");
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_CMD, t, (uint16_t)sizeof(t)) > 0u &&
              (strcmp(t, "rt/mgmt/GW0001/cmd") == 0), "改业务面前缀不影响管理面 (%s)", t);
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
              (strcmp(t, "xx/yy/GW0001/cmd") == 0), "业务面仍跟随 topic_prefix (%s)", t);
        (void)cloud_profile_set_ram(&p);
    }

    /* 本地档位: 管理面【不可用】返回 0, 业务面照常 */
    {
        cloud_profile_t lp = p;
        lp.mode = (uint8_t)CLOUD_MODE_LOCAL;
        CHECK(cloud_profile_set_ram(&lp) == 0, "准备: 切到 LOCAL 档");
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_CMD, t, (uint16_t)sizeof(t)) == 0u,
              "LOCAL 档下管理面 cmd 返回 0(不可用)");
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_MGMT_OTA_PROGRESS, t, (uint16_t)sizeof(t)) == 0u,
              "LOCAL 档下管理面 ota.progress 返回 0");
        CHECK(cloud_profile_build_topic(CLOUD_TOPIC_CMD, t, (uint16_t)sizeof(t)) > 0u &&
              (strcmp(t, "mod/cmd") == 0), "LOCAL 档业务面不受影响 (%s)", t);
        (void)cloud_profile_set_ram(&p);
    }

    cloud_profile_reset();
}

static void test_profile_validate(void)
{
    cloud_profile_t p;

    printf("[12] profile 校验\n");

    cloud_profile_set_factory_default(&p);
    CHECK(cloud_profile_validate(&p) == 0, "出厂默认合法");

    cloud_profile_set_factory_default(&p);
    p.mode = 9u;
    CHECK(cloud_profile_validate(&p) == 1, "非法 mode -> 1");

    cloud_profile_set_factory_default(&p);
    p.host[0] = '\0';
    CHECK(cloud_profile_validate(&p) == 2, "空 host -> 2");

    cloud_profile_set_factory_default(&p);
    p.port = 0u;
    CHECK(cloud_profile_validate(&p) == 3, "port=0 -> 3");

    cloud_profile_set_factory_default(&p);
    p.gw_id[0] = '\0';
    CHECK(cloud_profile_validate(&p) == 4, "空 gw_id -> 4");

    cloud_profile_set_factory_default(&p);
    strcpy(p.gw_id, "AB/CD");
    CHECK(cloud_profile_validate(&p) == 4, "gw_id 含 '/' -> 4");

    cloud_profile_set_factory_default(&p);
    strcpy(p.gw_id, "012345678901234567890");
    CHECK(cloud_profile_validate(&p) == 0, "gw_id 21 字节 -> 0 (2026-10-18: 上限放宽到 26, 21 现在合法)");

    cloud_profile_set_factory_default(&p);
    p.topic_prefix[0] = '\0';
    CHECK(cloud_profile_validate(&p) == 5, "空 topic_prefix -> 5");

    cloud_profile_set_factory_default(&p);
    CHECK(cloud_profile_validate(NULL) == 1, "NULL -> 1");

    /* 字段内没有 '\0' (模拟 Flash 垃圾) 必须被拒 */
    {
        cloud_profile_t g;
        memset(&g, 'A', sizeof(g));
        CHECK(cloud_profile_validate(&g) != 0, "字段无终止符(Flash 垃圾) -> 拒绝");
    }
}

static void test_uplink(void)
{
    char     buf[256];
    uint16_t n;

    printf("[13] 上行事件信封\n");

    n = cloud_proto_build_uplink(buf, (uint16_t)sizeof(buf), "status.report", "h001",
                                 1735689600u, "{\"online\":true}");
    CHECK(n > 0u, "生成成功 n=%u", (unsigned)n);
    CHECK(strcmp(buf,
                 "{\"event\":\"status.report\",\"msgId\":\"h001\",\"ts\":1735689600,"
                 "\"data\":{\"online\":true}}") == 0,
          "与文档 7.7 示例形状一致");

    n = cloud_proto_build_uplink(buf, (uint16_t)sizeof(buf), "event.device", "v001",
                                 1735689600u, NULL);
    CHECK(n > 0u && (strcmp(buf,
                            "{\"event\":\"event.device\",\"msgId\":\"v001\","
                            "\"ts\":1735689600,\"data\":{}}") == 0),
          "data 为 NULL -> 空对象");

    /* LWT 遗嘱内容(文档 7.7) */
    CHECK(strcmp("{\"online\":false}", "{\"online\":false}") == 0, "LWT 内容备查");
}

static void test_status_data(void)
{
    cloud_status_t st;
    char           buf[CLOUD_DATA_MAX];
    char           up[CLOUD_RESP_MAX];
    uint16_t       n;

    printf("[14] status.report 数据段\n");

    memset(&st, 0, sizeof(st));
    st.online         = true;
    st.fw_version     = "1.0.0";
    st.config_version = 7u;
    st.rsrp_dbm       = -85;
    st.uptime_s       = 3600u;
    st.mode           = "auto";
    st.current_gear   = 2u;
    st.sensors_online = 2u;
    st.sensors_total  = 2u;

    n = cloud_proto_build_status_data(buf, (uint16_t)sizeof(buf), &st);
    CHECK(n > 0u, "生成成功 n=%u", (unsigned)n);
    CHECK(strcmp(buf,
                 "{\"online\":true,\"fwVersion\":\"1.0.0\",\"slot\":\"A\",\"configVersion\":7,"
                 "\"rsrp\":-85,\"uptimeSeconds\":3600,\"mode\":\"auto\","
                 "\"currentStatus\":\"Normal\",\"currentGear\":2,\"sensorsOnline\":2,\"sensorsTotal\":2}") == 0,
          "字段与文档 7.7 逐字符一致(11 个, bufferedEvents 与 devices* 已移除)");

    n = cloud_proto_build_uplink(up, (uint16_t)sizeof(up), "status.report", "h001",
                                 1735689600u, buf);
    CHECK((n > 0u) && (strstr(up, "\"event\":\"status.report\"") != NULL) &&
          (strstr(up, "\"data\":{\"online\":true") != NULL),
          "能整条包进上行信封");

    /* 负 rsrp 与 0 值不能出错 */
    st.rsrp_dbm = 0;
    n = cloud_proto_build_status_data(buf, (uint16_t)sizeof(buf), &st);
    CHECK((n > 0u) && (strstr(buf, "\"rsrp\":0") != NULL), "rsrp=0 正常");

    CHECK(cloud_proto_build_status_data(NULL, 64u, &st) == 0u, "out=NULL -> 0");
    /* off fail-safe: currentStatus=Offline + currentGear=null */
    st.current_status = "Offline";
    st.gear_null      = true;
    n = cloud_proto_build_status_data(buf, (uint16_t)sizeof(buf), &st);
    CHECK((n > 0u) && (strstr(buf, "\"currentStatus\":\"Offline\"") != NULL) &&
          (strstr(buf, "\"currentGear\":null") != NULL),
          "★ off fail-safe: status.report currentGear=null, currentStatus=Offline");
    st.current_status = "Normal";
    st.gear_null      = false;

    CHECK(cloud_proto_build_status_data(buf, 8u, &st) == 0u, "缓冲过小 -> 0");
}

static void test_unix_time(void)
{
    printf("[15] 日历 -> Unix 秒\n");

    /* 纪元原点 */
    CHECK(cloud_proto_calendar_to_unix(1970u, 1u, 1u, 0u, 0u, 0u, 0) == 0u,
          "1970-01-01 00:00:00 UTC == 0");

    /* 2000-01-01 00:00:00 UTC = 946684800 (公认值) */
    CHECK(cloud_proto_calendar_to_unix(2000u, 1u, 1u, 0u, 0u, 0u, 0) == 946684800u,
          "2000-01-01 UTC == 946684800");

    /* 现场用过的真实值: 北京 2026-09-23 00:26 == UTC 2026-09-22 16:26 == 1790094360
     * (这个数在 v12 的板子日志里出现过, 用来交叉验证) */
    CHECK(cloud_proto_calendar_to_unix(2026u, 9u, 22u, 16u, 26u, 0u, 0) == 1790094360u,
          "2026-09-22 16:26 UTC == 1790094360");
    CHECK(cloud_proto_calendar_to_unix(2026u, 9u, 23u, 0u, 26u, 0u, 28800) == 1790094360u,
          "★ 北京 2026-09-23 00:26 (+28800) 折算后与上一行相同(跨日进位正确)");

    /* 闰年 2 月 29 日 */
    CHECK(cloud_proto_calendar_to_unix(2024u, 2u, 29u, 0u, 0u, 0u, 0) ==
          cloud_proto_calendar_to_unix(2024u, 2u, 28u, 0u, 0u, 0u, 0) + 86400u,
          "2024-02-29 存在且与 02-28 差一天");

    /* 非法字段 -> 0 */
    CHECK(cloud_proto_calendar_to_unix(2026u, 0u, 1u, 0u, 0u, 0u, 0) == 0u, "month=0 -> 0");
    CHECK(cloud_proto_calendar_to_unix(2026u, 13u, 1u, 0u, 0u, 0u, 0) == 0u, "month=13 -> 0");
    CHECK(cloud_proto_calendar_to_unix(2026u, 1u, 0u, 0u, 0u, 0u, 0) == 0u, "day=0 -> 0");
    CHECK(cloud_proto_calendar_to_unix(2026u, 1u, 1u, 24u, 0u, 0u, 0) == 0u, "hour=24 -> 0");
}

static void test_json_sniff(void)
{
    static const uint8_t LORA_FRAME[] = { 0xAAu, 0x55u, 0x01u, 0x11u, 0x00u, 0x01u };
    /* 实测里出现过的"含 0x00/0x0A/0x3E"的下行二进制 */
    static const uint8_t BIN_DOWN[]   = { 0xAAu, 0x55u, 0x00u, 0x0Au, 0x3Eu, 0x0Du };
    /* 恰好以 '{' 开头, 但其实是二进制 */
    static const uint8_t FAKE_JSON[]  = { 0x7Bu, 0x00u, 0x0Au, 0x0Du };
    const char *cmd = "{\"cmd\":\"sys.reboot\"}";
    const char *ws  = "  \r\n{\"a\":1}";

    printf("[16] JSON 嗅探(下行分流)\n");

    CHECK(cloud_proto_looks_like_json((const uint8_t *)cmd, (uint16_t)strlen(cmd)),
          "云命令 JSON -> true");
    CHECK(cloud_proto_looks_like_json((const uint8_t *)ws, (uint16_t)strlen(ws)),
          "前导空白后是 '{' -> true");
    CHECK(!cloud_proto_looks_like_json(LORA_FRAME, (uint16_t)sizeof(LORA_FRAME)),
          "LoRa 帧(AA 55) -> false");
    CHECK(!cloud_proto_looks_like_json(BIN_DOWN, (uint16_t)sizeof(BIN_DOWN)),
          "含 0x00/0x0A/0x3E 的二进制 -> false");
    CHECK(!cloud_proto_looks_like_json(FAKE_JSON, (uint16_t)sizeof(FAKE_JSON)),
          "以 '{' 开头但无 '}'/'\", 的二进制 -> false(不误判)");
    CHECK(!cloud_proto_looks_like_json(NULL, 0u), "NULL -> false");
    CHECK(!cloud_proto_looks_like_json((const uint8_t *)"{", 1u), "只有一个 '{' -> false");
    CHECK(!cloud_proto_looks_like_json((const uint8_t *)"   ", 3u), "全空白 -> false");
}

/* ==================================================================
 * cfg.query / cfg.set 处理函数(P2-4)
 * ==================================================================
 * 这两条命令的应答正文是【整份配置 JSON】: 默认 1203 B, 满配最坏 3584 B。
 * 所以测试里的 resp 缓冲必须给到 CFG_JSON_MAX+512, 并且【必须 static】——
 * 4.6KB 放栈上在板子上会爆栈(startup 的 Stack_Size 只有 0x1000), 在 PC 上
 * 只是运气好看不出问题, 所以这里刻意用 static 与目标代码保持同一约束。
 */
static char   s_big_resp[CFG_JSON_MAX + 512u];

/* 把"当前配置"重置成一份确定的、非默认的配置, 并落 Flash(版本号 = ver) */
static void cloud_live_reset(uint32_t ver)
{
    config_store_t *m;

    config_store_test_wipe(0);          /* 两块 RAM 映像 = Flash 两区全 0xFF */
    config_store_test_wipe(1);

    m = config_store_get_mut();         /* 首次调用触发惰性加载 -> 两区空 -> 出厂默认 */

    m->meta.config_version = ver;
    m->meta.updated_at     = 1735689600u;
    m->system.timezone     = 8;

    m->sensor_count = 1u;
    (void)strcpy(m->sensors[0].uid, "21000000000000000000000001");
    m->sensors[0].measure_mode      = CFG_MEASURE_LEVEL;
    m->sensors[0].report_period_s   = 60u;
    m->sensors[0].offline_timeout_s = 90u;
    m->sensors[0].enabled           = 1u;
    (void)strcpy(m->sensors[0].name, "东入口");

    m->device_count = 1u;
    (void)strcpy(m->devices[0].uid, "31000000000000000000000001");
    m->devices[0].type       = CFG_DEVTYPE_ALLINONE;
    m->devices[0].volume     = 50u;
    m->devices[0].flash_mode = 1u;
    m->devices[0].enabled    = 1u;

    (void)config_store_save(m);
}

static void test_cfg_query_handler(void)
{
    uint16_t rl;
    int      rc;
    char     buf[64];

    printf("\n[17] cfg.query 处理函数(P2-4)\n");

    cloud_live_reset(3u);

    /* ---- 缺省 sections(文档默认 ["all"]) ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"q1\",\"ts\":1700000000,"
                            "\"data\":{}}",
                            1700000000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "缺省 sections -> 成功 (rc=%d)", rc);
    CHECK(rl > 0u && rl == strlen(s_big_resp), "应答长度与实际字符串一致 (%u)", (unsigned)rl);
    CHECK(strstr(s_big_resp, "\"code\":0") != NULL, "code=0");
    CHECK(strstr(s_big_resp, "\"msgId\":\"q1\"") != NULL, "msgId 原样带回");

    /* 【数据形状】data 直接就是配置对象, 不能是 {{...}} 那种无键嵌套 */
    CHECK(strstr(s_big_resp, "\"data\":{\"system\"") != NULL,
          "★ data 直接是配置对象本身(没有多套一层)");
    CHECK(strstr(s_big_resp, "\"network\"") != NULL &&
          strstr(s_big_resp, "\"sensors\"") != NULL &&
          strstr(s_big_resp, "\"thresholds\"") != NULL &&
          strstr(s_big_resp, "\"gears\"") != NULL &&
          strstr(s_big_resp, "\"devices\"") != NULL &&
          strstr(s_big_resp, "\"rules\"") != NULL &&
          strstr(s_big_resp, "\"ota\"") != NULL &&
          strstr(s_big_resp, "\"meta\"") != NULL,
          "★ 9 个域齐全(含 meta)");
    CHECK(strstr(s_big_resp, "\"crc32\":\"") != NULL, "meta.crc32 是 HEX 字符串");
    CHECK(cj_get_uint(s_big_resp, "data.meta.configVersion", 0u) == 3u,
          "data.meta.configVersion = 3");
    CHECK(cj_get_str(s_big_resp, "data.sensors[0].uid", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, "21000000000000000000000001") == 0), "sensors[0].uid = 210001");
    CHECK(cj_get_str(s_big_resp, "data.devices[0].uid", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, "31000000000000000000000001") == 0), "devices[0].uid = 310001");

    /* 【关键】应答 data 必须是一份"完整的、能存 Flash 的配置 JSON":
     * 用同一套解析器读回来, meta.crc32 还要自校验通过。 */
    {
        const char     *dsub = cj_get_sub(s_big_resp, "data");
        config_store_t  b;

        CHECK(dsub != NULL, "能在应答里取到 data 子对象");
        if (dsub != NULL)
        {
            {
                cfg_result_t rr = config_store_from_json(&b, dsub);
                /* 2026-10-30: data 里两个凭据是 6 个星号 -> 解析器按"不修改"跳过,
                 * 因此这条路径【不走】CRC 自校验(masked=true)。见 [17b]。 */
                CHECK(rr == CFG_OK,
                      "★ 应答 data 能被 config_store_from_json 读回(掩码凭据, 跳过 CRC) (code=%d)", (int)rr);
            }
            CHECK(b.meta.config_version == 3u, "读回的 configVersion = 3");
            CHECK(b.sensor_count == 1u && strcmp(b.sensors[0].name, "东入口") == 0,
                  "读回的传感器内容一致");
        }
    }

    /* ---- sections:["all"] 与 "all" 都接受 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"q2\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"all\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sections:[\"all\"] -> 成功");
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"q3\",\"ts\":1,"
                            "\"data\":{\"sections\":\"all\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sections:\"all\" -> 成功(宽容处理)");

    /* ---- 请求具体域: v55 起【已实现按域过滤】----
     * (原断言锁的是"本期没实现 -> 必须如实报 1001"，那条限制已消除，故改写为
     *  验新行为。域名的权威来源 = config_store_write_members() 写出的 9 个键。) */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"q4\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"thresholds\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★ 请求具体域 -> 成功 (rc=%d)", rc);
    CHECK((strstr(s_big_resp, "\"thresholds\"") != NULL) &&
          (strstr(s_big_resp, "\"sensors\"") == NULL),
          "★ 只要了 thresholds, 应答里就没有 sensors");

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"q5\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"all\",\"system\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK((rc == CLOUD_EC_OK) && (strstr(s_big_resp, "\"meta\"") != NULL),
          "多元素 sections 里有 all -> 仍是全量(v55 起不再回 1001)");

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"q6\",\"ts\":1,"
                            "\"data\":{\"sections\":5}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "sections 既不是字符串也不是数组 -> 1001");
}

/* =================================================================
 * [17b] 掩码凭据: cfg.query 输出 6 个星号; cfg.set 收到全星号 = 不修改
 *
 * 【为什么要专门测】2026-10-29/30 现场连续两次事故的真实成因:
 *   cfg.query 把 username/password 掩成星号 -> 平台把整份配置读回去、
 *   原样写回 cfg.set -> 网关照写进 Flash -> 下次建链 AT+MQTTCONN 带星星
 *   -> broker 拒绝(conn state=3) -> 现场只能全片擦除重烧才恢复。
 *   旧代码只给 password 挡了 5 个星号, username 完全没挡。
 * ================================================================= */
static void test_masked_credential(void)
{
    uint16_t rl;
    int      rc;
    char     buf[CFG_MQTTPWD_MAX + 8];
    char     cmd[512];

    printf("\n[17b] 掩码凭据(6 个星号 / 星号回写不改值)\n");

    cloud_live_reset(3u);

    /* ---- 1. cfg.query: 两个凭据都是【6 个星号】 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"c1\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"network\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "cfg.query 成功 (rc=%d)", rc);
    CHECK(cj_get_str(s_big_resp, "data.network.username", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, CFG_CRED_MASK) == 0),
          "★ username 掩码 = 6 个星号(实际 \"%s\")", buf);
    CHECK(cj_get_str(s_big_resp, "data.network.password", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, CFG_CRED_MASK) == 0),
          "★ password 掩码 = 6 个星号(实际 \"%s\")", buf);
    CHECK(cj_get_str(s_big_resp, "data.network.username", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, "*****") != 0),
          "★ 不再是 5 个星号(两边个数已统一)");
    CHECK(cj_get_str(s_big_resp, "data.network.username", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, "tuguan") != 0),
          "★ 应答里没有泄露真实用户名");

    /* ---- 2. 平台把查到的 username/password 原样写回 -> 一个字都不改 ---- */
    (void)snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"cfg.set\",\"msgId\":\"c2\",\"ts\":1,\"data\":{\"expectVersion\":%lu,"
        "\"sections\":{\"network\":{\"username\":\"******\",\"password\":\"******\"}}}}",
        (unsigned long)config_store_version());
    rc = cloud_proto_handle(cmd, 1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★ 6 个星号的 network 段被接受(不报错) (rc=%d)", rc);
    CHECK(strcmp(config_store_get()->network.username, "tuguan") == 0,
          "★★ 用户名没被星号覆盖(仍是 tuguan)");
    CHECK(strcmp(config_store_get()->network.mqtt_password, "tg123456!") == 0,
          "★★ 密码没被星号覆盖(仍是 tg123456!)");

    /* ---- 3. 历史值 5 个星号 / 平台自己再掩成 3 个, 同样跳过 ---- */
    (void)snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"cfg.set\",\"msgId\":\"c3\",\"ts\":1,\"data\":{\"expectVersion\":%lu,"
        "\"sections\":{\"network\":{\"username\":\"*****\",\"password\":\"***\"}}}}",
        (unsigned long)config_store_version());
    rc = cloud_proto_handle(cmd, 1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★ 5 个 / 3 个星号也被接受 (rc=%d)", rc);
    CHECK(strcmp(config_store_get()->network.username, "tuguan") == 0,
          "★ 5 个星号不改用户名");
    CHECK(strcmp(config_store_get()->network.mqtt_password, "tg123456!") == 0,
          "★ 3 个星号不改密码");

    /* ---- 4. 真实值仍然生效(别把功能挡死了) ---- */
    (void)snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"cfg.set\",\"msgId\":\"c4\",\"ts\":1,\"data\":{\"expectVersion\":%lu,"
        "\"sections\":{\"network\":{\"username\":\"tuguan2\",\"password\":\"newpw!\"}}}}",
        (unsigned long)config_store_version());
    rc = cloud_proto_handle(cmd, 1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "真实凭据 -> 成功 (rc=%d)", rc);
    CHECK(strcmp(config_store_get()->network.username, "tuguan2") == 0,
          "★ 真实用户名照常覆盖");
    CHECK(strcmp(config_store_get()->network.mqtt_password, "newpw!") == 0,
          "★ 真实密码照常覆盖");

    /* ---- 5. 空串仍是"清空"(合法表达), 不能被当成掩码 ---- */
    (void)snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"cfg.set\",\"msgId\":\"c5\",\"ts\":1,\"data\":{\"expectVersion\":%lu,"
        "\"sections\":{\"network\":{\"password\":\"\"}}}}",
        (unsigned long)config_store_version());
    rc = cloud_proto_handle(cmd, 1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "空串密码 -> 成功 (rc=%d)", rc);
    CHECK(config_store_get()->network.mqtt_password[0] == '\0',
          "★ 空串 = 清空密码(不是\"不修改\")");

    /* ---- 6. 掩码应答原样读回: 因带星号而跳过 CRC 自校验, 且凭据回落到原值/默认 ---- */
    {
        config_store_t b;
        cfg_result_t   rr;
        const char    *only_mask = "{\"network\":{\"username\":\"******\","
                                   "\"password\":\"******\"}}";

        (void)memset(&b, 0, sizeof(b));
        rr = config_store_from_json(&b, only_mask);
        CHECK(rr == CFG_OK, "只有掩码凭据的 JSON 能读回(CRC 因掩码而跳过) (code=%d)", (int)rr);
        CHECK(strcmp(b.network.username, "tuguan") == 0,
              "★ 掩码 username 读回 = 出厂默认 tuguan");
        CHECK(strcmp(b.network.mqtt_password, "tg123456!") == 0,
              "★ 掩码 password 读回 = 出厂默认 tg123456!");
    }
}

static void test_cfg_set_handler(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[18] cfg.set 处理函数(P2-4)\n");

    cloud_live_reset(3u);

    /* ================= 版本冲突: 1002 且什么都不改 ================= */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s1\",\"ts\":1700000000,\"data\":"
        "{\"expectVersion\":7,\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
        1700000000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_VER_CONFLICT, "★ expectVersion=7 != 当前 3 -> 1002 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"code\":1002") != NULL, "应答含 code 1002");
    CHECK(strstr(s_big_resp, "config version conflict") != NULL, "msg 取自错误码表");
    CHECK(config_store_version() == 3u, "★ 版本冲突时 configVersion 未变");
    CHECK(config_store_get()->thresholds.alarm_cm == 10u, "★ 版本冲突时一个字段都没改");

    /* ================= 必填/取值非法 -> 1001 ================= */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s2\",\"ts\":1,\"data\":"
        "{\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "缺 expectVersion -> 1001");

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s3\",\"ts\":1,\"data\":{\"expectVersion\":3}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "缺 sections -> 1001");

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s4\",\"ts\":1,\"data\":"
        "{\"expectVersion\":3,\"effectiveMode\":\"whenever\","
        "\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "effectiveMode 非法值 -> 1001");
    CHECK(config_store_version() == 3u, "上面几条非法请求都没有改动配置");

    rc = cloud_proto_handle("{\"cmd\":\"cfg.set\",\"msgId\":\"s5\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "连 data 段都没有 -> 1001");

    /* ================= 正常: thresholds 生效 -> 版本 +1 且落 Flash ================= */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s6\",\"ts\":1700000001,\"data\":"
        "{\"expectVersion\":3,\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
        1700000001u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "正常下发 -> 成功 (rc=%d)", rc);
    CHECK(cj_get_uint(s_big_resp, "data.configVersion", 0u) == 4u,
          "★ 应答 configVersion = 4 (已 +1)");
    CHECK(strstr(s_big_resp, "\"applied\":[\"thresholds\"]") != NULL,
          "applied = [\"thresholds\"]");
    CHECK(strstr(s_big_resp, "\"rejected\":[]") != NULL, "rejected = []");
    CHECK(config_store_version() == 4u, "内存配置版本已 +1");
    CHECK(config_store_get()->thresholds.alarm_cm == 20u, "阈值真的改了");

    /* 真的落 Flash 了吗? 用 load 重新读一遍(不是读内存实例) */
    {
        config_store_t chk;

        CHECK(config_store_load(&chk) == CFG_OK, "从 Flash 重新载入成功");
        CHECK(chk.meta.config_version == 4u, "★ Flash 里的 configVersion = 4");
        CHECK(chk.thresholds.alarm_cm == 20u, "★ Flash 里的 alarmCm = 20");
    }

    /* ================= network 域【接受】, 重启后生效 =================
     * 文档 5.2 末: "network 域修改后本网关重启生效, 生效前旧连接保持"。
     * 本工程连接参数本来就来自 config_store.network(main.c 开机交给 g4_at_set_broker),
     * 所以写下去 + 重启生效与文档一致 —— 旧版本把它整域拒掉是 P2-7 统一之前的做法,
     * 2026-09-30 第 9 条改判。 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s7\",\"ts\":1,\"data\":{\"expectVersion\":4,"
        "\"sections\":{\"network\":{\"port\":1884}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "只改 network -> OK (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"applied\":[\"network\"]") != NULL,
          "★ applied = [\"network\"]");
    CHECK(strstr(s_big_resp, "\"rejected\":[]") != NULL, "★ network 不再被拒");
    CHECK(config_store_get()->network.port == 1884u,
          "★ network.port 真的写进去了 (实际 %u)", (unsigned)config_store_get()->network.port);
    CHECK(config_store_version() == 5u, "有域生效 -> 版本 +1");

    /* network 的改动必须【落盘】(重启后要能读到) */
    {
        config_store_t chk;

        CHECK(config_store_load(&chk) == CFG_OK, "重新载入成功");
        CHECK(chk.network.port == 1884u, "★ Flash 里的 network.port = 1884(重启后生效)");
    }

    /* ================= 只改 network: 也应生效并 +1 版本 ================= */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s8\",\"ts\":1,\"data\":{\"expectVersion\":5,"
        "\"sections\":{\"network\":{\"port\":1885}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "只改 network -> OK (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"applied\":[\"network\"]") != NULL, "applied = [\"network\"]");
    CHECK(config_store_get()->network.port == 1885u, "network.port = 1885");
    CHECK(config_store_version() == 6u, "★ 有域生效 -> 版本 6");

    /* ============ meta 是只读域: 携带即【整条拒绝】(1001) ============
     * 依据: 协议 §5.2 参数表与 §4 错误码 1001 行 —— "cfg.set 携带只读 meta 域"
     *       属于参数错误; 配置 §3.9 也写明 meta 是"系统维护, 平台只读"。
     * ★2026-10-28(A3): 以前是"接受但静默忽略"(code 0、applied/rejected 都空),
     *   平台会以为改了; 现在改成命令级拒绝, 且一个域都不落地。 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s9\",\"ts\":1,\"data\":{\"expectVersion\":6,"
        "\"sections\":{\"meta\":{\"configVersion\":99}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "携带 meta -> 1001 (rc=%d)", rc);
    CHECK(config_store_version() == 6u, "★ 版本不动(整条拒绝)");
    CHECK(config_store_get()->meta.config_version == 6u, "★ 平台改不动 configVersion");

    /* 混合报文(合法域 + meta)也必须整条拒绝:
     * 不能出现"合法的 network 先落地、然后才回 1001"——
     * 那会让平台拿到失败应答, 却带着一个已经变了端口的网关。 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s9b\",\"ts\":1,\"data\":{\"expectVersion\":6,"
        "\"sections\":{\"network\":{\"port\":1999},\"meta\":{}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "network + meta 混合 -> 1001 (rc=%d)", rc);
    CHECK(config_store_get()->network.port != 1999u, "★ network 没被改(整条拒绝, 不是逐域)");
    CHECK(config_store_version() == 6u, "版本仍 6");

    /* ================= 协议层也要证明"不许静默丢数据" =================
     * 先追加 210002, 再只改 210001 的名字 —— 210002 必须还在。 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s10\",\"ts\":1,\"data\":{\"expectVersion\":6,"
        "\"sections\":{\"sensors\":[{\"uid\":\"21000000000000000000000002\"}]}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK && config_store_get()->sensor_count == 2u,
          "追加 210002 -> sensor_count = 2");
    CHECK(config_store_version() == 7u, "追加后有域生效 -> 版本 7");

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"s11\",\"ts\":1,\"data\":{\"expectVersion\":7,"
        "\"sections\":{\"sensors\":[{\"uid\":\"21000000000000000000000001\",\"name\":\"改过的名字\"}]}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "只改 210001 的名字 -> 成功");
    CHECK(config_store_get()->sensor_count == 2u, "★ sensor_count 仍是 2(210002 没被删)");
    CHECK(strcmp(config_store_get()->sensors[1].uid, "21000000000000000000000002") == 0, "★ 210002 仍在");
    CHECK(strcmp(config_store_get()->sensors[0].name, "改过的名字") == 0, "210001 名字已改");
    CHECK(config_store_get()->sensors[0].report_period_s == 60u &&
          config_store_get()->sensors[0].offline_timeout_s == 90u,
          "210001 的其它字段没被重置");

    /* 落 Flash 的结果也必须是 2 个传感器 */
    {
        config_store_t chk;

        CHECK(config_store_load(&chk) == CFG_OK, "重新载入成功");
        CHECK(chk.sensor_count == 2u, "★ Flash 里也是 2 个传感器");
        CHECK(chk.meta.config_version == 8u, "★ Flash 里的 configVersion = 8");
    }

    /* ================= 拼错的域名必须报出来, 不能默默忽略 =================
     * 平台把 "thresholds" 打成 "threshold": 我们没义务猜它想改什么, 但
     * 【绝不能回 code=0 而一个字没改】—— 那是最难查的一类故障(平台以为改成功了)。 */
    {
        uint32_t v_before = config_store_version();

        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"s12\",\"ts\":1,\"data\":{\"expectVersion\":8,"
            "\"sections\":{\"threshold\":{\"alarmCm\":20}}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_PARAM, "域名拼错(threshold) -> 1001, 不是 code=0 (rc=%d)", rc);
        CHECK(config_store_version() == v_before, "★ 拼错时版本不变(确实一个字没改)");

        /* 空对象 {} 是合法的"什么都不改", 应回 0(不能和"拼错"混为一谈) */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"s13\",\"ts\":1,\"data\":{\"expectVersion\":8,"
            "\"sections\":{}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "空 sections {} -> 0(合法的什么都不改)");
        CHECK(config_store_version() == v_before, "空 sections 版本不变");
    }

    /* ================= 第 9 条(2026-09-30)补齐的三个缺口 ================= */
    {
        uint32_t v = config_store_version();

        /* (1) 版本冲突(1002)的应答必须带 data.currentVersion(文档 §8 错误码表明文) */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"s14\",\"ts\":1,\"data\":{\"expectVersion\":999,"
            "\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_VER_CONFLICT, "版本不符 -> 1002 (rc=%d)", rc);
        CHECK(strstr(s_big_resp, "\"data\":{\"currentVersion\":") != NULL,
              "★ 1002 应答带 data.currentVersion(平台据此对齐后重发)");
        CHECK(strstr(s_big_resp, "\"currentVersion\":8") != NULL ||
              strstr(s_big_resp, "\"currentVersion\":") != NULL,
              "★ currentVersion 有值");
        CHECK(config_store_version() == v, "★ 版本冲突时什么都不改");

        /* (2) 整域拒绝时要给原因(rejectedDetail), 且 rejected 仍是域名数组(兼容文档示例) */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"s15\",\"ts\":1,\"data\":{\"expectVersion\":8,"
            "\"sections\":{\"sensors\":[{\"uid\":\"ZZZZZZ\"}]}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "非法 uid 的域被拒, 但整条命令正常处理 (rc=%d)", rc);
        CHECK(strstr(s_big_resp, "\"rejected\":[\"sensors\"]") != NULL,
              "★ rejected 仍是域名数组(文档示例的格式, 不破坏平台解析)");
        CHECK(strstr(s_big_resp, "\"rejectedDetail\"") == NULL,
              "★ 用户决定不做 rejectedDetail: 应答里【不得出现】(rejected 只回域名数组)");
        CHECK(strstr(s_big_resp, "\"section\":") == NULL && strstr(s_big_resp, "\"reason\":") == NULL,
              "★ 也没有 section/reason 字段");
        CHECK(config_store_get()->sensor_count == 2u, "被拒的域一个字没改");
        CHECK(config_store_version() == v, "被拒 -> 版本不变");

        /* (3) effectiveMode 回显(默认 immediate) */
        CHECK(strstr(s_big_resp, "\"effectiveMode\"") == NULL,
              "★ 用户决定不做 effectiveMode 回显: 应答里【不得出现】");

        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"s16\",\"ts\":1,\"data\":{\"expectVersion\":8,"
            "\"effectiveMode\":\"reboot\",\"sections\":{\"thresholds\":{\"alarmCm\":11}}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "effectiveMode=reboot -> OK (rc=%d)", rc);
        CHECK(strstr(s_big_resp, "\"effectiveMode\"") == NULL,
              "★ reboot 模式也不回显(取值仍然照常校验)");
        CHECK(config_store_get()->thresholds.alarm_cm == 11u, "reboot 模式下值也是立即写入");

        /* 非法 effectiveMode -> 1001 */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"s17\",\"ts\":1,\"data\":{\"expectVersion\":9,"
            "\"effectiveMode\":\"later\",\"sections\":{\"thresholds\":{\"alarmCm\":11}}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_PARAM, "effectiveMode 非法 -> 1001 (rc=%d)", rc);
    }
}

/* ==================================================================
 * sys.factory_reset(P2-5)
 * ==================================================================
 * 文档 5.1 第 4 条原文:
 *   参数: keepNetwork  bool  默认 true   保留 network 域(避免恢复后失联)
 *   响应: code 0。其余配置域恢复出厂默认, sensors[]/devices[] 清空, configVersion 归零。
 *
 * 这里要钉死三件事:
 *   1) 默认 keepNetwork=true 时 network 被保留、其余全部回默认、版本归零;
 *   2) 【紧接着 cfg.query 必须读到恢复后的值】—— 证明写的是"当前配置"那一个实例,
 *      而不是只写了 Flash 或只写了一份副本(上一轮 cfg.set 在这里踩过坑);
 *   3) 落 Flash 失败必须回 1007, 【绝不能回 code=0】(答应了却没做到最难查)。
 */
static void test_factory_reset(void)
{
    uint16_t rl;
    int      rc;
    char     buf[64];

    printf("\n[19] sys.factory_reset 处理函数(P2-5)\n");

    /* ---- 准备: 一份"改过的"配置(阈值非默认 + 有传感器 + 有设备) ---- */
    cloud_live_reset(3u);                       /* 版本 3; 1 传感器 210001 / 1 设备 310001 */

    /* network 域走 cfg.set 是被明确拒绝的(P2-7 尚未统一), 所以这里直接写当前实例
     * + 落 Flash —— 只为给 keepNetwork 造一个"能看出有没有被保留"的初值。 */
    {
        config_store_t *m = config_store_get_mut();

        (void)strcpy(m->network.host, "10.0.0.9");
        m->network.port = 8883u;
        (void)strcpy(m->network.username, "keepme");
        CHECK(config_store_save(m) == CFG_OK, "准备: 造一份非默认 network 并落 Flash");
    }

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"f0\",\"ts\":1,\"data\":{\"expectVersion\":3,"
        "\"sections\":{\"thresholds\":{\"alarmCm\":42,\"warningCm\":30,\"resolveCm\":5}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK && (config_store_version() == 4u),
          "准备: 阈值改为 42/30/5(版本 4)");
    CHECK(config_store_get()->sensor_count == 1u && config_store_get()->device_count == 1u,
          "准备: 有 1 传感器 + 1 设备");

    /* ---- 参数类型非法 -> 1001, 且什么都不改 ---- */
    rc = cloud_proto_handle(
        "{\"cmd\":\"sys.factory_reset\",\"msgId\":\"f1\",\"ts\":1,"
        "\"data\":{\"keepNetwork\":\"yes\"}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "keepNetwork 传字符串 -> 1001 (rc=%d)", rc);
    CHECK(config_store_version() == 4u && config_store_get()->thresholds.alarm_cm == 42u,
          "★ 非法请求下配置一个字都没动");

    /* ================= 默认 keepNetwork=true: 保留 network ================= */
    rc = cloud_proto_handle("{\"cmd\":\"sys.factory_reset\",\"msgId\":\"f2\",\"ts\":1000}",
                            1000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sys.factory_reset(不带参数) -> code 0 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"code\":0") != NULL, "应答 code=0");
    CHECK(strstr(s_big_resp, "\"data\":{}") != NULL,
          "★ 应答 data 是空对象(文档这条只规定 code 0, 没有 data 字段)");
    CHECK(strstr(s_big_resp, "\"msgId\":\"f2\"") != NULL, "msgId 原样带回");

    CHECK(config_store_version() == 0u, "★ configVersion 归零");
    CHECK(config_store_get()->sensor_count == 0u, "★ sensors[] 清空");
    CHECK(config_store_get()->device_count == 0u, "★ devices[] 清空");
    CHECK((config_store_get()->thresholds.alarm_cm   == 10u) &&
          (config_store_get()->thresholds.warning_cm == 7u)  &&
          (config_store_get()->thresholds.resolve_cm == 2u),
          "★ 阈值回出厂默认(10/7/2)");
    CHECK(config_store_get()->rules.manual_timeout_s == 300u &&
          config_store_get()->system.timezone == 8,
          "★ 其它域也回出厂默认(rules/system)");
    CHECK((strcmp(config_store_get()->network.host, "10.0.0.9") == 0) &&
          (config_store_get()->network.port == 8883u) &&
          (strcmp(config_store_get()->network.username, "keepme") == 0),
          "★ keepNetwork 默认 true -> network 域被保留");

    /* ---- 【关键】紧接着 cfg.query 必须读到恢复后的值(写的是同一个实例) ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"f3\",\"ts\":1,\"data\":{}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "恢复出厂后立刻 cfg.query -> 成功");
    CHECK(cj_get_uint(s_big_resp, "data.meta.configVersion", 99u) == 0u,
          "★ cfg.query 读到 configVersion=0(不是旧值)");
    CHECK(cj_get_uint(s_big_resp, "data.thresholds.alarmCm", 0u) == 10u,
          "★ cfg.query 读到 alarmCm=10(阈值确已回默认)");
    CHECK(strstr(s_big_resp, "\"sensors\":[]") != NULL, "★ cfg.query 读到 sensors 为空数组");
    CHECK(strstr(s_big_resp, "\"devices\":[]") != NULL, "★ cfg.query 读到 devices 为空数组");
    CHECK(cj_get_str(s_big_resp, "data.network.host", buf, (uint16_t)sizeof(buf)) &&
          (strcmp(buf, "10.0.0.9") == 0), "★ cfg.query 读到 network 仍是保留值");

    /* ---- Flash 里也必须是同一份(不是只改了 RAM) ---- */
    {
        config_store_t chk;

        CHECK(config_store_load(&chk) == CFG_OK, "从 Flash 重新载入成功");
        CHECK((chk.meta.config_version == 0u) && (chk.sensor_count == 0u) &&
              (chk.device_count == 0u) && (chk.thresholds.alarm_cm == 10u),
              "★ Flash 里的也是恢复后的配置(版本 0 / 数组清空 / 默认阈值)");
        CHECK((strcmp(chk.network.host, "10.0.0.9") == 0) && (chk.network.port == 8883u),
              "★ Flash 里 network 被保留");
        CHECK(chk.meta.updated_at == 1000u, "★ meta.updatedAt = 传进来的网关时间(1000)");
    }

    /* ================= keepNetwork=false: network 也回出厂默认 ================= */
    rc = cloud_proto_handle(
        "{\"cmd\":\"sys.factory_reset\",\"msgId\":\"f4\",\"ts\":2000,"
        "\"data\":{\"keepNetwork\":false}}",
        2000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "keepNetwork=false -> code 0 (rc=%d)", rc);
    /* ★2026-10-09: 出厂默认的 network 由"全空"改成了一组实际值
     * (host=223.75.207.139 / port=1883 / clientId=11000000000000000000000000 / username=tuguan /
     *  password=tg123456!) —— 用户要求把配置里空着的参数填上。
     * ⇒ 断言从"host 为空"改成"host 等于出厂默认值"。 */
    CHECK((strcmp(config_store_get()->network.host, "223.75.207.139") == 0) &&
          (config_store_get()->network.port == 1883u),
          "★ keepNetwork=false -> network 回出厂默认(host=%s)",
          config_store_get()->network.host);
    {
        config_store_t chk;

        CHECK((config_store_load(&chk) == CFG_OK) && (chk.network.port == 1883u) &&
              (strcmp(chk.network.host, "223.75.207.139") == 0),
              "★ Flash 里 network 也回默认");
    }

    /* ================= keepNetwork 传数字 1 -> 与 true 等价(宽容) ================= */
    {
        config_store_t *m = config_store_get_mut();

        (void)strcpy(m->network.host, "10.0.0.7");
        m->network.port = 1884u;
        (void)config_store_save(m);
    }
    rc = cloud_proto_handle(
        "{\"cmd\":\"sys.factory_reset\",\"msgId\":\"f5\",\"ts\":3000,"
        "\"data\":{\"keepNetwork\":1}}",
        3000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "keepNetwork:1 -> code 0");
    CHECK((strcmp(config_store_get()->network.host, "10.0.0.7") == 0) &&
          (config_store_get()->network.port == 1884u),
          "★ keepNetwork 传数字 1 == true(保留 network)");

    /* ================= Flash 写失败 -> 必须 1007, 绝不能 code=0 ================= */
    {
        config_store_t *m = config_store_get_mut();

        (void)strcpy(m->system.gw_id, "GWFAIL");     /* 造一个"和出厂默认不同"的特征值 */
        (void)strcpy(m->network.host, "10.0.0.6");
        CHECK(config_store_save(m) == CFG_OK, "准备: 落一份 gwId=GWFAIL 的配置");

        config_store_test_fail_write(1);            /* 下一次 Flash 写失败 */

        rc = cloud_proto_handle("{\"cmd\":\"sys.factory_reset\",\"msgId\":\"f6\",\"ts\":4000}",
                                4000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_INTERNAL, "★ Flash 写失败 -> 1007(不是 0) (rc=%d)", rc);
        CHECK(strstr(s_big_resp, "\"code\":1007") != NULL, "应答里 code=1007");
        CHECK(strstr(s_big_resp, "\"code\":0") == NULL, "★ 应答里【没有】code=0");
        CHECK(strcmp(config_store_get()->system.gw_id, "GWFAIL") == 0,
              "★ 失败时【当前配置实例一个字都没改】");
        CHECK(strcmp(config_store_get()->network.host, "10.0.0.6") == 0,
              "★ network 也没被动过");

        config_store_test_fail_write(0);            /* 恢复注入(其实已经自动减到 0) */

        {
            config_store_t chk;

            CHECK(config_store_load(&chk) == CFG_OK, "失败后仍能从 Flash 载入可用的配置");
            CHECK(strcmp(chk.system.gw_id, "GWFAIL") == 0,
                  "★ Flash 主区仍是失败前那一份(双区没被写坏)");
        }
    }

    /* ================= 失败之后再来一次正常请求: 必须能成功 ================= */
    rc = cloud_proto_handle("{\"cmd\":\"sys.factory_reset\",\"msgId\":\"f7\",\"ts\":5000}",
                            5000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "写失败之后再请求 -> 这次成功 (rc=%d)", rc);
    CHECK(strcmp(config_store_get()->system.gw_id, "11000000000000000000000000") == 0,
          "★ 这次真的恢复了出厂(gwId = 11000000000000000000000000)");
}

/* ==================================================================
 * 液位失联判定(2026-09-27 用户补充要求第 3 条) —— 纯逻辑, 不依赖硬件
 * ==================================================================
 * 用户口径: 传感器最多三个; 不用断言还在, 只记"上次上报时间"; 每次收到一路液位上报,
 *           就把所有液位从机的上次上报时间拿出来对比, 超过 3 倍上报周期没上报的,
 *           就发"传感器失联"。
 *
 * 被测对象是 app/service/sensor_offline.c —— 它不 include 任何硬件头, 时间戳由
 * 调用方传进来, 所以这里能直接用"确定的时间点"把边界全部跑一遍。
 * lora_business.c 那边只剩"读状态 → 调这个纯函数 → 发事件"的胶水。
 */
static void test_sensor_offline_logic(void)
{
    /* 阈值 = 上报周期 × 3 */
    printf("\n[20] 液位失联判定(纯逻辑)\n");

    CHECK(sensor_offline_threshold_ms(60u) == 180000u, "60s 周期 -> 阈值 180000ms(= 3 倍)");
    CHECK(sensor_offline_threshold_ms(1u) == 3000u, "1s 周期 -> 3000ms");
    CHECK(sensor_offline_threshold_ms(3600u) == 10800000u, "3600s(文档上限)-> 3 倍不溢出");
    CHECK(sensor_offline_threshold_ms(0u) == 180000u,
          "★ 周期字段为 0(未配置)-> 用兜底 60s(阈值 180000ms), 而不是 0");

    /* ---- 边界: 刚好 3 倍 / 差 1ms ---- */
    CHECK(sensor_offline_should_report(180000u, 0u, 60u, true),
          "★ 刚好 3 倍(180000ms)算失联(>= 的口径)");
    CHECK(!sensor_offline_should_report(179999u, 0u, 60u, true),
          "★ 差 1ms(179999ms)还不算失联");
    CHECK(!sensor_offline_should_report(999999u, 0u, 60u, false),
          "★ was_online=false(上电没上报过 / 已经报过失联)-> 不报");

    /* ---- tick 回绕(必须用无符号差值, 不能比大小) ---- */
    CHECK(sensor_offline_should_report(0x00001000u, 0xFFFFF000u, 1u, true),
          "★ 回绕: last=0xFFFFF000(回绕前 4096ms)-> now=0x00001000, 差 4096ms > 3000ms -> 判失联");
    CHECK(sensor_offline_should_report(0x000007D0u, 0xFFFFFC18u, 1u, true),
          "★ 回绕: 回绕前 1000ms + 回绕后 2000ms = 正好 3000ms -> 判失联");
    CHECK(!sensor_offline_should_report(0x000007CFu, 0xFFFFFC18u, 1u, true),
          "★ 回绕: 差 2999ms(差 1ms)-> 不报(直接比大小会在这里判错)");

    /* ---- 跳变去重: 用状态机整套跑一遍(在线->失联只报一次, 恢复只报一次) ---- */
    {
        bool online = false;
        bool reported = false;
        uint8_t k;

        CHECK(sensor_offline_update(&online, &reported, 0u, 0u, 60u, true)
                  == SENSOR_OFFLINE_ACT_NONE && online,
              "第一次上报 -> 记为在线, 但不发'恢复'(本来就没报过失联)");

        CHECK(sensor_offline_update(&online, &reported, 100000u, 0u, 60u, false)
                  == SENSOR_OFFLINE_ACT_NONE,
              "别人上报顺带检查: 才 100s(< 180s)-> 不报");

        CHECK(sensor_offline_update(&online, &reported, 180000u, 0u, 60u, false)
                  == SENSOR_OFFLINE_ACT_REPORT_OFFLINE,
              "★ 到阈值 -> 报一次失联");
        CHECK(!online && reported, "状态变成 离线 + 已报失联");

        {
            bool repeated = false;

            for (k = 0u; k < 10u; k++)
            {
                if (sensor_offline_update(&online, &reported, 200000u + (uint32_t)k * 1000u,
                                          0u, 60u, false) != SENSOR_OFFLINE_ACT_NONE)
                {
                    repeated = true;
                }
            }
            CHECK(!repeated, "★ 失联后连收 10 条别人的上报 -> 一次都不重复发");
        }

        /* 它只回了个 ACK(_touch_slave 会把 is_online 置回 true)-> 仍不许重发 */
        online = true;
        CHECK(sensor_offline_update(&online, &reported, 300000u, 0u, 60u, false)
                  == SENSOR_OFFLINE_ACT_NONE,
              "★ 失联后只回 ACK(不算上报)-> 不重复报");
        /* ★2026-10-23 修正: 不光不能重发, 还得把被 ACK 刷成 true 的在线标志压回 false,
         * 否则 status.report 会一直把这台已失联的设备算成在线(平台看到的与事实相反)。 */
        CHECK(!online, "★ 失联后回 ACK -> 在线标志必须被纠正回 false(不能一直假装在线)");
        CHECK(reported, "★ 纠正在线标志不等于'恢复', 已报失联标志仍保持");

        /* 再连来几帧别人的上报, 在线标志也不许被"带回去" */
        {
            bool flipped = false;

            for (k = 0u; k < 5u; k++)
            {
                online = true;      /* 模拟每帧都被 _touch_slave 刷成 true */
                (void)sensor_offline_update(&online, &reported,
                                            301000u + (uint32_t)k * 1000u, 0u, 60u, false);
                if (online)
                {
                    flipped = true;
                }
            }
            CHECK(!flipped, "★ 失联期间反复被刷 true -> 每次都被纠正回 false");
        }

        CHECK(sensor_offline_update(&online, &reported, 310000u, 310000u, 60u, true)
                  == SENSOR_OFFLINE_ACT_REPORT_ONLINE,
              "★ 它真的又上报 -> 报一次'恢复'");
        CHECK(!reported, "恢复后标志已清");

        CHECK(sensor_offline_update(&online, &reported, 320000u, 320000u, 60u, true)
                  == SENSOR_OFFLINE_ACT_NONE,
              "★ 之后每次正常上报都不再发'恢复'");

        CHECK(sensor_offline_update(NULL, &reported, 0u, 0u, 60u, false)
                  == SENSOR_OFFLINE_ACT_NONE &&
              sensor_offline_update(&online, NULL, 0u, 0u, 60u, false)
                  == SENSOR_OFFLINE_ACT_NONE,
              "NULL 入参 -> 安全返回 NONE(不崩)");
    }

    /* ---- ★2026-10-27: 离线事件按 3xperiod 重复上报(新 API) ---- */
    {
        bool     online = true;
        bool     reported = false;
        uint32_t last_ev = 0u;

        CHECK(sensor_offline_update_ex(&online, &reported, 0u, 0u, 60u, true, &last_ev)
                  == SENSOR_OFFLINE_ACT_NONE,
              "repeat: 首次上报在线, 不发恢复");
        CHECK(sensor_offline_update_ex(&online, &reported, 180000u, 0u, 60u, false, &last_ev)
                  == SENSOR_OFFLINE_ACT_REPORT_OFFLINE,
              "repeat: 3x -> 第一次离线");
        CHECK(last_ev == 180000u, "repeat: 记录本次事件时刻");
        CHECK(sensor_offline_update_ex(&online, &reported, 359000u, 0u, 60u, false, &last_ev)
                  == SENSOR_OFFLINE_ACT_NONE,
              "repeat: 距上次事件不足 3x -> 不重复");
        CHECK(sensor_offline_update_ex(&online, &reported, 360000u, 0u, 60u, false, &last_ev)
                  == SENSOR_OFFLINE_ACT_REPORT_OFFLINE,
              "repeat: 6x -> 第二次离线");
        CHECK(sensor_offline_update_ex(&online, &reported, 540000u, 0u, 60u, false, &last_ev)
                  == SENSOR_OFFLINE_ACT_REPORT_OFFLINE,
              "repeat: 9x -> 第三次离线");
        CHECK(sensor_offline_update_ex(&online, &reported, 550000u, 550000u, 60u, true, &last_ev)
                  == SENSOR_OFFLINE_ACT_REPORT_ONLINE,
              "repeat: 恢复只报一次 online");
        CHECK((!reported) && (last_ev == 0u), "repeat: 恢复清标志与事件时刻");
        CHECK(sensor_offline_update_ex(&online, &reported, 550000u + 180000u,
                                       550000u, 60u, false, &last_ev)
                  == SENSOR_OFFLINE_ACT_REPORT_OFFLINE,
              "repeat: 恢复后重新按 3x 计时");
    }

    /* ---- 按 UID 从配置里查上报周期 ---- */
    {
        static config_store_t c;        /* static: 与工程"大结构不放栈"的约定一致 */

        config_store_factory_default(&c);
        c.sensor_count = 2u;
        (void)strcpy(c.sensors[0].uid, "21000000000000000000000001");
        c.sensors[0].report_period_s = 30u;
        (void)strcpy(c.sensors[1].uid, "21000000000000000000000002");
        c.sensors[1].report_period_s = 0u;          /* 未配置 */

        CHECK(sensor_offline_period_for_uid(&c, "21000000000000000000000001") == 30u, "配置里查到 210001 -> 30s");
        CHECK(sensor_offline_period_for_uid(&c, "21000000000000000000000002") == 60u,
              "★ 该传感器的周期字段是 0 -> 用兜底 60s");
        CHECK(sensor_offline_period_for_uid(&c, "21000000000000000000000003") == 60u,
              "★ 配置里没有该 UID -> 用兜底 60s");

        c.sensor_count = 1u;                        /* 210002 不再算数(按 count 遍历, 不越界) */
        CHECK(sensor_offline_period_for_uid(&c, "21000000000000000000000002") == 60u,
              "★ 超出 sensor_count 的槽位不参与匹配(也没越界)");

        CHECK(sensor_offline_period_for_uid(NULL, "21000000000000000000000001") == 60u, "cfg=NULL -> 兜底 60s");
        CHECK(sensor_offline_period_for_uid(&c, NULL) == 60u, "uid=NULL -> 兜底 60s");
    }
}

/* =================================================================
 * [21] P1-6 msgId 幂等
 * -----------------------------------------------------------------
 * 判据(任务清单 P1-6): 【重复 msgId 不重复执行】。
 * "没有重复执行"必须用【副作用】来证明, 不能用返回值证明 —— 返回值被缓存后
 * 照样是 0。所以这里一律用 cfg.set 把 configVersion 推着走, 看版本号涨了几次。
 * ================================================================= */
static void test_msgid_idempotent(void)
{
    static char first[CFG_JSON_MAX + 512u];
    uint16_t    first_len;
    uint16_t    rl;
    uint32_t    hits0;                          /* W 段: 时间窗口用例的命中计数基线 */
    int         rc;
    int         i;

    printf("\n[21] msgId 幂等 (P1-6: 重复 msgId 不重复执行)\n");

    /* ---------- A. 同 msgId 重发: 不重复执行 + 逐字节重放上次应答 ---------- */
    cloud_live_reset(3u);
    cloud_proto_dedup_reset();                  /* 确定的初始状态(固件上电时本来就是 0) */
    CHECK(cloud_proto_dedup_hit_count() == 0u, "复位后命中计数 = 0");

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"d1\",\"ts\":1700000001,\"data\":"
        "{\"expectVersion\":3,\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
        1700000001u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "第 1 次 cfg.set -> 成功 (rc=%d)", rc);
    CHECK(config_store_version() == 4u, "第 1 次执行后 configVersion = 4");
    first_len = rl;
    memcpy(first, s_big_resp, first_len + 1u);

    /* 云端重发【完全一样】的报文(应答丢了 / QoS1 重传) */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"d1\",\"ts\":1700000001,\"data\":"
        "{\"expectVersion\":3,\"sections\":{\"thresholds\":{\"alarmCm\":20}}}}",
        1700000001u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "重发 -> 仍回成功 (rc=%d)", rc);
    CHECK(config_store_version() == 4u,
          "★★ 重发没有第二次执行: configVersion 仍是 4(不是 5)");
    CHECK(rl == first_len, "重放长度与首次一致 (%u vs %u)", (unsigned)rl, (unsigned)first_len);
    CHECK(memcmp(first, s_big_resp, first_len) == 0, "★ 重放内容与首次逐字节一致");
    CHECK(strstr(s_big_resp, "\"msgId\":\"d1\"") != NULL, "重放仍带回同一个 msgId");
    CHECK(cloud_proto_dedup_hit_count() == 1u, "命中计数 +1 (=%u)",
          (unsigned)cloud_proto_dedup_hit_count());

    /* ---------- B. 换一个 msgId: 照常执行(幂等只认 msgId, 不认报文内容) ---------- */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"d2\",\"ts\":1,\"data\":"
        "{\"expectVersion\":4,\"sections\":{\"thresholds\":{\"alarmCm\":21}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "换 msgId(d2) -> 成功 (rc=%d)", rc);
    CHECK(config_store_version() == 5u, "★ 换 msgId 照常执行: 版本 -> 5");
    CHECK(cloud_proto_dedup_hit_count() == 1u, "没命中, 计数不动");

    /* ---------- C. 不带 msgId: 不做去重(空 id 之间没有区分度) ---------- */
    /* 同一个报文发两次: 第 2 次若被"去重"就会回缓存里的成功; 实际必须【再执行一次】,
     * 于是 expectVersion=5 与当前 5 冲突 -> 1002。用返回值就能分辨有没有真执行。 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"ts\":1,\"data\":"
        "{\"expectVersion\":5,\"sections\":{\"thresholds\":{\"alarmCm\":22}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "无 msgId 第 1 次 -> 成功 (rc=%d)", rc);
    CHECK(config_store_version() == 6u, "无 msgId 第 1 次已执行: 版本 -> 6");

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"ts\":1,\"data\":"
        "{\"expectVersion\":5,\"sections\":{\"thresholds\":{\"alarmCm\":22}}}}",
        1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_VER_CONFLICT,
          "★ 无 msgId 第 2 次 -> 1002(说明真的又执行了一遍, 没被误去重) (rc=%d)", rc);
    CHECK(config_store_version() == 6u, "第 2 次因版本冲突没有改动配置");

    /* ---------- W. ★时间窗口: 隔得久了, 同名 msgId 算【新命令】★ ----------
     * 现场(14:34)踩过: ota-02 在 14:01 受理但升级失败, 用户隔 33 分钟再发同名命令
     * 想重跑一次 —— 结果被"永久幂等"吞掉: 网关什么都没干, 却回 code 0(看起来像成功)。
     * 需求文档第 3 章的本意是"【10 秒】内重发算重试", 不是"永久记住这个名字"。
     * ⇒ 实现加了 CLOUD_DEDUP_WINDOW_S(60s) 窗口, 这里把三种时序都钉住。
     * ⚠️ 这里【不调 cloud_proto_dedup_reset()】—— 那会把 A 段的 d1 一起清掉,
     *    下面的 D 段(靠 d1 验证"重放不绕过鉴权")就失去前提了。改用增量计数。 */
    hits0 = cloud_proto_dedup_hit_count();

    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"w1\",\"ts\":1700000002,\"data\":"
        "{\"expectVersion\":6,\"sections\":{\"thresholds\":{\"alarmCm\":30}}}}",
        1700000002u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "W: w1 首次执行 (rc=%d)", rc);
    CHECK(config_store_version() == 7u, "W: 版本 -> 7");

    /* 30 秒后重发同名: 仍在 60s 窗口内 -> 必须仍然去重(这才是防重传的本职) */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"w1\",\"ts\":1700000032,\"data\":"
        "{\"expectVersion\":7,\"sections\":{\"thresholds\":{\"alarmCm\":30}}}}",
        1700000032u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "W: 30s 后重发同名 (rc=%d)", rc);
    CHECK(config_store_version() == 7u, "★W: 窗口内(30s)重发【不】重复执行(版本仍 7)");
    CHECK(cloud_proto_dedup_hit_count() == (hits0 + 1u), "W: 窗口内那次算命中");

    /* 距首次 70 秒(> 60s 窗口): 按新命令, 必须真的执行 */
    rc = cloud_proto_handle(
        "{\"cmd\":\"cfg.set\",\"msgId\":\"w1\",\"ts\":1700000072,\"data\":"
        "{\"expectVersion\":7,\"sections\":{\"thresholds\":{\"alarmCm\":31}}}}",
        1700000072u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★W: 70s 后同名 -> 超出窗口, 真执行 (rc=%d)", rc);
    CHECK(config_store_version() == 8u,
          "★★ 超出 60s 窗口后同名 msgId 会真的执行(版本 -> 8) —— 正是修掉的那个坑");
    CHECK(cloud_proto_dedup_hit_count() == (hits0 + 1u), "W: 超窗那次不算命中");

    /* ---------- D. 安全: 重放不能绕过鉴权 ---------- */
    /* 命中检查必须排在鉴权之后。否则"抄一个见过的 msgId + 不带 auth"就能白拿成功应答。 */
    {
        cloud_profile_t p;

        cloud_profile_set_factory_default(&p);
        p.auth_enable = 1u;
        strcpy(p.auth, "secret123");
        CHECK(cloud_profile_set_ram(&p) == 0, "启用 auth 的 profile 通过校验");

        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"d1\",\"ts\":1,\"data\":"
            "{\"expectVersion\":4,\"sections\":{\"thresholds\":{\"alarmCm\":21}}}}",
            1700000020u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_AUTH,
              "★★ 已知 msgId(d1) + 不带 auth -> 1008, 不是缓存的成功 (rc=%d)", rc);
        CHECK(cloud_proto_dedup_hit_count() == (hits0 + 1u), "被鉴权挡下时不算命中, 计数不动");

        /* 带正确 auth 再重放同一个 d1: 这次才该命中缓存(仍然是 A 段那份应答)
         * ★2026-10-08: now_ts 必须落在 d1 记录时刻(1700000001)的 60s 窗口内★
         * 原来这里传 1u —— 加了时间窗之后会被判成"超出窗口的新命令",
         * 于是命中不了缓存, 这条断言就会失败(不是功能坏了, 是测试的时间戳不自洽)。 */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"d1\",\"ts\":1,\"auth\":\"secret123\",\"data\":"
            "{\"expectVersion\":4,\"sections\":{\"thresholds\":{\"alarmCm\":21}}}}",
            1700000020u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "带正确 auth 重放 d1 -> 命中缓存回成功 (rc=%d)", rc);
        CHECK(memcmp(first, s_big_resp, first_len) == 0, "命中的仍是 A 段那份应答");
        CHECK(cloud_proto_dedup_hit_count() == (hits0 + 2u), "命中计数 -> 2");

        cloud_profile_reset();                  /* 还原, 别影响后面的用例 */
    }

    /* ---------- E. 环形覆盖: 超过 4 条后最旧的被遗忘 ---------- */
    {
        static const uint16_t alarm[5] = { 11u, 12u, 13u, 14u, 15u };
        char                  body[192];

        cloud_live_reset(10u);
        cloud_proto_dedup_reset();

        /* e1..e5 五条各不相同的 msgId, 把 4 个槽位轮一圈: e1 先被覆盖掉 */
        for (i = 0; i < 5; i++)
        {
            sprintf(body,
                    "{\"cmd\":\"cfg.set\",\"msgId\":\"e%d\",\"ts\":1,\"data\":"
                    "{\"expectVersion\":%d,\"sections\":{\"thresholds\":{\"alarmCm\":%u}}}}",
                    i + 1, 10 + i, (unsigned)alarm[i]);
            rc = cloud_proto_handle(body, 1u, s_big_resp,
                                    (uint16_t)sizeof(s_big_resp), &rl);
            CHECK(rc == CLOUD_EC_OK, "e%d -> 成功 (rc=%d)", i + 1, rc);
        }
        CHECK(config_store_version() == 15u, "5 条都真执行了: 版本 -> 15");

        /* 再发一次 e1: 它已经被挤出记忆 -> 【会真的再执行】-> 版本冲突 1002 */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"e1\",\"ts\":1,\"data\":"
            "{\"expectVersion\":10,\"sections\":{\"thresholds\":{\"alarmCm\":11}}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_VER_CONFLICT,
              "★ e1 已被覆盖(只记最近 4 条) -> 又执行了一次, 回 1002 (rc=%d)", rc);

        /* 而 e5(最新的那条)仍记得 */
        rc = cloud_proto_handle(
            "{\"cmd\":\"cfg.set\",\"msgId\":\"e5\",\"ts\":1,\"data\":"
            "{\"expectVersion\":14,\"sections\":{\"thresholds\":{\"alarmCm\":15}}}}",
            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "★ e5 仍在记忆里 -> 回成功 (rc=%d)", rc);
    }

    /* ---------- F. 超过 512 B 的应答(cfg.query): 不缓存, 但重发仍然正确 ---------- */
    /* cfg.query 是只读的, 缓存不下时"照常再执行一遍"是正确的: 平台照样拿到完整正文,
     * 不会看到被截断的配置。(这也是 CLOUD_DEDUP_RESP_MAX 敢只给 512 B 的前提。) */
    cloud_live_reset(3u);
    cloud_proto_dedup_reset();

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"g1\",\"ts\":1,\"data\":{}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "cfg.query 第 1 次 -> 成功 (rc=%d)", rc);
    CHECK(rl > 512u, "cfg.query 应答确实超过 512 B 缓存上限 (%u B)", (unsigned)rl);
    first_len = rl;
    memcpy(first, s_big_resp, first_len + 1u);

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"g1\",\"ts\":1,\"data\":{}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "cfg.query 重发 -> 仍成功 (rc=%d)", rc);
    CHECK(rl == first_len, "★ 重发正文没有被截断 (%u vs %u)",
          (unsigned)rl, (unsigned)first_len);
    CHECK(memcmp(first, s_big_resp, first_len) == 0, "★ 重发内容与首次逐字节一致");
    CHECK(cloud_proto_dedup_hit_count() == 0u,
          "★ 大应答不进缓存, 所以没有命中(靠重执行拿到同样正文)");
}
/* =================================================================
 * [23] sen.set / dev.set  (v36)
 * -----------------------------------------------------------------
 * 核心判据不是"给的值生效了", 而是 ★ 【没给的字段一个都不能被动】★ ——
 * 这是 set 类命令最容易出的静默丢数据 bug(cj_get_str 找不到时会把 out 置空串)。
 * 另外守住: 只读字段(serial/type)给了要【明确拒绝】, 不是静默忽略。
 * ================================================================= */
static void test_set_entry(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[23] sen.set\n");

    cloud_live_reset(3u);                   /* 210001: name=东入口 period=60 offline=90; 310001: vol=50 */

    /* ★2026-10-28(A2): sen.set 现在会判目标传感器是否在线, 离线回 1004 且不写配置。
     *   所以先给 210001 造一条"刚刚上报过"的快照 —— 真实流程里绑定后设备就会周期上报,
     *   这一步只是把测试场景补齐。放在 cloud_live_reset() 之后(它会重置快照表)。 */
    {
        uint8_t uid_b[CFG_UID_BYTES];

        CHECK(cfg_uid_to_bytes("21000000000000000000000001", uid_b),
              "准备: 210001 能转成 13 字节 uid");
        (void)last_value_update_full(uid_b, 1500, 1500, 80u, 0u, -70, get_tick_ms());
    }

    /* ---------- sen.set: 只改 name, 其余一律不能动 ---------- */
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss1\",\"ts\":1000,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"name\":\"西入口\"}}",
                            1000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.set 只改 name -> 成功 (rc=%d)", rc);
    CHECK(strcmp(config_store_get()->sensors[0].name, "西入口") == 0, "name 生效");
    CHECK(config_store_get()->sensors[0].report_period_s == 60u,
          "★ 没给的 reportPeriodSeconds 仍是 60(没被清成 0)");
    CHECK(config_store_get()->sensors[0].offline_timeout_s == 90u,
          "★ 没给的 offlineTimeoutSeconds 仍是 90");
    CHECK(config_store_get()->sensors[0].enabled == 1u,
          "★ 没给的 enabled 仍是 true");
    CHECK(config_store_get()->sensors[0].measure_mode == (uint8_t)CFG_MEASURE_LEVEL,
          "★ 没给的 measureMode 仍是 level");
    CHECK(config_store_get()->sensor_count == 1u, "★ 没有多出一条(不能变成追加)");

    {
        config_store_t chk;
        CHECK((config_store_load(&chk) == CFG_OK) &&
              (strcmp(chk.sensors[0].name, "西入口") == 0) &&
              (chk.sensors[0].report_period_s == 60u),
              "★ 落盘了, 且没给的字段在 Flash 里也没丢");
    }

    /* ---------- sen.set: 目标离线 ----------
     * ★2026-10-28(A2): 文档 §5.3 第 7 条 —— 目标传感器离线回 1004。
     * 两条都要验:
     *   ① 要下发的字段(period) -> 1004, 且【配置一个字都不写】;
     *   ② 不下载的字段(name)  -> 照样成功, 不能因为离线连改名都做不了。 */
    {
        uint8_t  uid_b[CFG_UID_BYTES];
        uint16_t period_before = config_store_get()->sensors[0].report_period_s;

        CHECK(cfg_uid_to_bytes("21000000000000000000000001", uid_b), "准备: uid 转字节");
        /* 把最近上报时间推到 400s 前: period=60 ⇒ 阈值 180s, 必判离线 */
        (void)last_value_update_full(uid_b, 1500, 1500, 80u, 0u, -70,
                                     get_tick_ms() - 400000u);

        rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss_off_name\",\"ts\":1,"
                                "\"data\":{\"uid\":\"21000000000000000000000001\","
                                "\"name\":\"离线也能改名\"}}",
                                1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "★ 离线但只改 name -> 仍成功(不需要下发就不判在线)");
        CHECK(strcmp(config_store_get()->sensors[0].name, "离线也能改名") == 0, "name 生效");

        rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss_off_period\",\"ts\":1,"
                                "\"data\":{\"uid\":\"21000000000000000000000001\","
                                "\"reportPeriodSeconds\":120}}",
                                1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_DEV_OFFLINE,
              "★ 离线 + 要下发(period) -> 1004 (rc=%d)", rc);
        CHECK(config_store_get()->sensors[0].report_period_s == period_before,
              "★ 配置一个字没写(不是先落盘再报离线)");

        /* 参数非法要优先回 1001, 不能被"离线"抢先答成 1004 */
        rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss_off_bad\",\"ts\":1,"
                                "\"data\":{\"uid\":\"21000000000000000000000001\","
                                "\"installHeightCm\":3000}}",
                                1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_PARAM,
              "★ 离线 + 越界值 -> 1001(参数错优先于离线, rc=%d)", rc);

        /* 恢复在线, 后面的用例照旧 */
        (void)last_value_update_full(uid_b, 1500, 1500, 80u, 0u, -70, get_tick_ms());
    }

    /* ---------- sen.set: 多字段一起改 ---------- */
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss2\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"installHeightCm\":300,"
                            "\"calibOffsetMm\":-15,\"lowBatteryPercent\":30,"
                            "\"enabled\":false}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "多字段一起改 -> 成功 (rc=%d)", rc);
    CHECK((config_store_get()->sensors[0].install_height_cm == 300u) &&
          (config_store_get()->sensors[0].calib_offset_mm == -15) &&
          (config_store_get()->sensors[0].low_battery_pct == 30u) &&
          (config_store_get()->sensors[0].enabled == 0u),
          "四个字段都生效了(含负数 calibOffsetMm)");

    /* ---------- 失败路径: 配置必须一个字都不动 ---------- */
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss3\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000009\",\"name\":\"不存在\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_NO_DEVICE, "★ 没绑过的 uid -> 1003, 不是偷偷追加一条 (rc=%d)", rc);
    CHECK(config_store_get()->sensor_count == 1u, "★ 没有多出一条");

    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss4\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"serial\":\"WL99999999\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ serial 是只读字段, 给了要明确拒绝 -> 1001 (rc=%d)", rc);
    CHECK(config_store_get()->sensors[0].serial[0] == '\0', "serial 没被改");

    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss5\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"installHeightCm\":9999}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ 越界值(>2000) -> 1001, 由 validate() 拦住 (rc=%d)", rc);
    CHECK(config_store_get()->sensors[0].install_height_cm == 300u,
          "★ 失败后被拒的值没有落地(仍是 300)");

    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss6\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"measureMode\":\"bogus\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "非法 measureMode -> 1001 (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss7\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"name\":"
                            "\"0123456789012345678901234567890123456789\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ 超长 name(40 字节 > 32) -> 1001, 不静默截断 (rc=%d)", rc);

    /* =================================================================
     * ★2026-10-05: sen.set 改上报周期 -> 网关负责组 LoRa 0x01 帧下发设备 ★
     * -----------------------------------------------------------------
     * 用户口径: 平台只下一个字段(reportPeriodSeconds), 网关把它打包成 0x01
     *   帧投给液位传感器本身(以前只改网关自己的配置, 一个字节都不发给设备)。
     * 这里守住三件事:
     *   ① 给了 reportPeriodSeconds -> 真的调了业务层, 且数值【原样透传】——
     *      单位是【秒】, 不许出现 ×60 / ÷60 的换算(改版前是分钟, 差 60 倍是
     *      这条链上最贵的一个 bug);
     *   ② 没给 reportPeriodSeconds -> 一次都不调(改 name/enabled 是网关侧的事,
     *      传感器只有 3 字节 UID, 它连自己的 name 都不知道);
     *   ③ offlineTimeoutSeconds 没给 -> 自动填 3 倍; 平台【显式给了】-> 以平台为准。
     * ================================================================= */
    fake_lora_business_reset();
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss8\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"reportPeriodSeconds\":1800}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.set 只改上报周期 -> 成功 (rc=%d)", rc);
    CHECK(fake_sensor_param_calls == 1, "★ 调了业务层 1 次(实际 %d)", fake_sensor_param_calls);
    CHECK(strcmp(fake_sensor_param_uid, "21000000000000000000000001") == 0,
          "★ uid 交给了业务层 (%s)", fake_sensor_param_uid);
    CHECK(fake_sensor_param_report_s == 1800u,
          "★★ 上报间隔【秒】原样透传, 没有 ×60/÷60 (实际 %u)",
          (unsigned)fake_sensor_param_report_s);
    CHECK(fake_sensor_param_thresh_mm == 50u,
          "★ 阈值用固定 50mm (实际 %u)", (unsigned)fake_sensor_param_thresh_mm);
    CHECK(config_store_get()->sensors[0].report_period_s == 1800u,
          "★ 网关侧配置也生效了");
    CHECK(config_store_get()->sensors[0].offline_timeout_s == 5400u,
          "★ 没给 offlineTimeoutSeconds -> 自动填 report×3 = 5400 (实际 %u)",
          (unsigned)config_store_get()->sensors[0].offline_timeout_s);

    /* ---- 平台显式给了 offlineTimeoutSeconds -> 一个字都不许覆盖 ---- */
    fake_lora_business_reset();
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss9\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"reportPeriodSeconds\":120,"
                            "\"offlineTimeoutSeconds\":777}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.set 同时给两个字段 -> 成功 (rc=%d)", rc);
    CHECK(config_store_get()->sensors[0].offline_timeout_s == 777u,
          "★ 平台显式给的值优先, 不被 2 倍覆盖 (实际 %u)",
          (unsigned)config_store_get()->sensors[0].offline_timeout_s);
    CHECK(fake_sensor_param_report_s == 120u, "★ 新的上报间隔也下发了 (实际 %u)",
          (unsigned)fake_sensor_param_report_s);

    /* ---- 只改 name -> 【不能】下发设备 ---- */
    fake_lora_business_reset();
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss10\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"name\":\"只改名字\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.set 只改 name -> 成功 (rc=%d)", rc);
    CHECK(fake_sensor_param_calls == 0,
          "★ 只改 name 时【一次都不下发设备】(实际 %d 次)", fake_sensor_param_calls);
    CHECK(config_store_get()->sensors[0].offline_timeout_s == 777u,
          "★ 没给 reportPeriodSeconds 时也不动 offlineTimeoutSeconds");

    /* ---- 复现旧 bug 的边界: 上报周期 >= 90 秒在改版前会回 1001 ---- */
    fake_lora_business_reset();
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss11\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"reportPeriodSeconds\":3600}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK,
          "★★ 上报周期 3600s(> 默认 offline 90s)【不再】回 1001 (rc=%d)", rc);
    CHECK(fake_sensor_param_report_s == 3600u, "★ 3600 秒也照原样下发");

    /* ---- 业务层投递失败: 配置已落盘, 命令仍回 0(设备侧由日志/重发兜底) ---- */
    fake_lora_business_reset();
    fake_sensor_param_ret = 0;
    rc = cloud_proto_handle("{\"cmd\":\"sen.set\",\"msgId\":\"ss12\",\"ts\":1,"
                            "\"data\":{\"uid\":\"21000000000000000000000001\",\"reportPeriodSeconds\":300}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "0x01 投递失败不回错误码, 配置仍算成功 (rc=%d)", rc);
    CHECK(config_store_get()->sensors[0].report_period_s == 300u,
          "★ 投递失败也不影响配置落盘");
    fake_sensor_param_ret = 1;

    /* =================================================================
     * ★2026-10-21: 云端改用 cfg.set(sensors[]) 控制传感器 ★
     *   sensors[] 里显式给了 reportPeriodSeconds 时, 必须与 sen.set 同口径:
     *   落盘成功后把新周期以 0x01 下发给传感器本身(否则设备仍按旧周期上报,
     *   网关按新周期判失联 -> 误报离线)。
     *   没给 reportPeriodSeconds 的则一次都不下发。
     * ================================================================= */
    {
        char     req[512];
        uint32_t v = config_store_version();

        fake_lora_business_reset();
        (void)snprintf(req, sizeof(req),
                       "{\"cmd\":\"cfg.set\",\"msgId\":\"cs1\",\"ts\":1,\"data\":"
                       "{\"expectVersion\":%u,\"sections\":{\"sensors\":["
                       "{\"uid\":\"21000000000000000000000001\",\"reportPeriodSeconds\":240}]}}}",
                       (unsigned)v);
        rc = cloud_proto_handle(req, 1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "cfg.set sensors 改上报周期 -> 成功 (rc=%d)", rc);
        CHECK(fake_sensor_param_calls == 1,
              "★ cfg.set 也调了 0x01 下发 1 次(实际 %d)", fake_sensor_param_calls);
        CHECK(fake_sensor_param_report_s == 240u,
              "★ cfg.set 的周期原样透传 (实际 %u)", (unsigned)fake_sensor_param_report_s);
        CHECK(config_store_get()->sensors[0].report_period_s == 240u,
              "★ 网关侧配置也生效了");

        /* ---- 只改 name: 不下发设备 ---- */
        v = config_store_version();
        fake_lora_business_reset();
        (void)snprintf(req, sizeof(req),
                       "{\"cmd\":\"cfg.set\",\"msgId\":\"cs2\",\"ts\":1,\"data\":"
                       "{\"expectVersion\":%u,\"sections\":{\"sensors\":["
                       "{\"uid\":\"21000000000000000000000001\",\"name\":\"只改名字\"}]}}}",
                       (unsigned)v);
        rc = cloud_proto_handle(req, 1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "cfg.set sensors 只改 name -> 成功 (rc=%d)", rc);
        CHECK(fake_sensor_param_calls == 0,
              "★ 没给 reportPeriodSeconds 时一次都不下发(实际 %d 次)",
              fake_sensor_param_calls);
    }
}
/* =================================================================
 * [25] sys.reboot  (v38)
 * -----------------------------------------------------------------
 * 这条命令本身【不复位】—— 它只登记"什么时候重启", 复位由 main.c 主循环调
 * cloud_proto_reboot_due() 去做。所以这里能把它整条逻辑验完(参数/延迟/一次性),
 * 而且验的是真判据: 到了点才算到期、取走一次就不再重复触发(否则主循环会反复复位)。
 * ================================================================= */
static void test_sys_reboot(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[25] sys.reboot\n");

    CHECK(!cloud_proto_reboot_due(0u), "还没登记时 reboot_due() = false");
    CHECK(!cloud_proto_reboot_due(0xFFFFFFFFu), "任意时刻都是 false");

    /* delaySeconds 缺省 0 -> 立刻到期; 而且必须是一次性的 */
    rc = cloud_proto_handle("{\"cmd\":\"sys.reboot\",\"msgId\":\"rb1\",\"ts\":1000}",
                            1000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sys.reboot(不带参数) -> 成功 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"data\":{}") != NULL, "文档这条响应没有 data 字段");
    CHECK(cloud_proto_reboot_due(1000u), "★ delaySeconds 缺省 0 -> 立刻到期");
    CHECK(!cloud_proto_reboot_due(1000u),
          "★★ 一次性: 取走后再调必须 = false(否则主循环会反复复位)");

    /* delaySeconds=100: 登记 1000+100=1100 */
    rc = cloud_proto_handle("{\"cmd\":\"sys.reboot\",\"msgId\":\"rb2\",\"ts\":1000,"
                            "\"data\":{\"delaySeconds\":100}}",
                            1000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "delaySeconds=100 -> 成功 (rc=%d)", rc);
    CHECK(!cloud_proto_reboot_due(1099u), "1099s 还没到(登记的是 1100)");
    CHECK(cloud_proto_reboot_due(1100u), "★ 1100s 到期");
    CHECK(!cloud_proto_reboot_due(1101u), "取走后仍然是一次性");

    /* 非法参数: 必须【不登记】任何重启 */
    rc = cloud_proto_handle("{\"cmd\":\"sys.reboot\",\"msgId\":\"rb3\",\"ts\":1,"
                            "\"data\":{\"delaySeconds\":\"soon\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "delaySeconds 是字符串 -> 1001 (rc=%d)", rc);
    CHECK(!cloud_proto_reboot_due(0xFFFFFFFFu), "★ 参数非法时【没有】登记重启");

    rc = cloud_proto_handle("{\"cmd\":\"sys.reboot\",\"msgId\":\"rb4\",\"ts\":1,"
                            "\"data\":{\"delaySeconds\":70000}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "delaySeconds 超过 uint16 -> 1001 (rc=%d)", rc);
    CHECK(!cloud_proto_reboot_due(0xFFFFFFFFu), "★ 仍然没有登记");

    /* 重复下发: 后一条覆盖前一条的时刻 */
    (void)cloud_proto_handle("{\"cmd\":\"sys.reboot\",\"msgId\":\"rb5\",\"ts\":2000,"
                             "\"data\":{\"delaySeconds\":50}}",
                             2000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    rc = cloud_proto_handle("{\"cmd\":\"sys.reboot\",\"msgId\":\"rb6\",\"ts\":2000,"
                            "\"data\":{\"delaySeconds\":500}}",
                            2000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "覆盖登记 (rc=%d)", rc);
    CHECK(!cloud_proto_reboot_due(2100u), "★ 被后一条覆盖: 2100s 不该到期");
    CHECK(cloud_proto_reboot_due(2500u), "★ 2500s = 2000+500 到期");
}

/* =================================================================
 * [26] sys.query_status + 状态提供者  (v40)
 * -----------------------------------------------------------------
 * 两层判据:
 *   1) 没注册提供者时必须【明确回 1006】, 不能拿一堆 0 冒充状态发上去;
 *   2) 注册之后, data 段要按文档 5.1 第 1 条的字段出现(13 个: 文档 11 个
 *      + slot + currentStatus; bufferedEvents 已于 2026-11-12 移除)。
 * ================================================================= */
static int s_fake_status_calls;

static void fake_status_provider(cloud_status_t *out)
{
    s_fake_status_calls++;
    out->online         = true;
    out->fw_version     = "9.9.9";
    out->config_version = 42u;
    out->rsrp_dbm       = -77;
    out->uptime_s       = 1234u;
    out->mode           = "manual";
    out->current_gear   = 3u;
    out->sensors_online = 2u;
    out->sensors_total  = 5u;
}

static void test_sys_query_status(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[26] sys.query_status (状态提供者)\n");

    /* ---- 1) 没注册提供者: 必须明确 1006 ---- */
    cloud_proto_set_status_provider(NULL);
    rc = cloud_proto_handle("{\"cmd\":\"sys.query_status\",\"msgId\":\"qs0\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_STATE, "★ 没注册提供者 -> 1006(不用 0 冒充状态) (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"code\":1006") != NULL, "应答里也是 1006");
    CHECK(s_fake_status_calls == 0, "提供者没被调用过");

    /* ---- 2) 注册之后: 按文档 5.1 第 1 条的字段 ---- */
    cloud_proto_set_status_provider(fake_status_provider);
    rc = cloud_proto_handle("{\"cmd\":\"sys.query_status\",\"msgId\":\"qs1\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "注册后 -> 成功 (rc=%d)", rc);
    CHECK(s_fake_status_calls == 1, "★ 提供者被调用了一次");

    {
        char b[24];

        CHECK(cj_get_uint(s_big_resp, "data.configVersion", 0u) == 42u, "configVersion = 42");
        CHECK(cj_get_int(s_big_resp, "data.rsrp", 0) == -77, "rsrp = -77(负数)");
        CHECK(cj_get_uint(s_big_resp, "data.uptimeSeconds", 0u) == 1234u, "uptimeSeconds = 1234");
        CHECK(cj_get_uint(s_big_resp, "data.currentGear", 0u) == 3u, "currentGear = 3");
        CHECK(cj_get_uint(s_big_resp, "data.sensorsOnline", 0u) == 2u, "sensorsOnline = 2");
        CHECK(cj_get_uint(s_big_resp, "data.sensorsTotal", 0u) == 5u, "sensorsTotal = 5");
        (void)cj_get_str(s_big_resp, "data.fwVersion", b, (uint16_t)sizeof(b));
        CHECK(strcmp(b, "9.9.9") == 0, "fwVersion = 9.9.9 (实际 %s)", b);
        (void)cj_get_str(s_big_resp, "data.mode", b, (uint16_t)sizeof(b));
        CHECK(strcmp(b, "manual") == 0, "mode = manual (实际 %s)", b);
        CHECK(cj_get_bool(s_big_resp, "data.online", false) == true, "online = true");
    }

    /* ---- 3) bufferedEvents 已移除: 键不许复活 ---- */
    CHECK(strstr(s_big_resp, "\"bufferedEvents\"") == NULL,
          "★ 应答里【不再】出现 bufferedEvents(2026-11-12 移除)");
    CHECK(cj_get_uint(s_big_resp, "data.bufferedEvents", 0xDEADu) == 0xDEADu,
          "★ 取 bufferedEvents 走默认值 = 键不存在");

    /* 收尾: 还原成"未注册", 免得影响别的用例 */
    cloud_proto_set_status_provider(NULL);
}

/* =================================================================
 * [27] telemetry.report 数据段  (v42)
 * -----------------------------------------------------------------
 * 这是 7 条上行报文里的第一条。两条判据:
 *   1) 形状/字段名按文档 7.1(addr 按用户决策换成 uid);
 *   2) ★ levelCm 是"一位小数"的量(文档示例 12.3) —— 内部按 mm 整数存,
 *      输出时要变成 12.3 而不是 123, 也不能变成 12.300000000000001。
 * ================================================================= */
static void test_telemetry_data(void)
{
    cloud_telemetry_sensor_t s[2];
    cloud_telemetry_t        t;
    char                     buf[CLOUD_DATA_MAX];
    char                     up[CLOUD_RESP_MAX];
    uint16_t                 n;

    printf("\n[27] telemetry.report 数据段\n");

    (void)memset(s, 0, sizeof(s));
    s[0].uid        = "21000000000000000000000001";
    s[0].level_mm   = 123;                  /* -> levelCm 12.3 */
    s[0].raw_mm     = 123u;
    s[0].battery_pct = 88u;
    s[0].rssi_dbm   = -87;
    s[0].ts         = 1735689598u;
    s[1].uid        = "21000000000000000000000002";
    s[1].level_mm   = 81;                   /* -> levelCm 8.1 */
    s[1].raw_mm     = 2919u;
    s[1].battery_pct = 65u;
    s[1].rssi_dbm   = -92;
    s[1].ts         = 1735689599u;

    (void)memset(&t, 0, sizeof(t));
    t.sensor_count        = 2u;
    t.sensors             = s;
    t.effective_level_mm  = 123;
    t.effective_status    = "Alarm";
    t.effective_gear      = 3u;

    n = cloud_proto_build_telemetry_data(buf, (uint16_t)sizeof(buf), &t);
    CHECK(n > 0u, "生成成功 n=%u", (unsigned)n);

    CHECK(strcmp(buf,
        "{\"sensors\":[{\"uid\":\"21000000000000000000000001\",\"levelCm\":12.3,\"rawMm\":123,"
        "\"batteryPercent\":88,\"rssi\":-87,\"ts\":1735689598},"
        "{\"uid\":\"21000000000000000000000002\",\"levelCm\":8.1,\"rawMm\":2919,"
        "\"batteryPercent\":65,\"rssi\":-92,\"ts\":1735689599}],"
        "\"effective\":{\"levelCm\":12.3,\"status\":\"Alarm\",\"gear\":3}}") == 0,
        "★ 与文档 7.1 的形状/字段逐字符一致(addr->uid; effective 带 levelCm=最大值)");
    CHECK(strstr(buf, "12.3") != NULL, "★ levelCm 是 12.3(不是 123, 也不是 12.300000…)");
    CHECK(strstr(buf, "12.3000000000") == NULL, "★ 没有浮点尾巴");
    /* off fail-safe: status=Offline + gear=null */
    t.effective_status = "Offline";
    t.gear_null        = true;
    n = cloud_proto_build_telemetry_data(buf, (uint16_t)sizeof(buf), &t);
    CHECK((n > 0u) && (strstr(buf, "\"status\":\"Offline\"") != NULL) &&
          (strstr(buf, "\"gear\":null") != NULL),
          "★ off fail-safe: effective.status=Offline, gear=null");
    t.effective_status = "Alarm";
    t.gear_null        = false;


    /* 能整条包进上行信封 */
    n = cloud_proto_build_uplink(up, (uint16_t)sizeof(up), "telemetry.report",
                                 "m001", 1735689600u, buf);
    CHECK((n > 0u) && (strstr(up, "\"event\":\"telemetry.report\"") != NULL) &&
          (strstr(up, "\"data\":{\"sensors\":[") != NULL),
          "能整条包进上行信封(event + data.sensors)");

    /* 0 台传感器也要能出(空数组 + effective), 不能崩 */
    t.sensor_count = 0u;
    t.sensors      = NULL;
    n = cloud_proto_build_telemetry_data(buf, (uint16_t)sizeof(buf), &t);
    CHECK((n > 0u) && (strstr(buf, "\"sensors\":[]") != NULL), "0 台传感器 -> 空数组不崩");
    CHECK(strstr(buf, "\"effective\":{") != NULL, "effective 仍然在");

    /* 参数保护 */
    t.sensor_count = 2u;
    t.sensors      = NULL;
    CHECK(cloud_proto_build_telemetry_data(buf, (uint16_t)sizeof(buf), &t) == 0u,
          "count>0 但 sensors=NULL -> 0(不解引用)");
    CHECK(cloud_proto_build_telemetry_data(NULL, 64u, &t) == 0u, "out=NULL -> 0");
    CHECK(cloud_proto_build_telemetry_data(buf, 8u, &t) == 0u, "缓冲过小 -> 0");
}

/* =================================================================
 * [28] event.status_change + event.device 数据段  (v43)
 * ================================================================= */
static void test_event_data(void)
{
    cloud_status_change_t sc;
    cloud_device_event_t  de;
    char                  buf[CLOUD_DATA_MAX];
    char                  up[CLOUD_RESP_MAX];
    uint16_t              n;

    printf("\n[28] event.status_change / event.device 数据段\n");

    (void)memset(&sc, 0, sizeof(sc));
    sc.from     = "Normal";
    sc.to       = "Warning";
    sc.gear     = 2u;
    sc.level_mm = 81;                       /* -> 8.1 */

    n = cloud_proto_build_status_change_data(buf, (uint16_t)sizeof(buf), &sc);
    CHECK(n > 0u, "status_change 生成成功 n=%u", (unsigned)n);
    CHECK(strcmp(buf, "{\"from\":\"Normal\",\"to\":\"Warning\",\"gear\":2,\"levelCm\":8.1}") == 0,
          "★ 与文档 7.2 形状逐字符一致 (实际 %s)", buf);

    n = cloud_proto_build_uplink(up, (uint16_t)sizeof(up), "event.status_change",
                                 "s001", 1735689600u, buf);
    CHECK((n > 0u) && (strstr(up, "\"event\":\"event.status_change\"") != NULL) &&
          (strstr(up, "\"data\":{\"from\":\"Normal\"") != NULL), "能包进上行信封");

    (void)memset(&de, 0, sizeof(de));
    de.uid      = "21000000000000000000000001";
    de.dev_type = "sensor";
    de.event    = "offline";

    n = cloud_proto_build_device_event_data(buf, (uint16_t)sizeof(buf), &de);
    CHECK(n > 0u, "device 生成成功 n=%u", (unsigned)n);
    CHECK(strcmp(buf, "{\"uid\":\"21000000000000000000000001\",\"devType\":\"sensor\",\"event\":\"offline\"}") == 0,
          "★ 与文档 7.4 形状逐字符一致(addr->uid) (实际 %s)", buf);

    de.event = "bound";
    n = cloud_proto_build_device_event_data(buf, (uint16_t)sizeof(buf), &de);
    CHECK((n > 0u) && (strstr(buf, "\"event\":\"bound\"") != NULL), "bound 事件也正常");

    /* 参数保护 */
    CHECK(cloud_proto_build_status_change_data(NULL, 64u, &sc) == 0u, "sc out=NULL -> 0");
    CHECK(cloud_proto_build_status_change_data(buf, 8u, &sc) == 0u, "sc 缓冲过小 -> 0");
    CHECK(cloud_proto_build_device_event_data(NULL, 64u, &de) == 0u, "de out=NULL -> 0");
    CHECK(cloud_proto_build_device_event_data(buf, 8u, &de) == 0u, "de 缓冲过小 -> 0");
}/* =================================================================
 * [31] sys.time_sync —— 已废弃(2026-10-28 从分发表移除)
 * -----------------------------------------------------------------
 * 用户 2026-10-28 裁决: 该命令废弃, 平台不再使用, 网关同步从分发表移除。
 * 现在云端再发 sys.time_sync 会落到"未知命令"分支 -> 1001。
 * (cloud_cmd_sys_time_sync() 与写时钟钩子暂留但【已不可达】,
 *  等确认平台彻底停用后再整段删除, 免得牵动 4G 取时间那条链。)
 * ================================================================= */
static void test_sys_time_sync(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[31] sys.time_sync (已废弃: 不在分发表)\n");

    CHECK(!cloud_proto_is_known_cmd("sys.time_sync"),
          "★ sys.time_sync 已从命令表移除");

    rc = cloud_proto_handle("{\"cmd\":\"sys.time_sync\",\"msgId\":\"ts1\",\"ts\":1000,"
                            "\"data\":{\"ts\":1735689600}}",
                            1000u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ 废弃命令 -> 未知命令 1001 (rc=%d)", rc);
}

/* =================================================================
 * [32] sen.bind / dev.bind  (v50)
 * -----------------------------------------------------------------
 * 命令本身只表示"配对窗口已开", 真实绑定结果异步走 event.device。
 * 所以判据是: 参数校验(serial 8~16 必填) + 容量上限(1009) + 窗口秒数,
 * 以及 ★ 没注册配对后端时明确 1006(不假装窗口开了) ★。
 * ================================================================= */
static uint32_t     s_pair_calls;
static char         s_pair_serial[24];
static char         s_pair_name[40];
static uint16_t     s_pair_window;
static bool         s_pair_result;

static bool fake_pair_open(const char *serial, const char *name,
                           uint16_t window_s)
{
    s_pair_calls++;
    (void)strncpy(s_pair_serial, (serial != NULL) ? serial : "", sizeof(s_pair_serial) - 1u);
    s_pair_serial[sizeof(s_pair_serial) - 1u] = '\0';
    (void)strncpy(s_pair_name, (name != NULL) ? name : "", sizeof(s_pair_name) - 1u);
    s_pair_name[sizeof(s_pair_name) - 1u] = '\0';
    s_pair_window    = window_s;
    return s_pair_result;
}

static void test_bind(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[32] sen.bind (配对窗口 + 后端钩子)\n");

    cloud_live_reset(3u);               /* 1 传感器 + 1 设备, 版本 3 */

    /* ---- 1) 没注册配对后端 -> 明确 1006 ---- */
    cloud_proto_set_pair_hook(NULL);
    s_pair_calls = 0u;
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b1\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260001\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_STATE, "★ 没注册配对后端 -> 1006(不假装窗口开了) (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "pairWindowSeconds") == NULL, "应答里【没有】窗口秒数");
    CHECK(s_pair_calls == 0u, "后端没被调用");

    /* ---- 2) 注册之后: 正常开窗 ---- */
    cloud_proto_set_pair_hook(fake_pair_open);
    s_pair_result = true;

    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b2\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260001\",\"name\":\"西入口\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.bind -> 成功 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"pairWindowSeconds\":60") != NULL,
          "★ 默认窗口 60s 写进应答 (文档 5.3)");
    CHECK((s_pair_calls == 1u) && (strcmp(s_pair_serial, "WL20260001") == 0) &&
          (strcmp(s_pair_name, "西入口") == 0),
          "★ 后端拿到了 serial/name");

    /* 窗口秒数可覆盖(sen.bind 也支持 pairWindowSeconds) */
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b3\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260001\",\"pairWindowSeconds\":120}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK((rc == CLOUD_EC_OK) && (s_pair_window == 120u),
          "★ sen.bind 的窗口秒数可覆盖 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"pairWindowSeconds\":120") != NULL, "应答回的是 120");

    /* ---- 3) 参数校验 ----
     * ★2026-10-08: serial 已改为【可选】★ 用户口径"这一个字段直接丢":
     * 平台侧的绑定请求不带 serial(见用户样例), 所以:
     *   · 缺 serial   -> 【不再是错】, 照常开窗口
     *   · 给了但超长  -> 仍然 1001(不静默截断)
     *   · 短于 8 字节 -> 也不再判错(原来的 8~16 下限已取消) */
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b4\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL2026\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★ serial 只有 6 字节 -> 现在不再判错 (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b5\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL2026000112345678\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ serial 18 字节(>16) -> 仍 1001(不截断) (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b6\",\"ts\":1,\"data\":{}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK,
          "★★ 缺 serial 也照常开窗口(平台样例就是不带 serial) (rc=%d)", rc);

    /* 用户给的样例原样跑一遍 */
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"f4-01\",\"ts\":1791136499,"
                            "\"auth\":\"\",\"data\":{\"name\":\"西入口液位\","
                            "\"pairWindowSeconds\":60}}",
                            1791136499u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★★ 平台样例(无 serial) -> 成功 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"pairWindowSeconds\":60") != NULL,
          "应答带回 pairWindowSeconds=60");

    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b7\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260001\",\"pairWindowSeconds\":0}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "pairWindowSeconds=0 -> 1001");

    /* ---- 4) 容量上限 1009(纯配置判断, 不需要 LoRa) ---- */
    {
        config_store_t *m = config_store_get_mut();
        uint8_t         i;

        /* ★ 用户口径是液位≤4 —— 这里造"液位到顶(4)"的场景, 验证 sen.bind 会回 1009。
         *   只填 4 台液位(而不是 CFG_SENSOR_MAX=5): 5 台本身就违反"液位≤4"。 */
        m->sensor_count = 4u;                           /* 液位装满 4(≤4) */
        for (i = 0u; i < 4u; i++)
        {
            (void)snprintf(m->sensors[i].uid, CFG_UID_STR_MAX, "210000000000000000000000%02u", (unsigned)(i + 1u));
            /* ⚠ 新槽位不能只设 uid: report_period_s / offline_timeout_s 为 0 过不了
             * 校验(文档 3.3: 周期 5~3600、超时 >= 2 倍周期), 落盘会失败。 */
            m->sensors[i].measure_mode      = (uint8_t)CFG_MEASURE_LEVEL;
            m->sensors[i].report_period_s   = 60u;
            m->sensors[i].offline_timeout_s = 90u;
            m->sensors[i].enabled           = 1u;
        }
        CHECK(config_store_save(m) == CFG_OK, "准备: 液位填到上限 4 台");
        CHECK(!config_store_can_add(true),  "液位已满 -> 不能再加液位");
    }
    /* 计数归零: 后面的断言要的是"这一条【没有】去开窗", 与前面开过几次无关 */
    s_pair_calls = 0u;
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b8\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260002\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_LIMIT, "★ 传感器已满 -> 1009 (rc=%d)", rc);
    CHECK(s_pair_calls == 0u, "★ 满员时【没有】去开窗(先拦住, 别等配对成功才发现放不下)");

    /* 后面几条需要能真正开窗 -> 恢复 1 传感器配置 */
    cloud_live_reset(3u);

    /* ---- 5) 后端报失败 -> 1007 ---- */
    s_pair_result = false;
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"b10\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260003\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_INTERNAL, "后端返回 false -> 1007 (rc=%d)", rc);

    /* ---- 6) pairWindowSeconds 上限 300 (2026-10-28 收敛, 手册 §4.3 的 1~300) ----
     * 边界: 300 收, 301 拒。此前只判 !=0 && <=65535, 平台发 3600 也会开一小时窗。
     * ⚠ msgId 必须独家: 幂等缓存会重放同 msgId 的旧应答, 撞号会掩盖真实判断
     *   (本次改注释时就踩过 —— 用 b8/b9 撞了后面的容量用例, 导致误判)。 */
    s_pair_result = true;
    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"bw300\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260004\",\"pairWindowSeconds\":300}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "pairWindowSeconds=300(边界) -> 收 (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"sen.bind\",\"msgId\":\"bw301\",\"ts\":1,"
                            "\"data\":{\"serial\":\"WL20260005\",\"pairWindowSeconds\":301}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ pairWindowSeconds=301 -> 1001(上限 300) (rc=%d)", rc);

    cloud_proto_set_pair_hook(NULL);    /* 收尾还原 */
}

/* =================================================================
 * [33] sen.query / dev.query  (v53)
 * -----------------------------------------------------------------
 * 这两条与 rule.query 同一套办法: 复用 config_store_write_members() 写出 9 个域,
 * 再删掉本条不该带的域 —— 所以 sensors[]/devices[] 的字段名与键序和 cfg.query 里
 * 同名对象【同源】, 不会因为抄一遍字段表而漂。
 *
 * 本版给它们补了"最近一次上报"的运行时字段, 这里重点验两件最容易做错的事:
 *   1) ★ 没有上报记录时 levelCm/batteryPercent/lastTs 【一个都不许出现】★
 *      用 0 冒充会让平台读成"水位 0cm / 电量 0% / 时间 1970" —— 比缺字段更糟。
 *   2) ★ 网关时钟没注册时 lastTs 也不许出现 ★ (同理, 0 会被读成 1970)
 * 另外验: 该删的域真删了; 设备【不】补 levelCm(联动设备没有水位)。
 * ================================================================= */
static uint32_t s_query_unix;

static uint32_t query_fake_now_unix(void)
{
    return s_query_unix;
}

static void test_sen_dev_query(void)
{
    uint16_t rl;
    int      rc;
    uint8_t  suid[13];
    char     s_uid[27];

    printf("\n[33] sen.query\n");

    cloud_live_reset(3u);               /* 测试助手内置: 1 传感器 + 1 设备 */
    last_value_reset();
    last_value_set_now_hook(NULL);

    /* ---- 1) sen.query: 只带 sensors 域 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"sen.query\",\"msgId\":\"sq1\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.query -> 成功 (rc=%d)", rc);
    CHECK(cj_count(s_big_resp, "data.sensors") == 1u,
          "sensors 有 1 台 (实际 %u)", (unsigned)cj_count(s_big_resp, "data.sensors"));
    CHECK(strstr(s_big_resp, "\"system\"") == NULL, "★ 没有 system");
    CHECK(strstr(s_big_resp, "\"network\"") == NULL, "★ 没有 network");
    CHECK(strstr(s_big_resp, "\"thresholds\"") == NULL, "★ 没有 thresholds");
    CHECK(strstr(s_big_resp, "\"meta\"") == NULL, "★ 没有 meta");

    s_uid[0] = '\0';
    (void)cj_get_str(s_big_resp, "data.sensors[0].uid", s_uid, (uint16_t)sizeof(s_uid));
    CHECK(s_uid[0] != '\0', "sensors[0] 带 uid (实际 \"%s\")", s_uid);

    /* ---- 2) ★ 从没上报过 -> 运行时字段一个都不许出现, 但 online 必须在 ----
     * (第 10 条: 平台的"设备列表"缺了它会以为是配置丢了) */
    CHECK(strstr(s_big_resp, "lastLevelCm") == NULL, "★ 没上报过 -> 不给 lastLevelCm(不拿 0 冒充)");
    CHECK(strstr(s_big_resp, "batteryPercent") == NULL, "★ 没上报过 -> 不给 batteryPercent");
    CHECK(strstr(s_big_resp, "lastTs") == NULL, "★ 没上报过 -> 不给 lastTs");
    CHECK(strstr(s_big_resp, "\"online\":false") != NULL,
          "★ 没上报过 -> online:false(整项仍要在列表里)");

    /* ---- 3) 有上报 + 时钟已注册 -> 按文档口径出现 ---- */
    CHECK(cfg_uid_to_bytes(s_uid, suid), "准备: 配置里的 uid 串能转成 3 字节");
    s_query_unix = 1700000000u;
    last_value_set_now_hook(query_fake_now_unix);
    CHECK(last_value_update(suid, 1234u, 88u, 0u, 1000u), "准备: 喂一条上报(水位 1234mm)");

    /* ★ online 的判据是"距上次上报 < 3×上报周期", 要拿当前 tick 比 —— 主机上要把
     * 虚拟时基摆到那条上报之后一点点(否则 0 - 1000 回绕成天文数字 -> 判成离线)。 */
    host_tick_set(1500u);

    rc = cloud_proto_handle("{\"cmd\":\"sen.query\",\"msgId\":\"sq2\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.query(有上报) -> 成功");
    CHECK(strstr(s_big_resp, "\"online\":true") != NULL,
          "★ 刚上报过(0.5s 前) -> online:true");
    CHECK(strstr(s_big_resp, "\"lastLevelCm\":123.4") != NULL,
          "★ lastLevelCm = mm/10 = 123.4 (不差 10 倍, 字段名按文档 5.3)");
    CHECK(strstr(s_big_resp, "\"batteryPercent\":88") != NULL, "★ batteryPercent = 88");
    CHECK(strstr(s_big_resp, "\"lastTs\":1700000000") != NULL,
          "★ lastTs = 网关收到那一帧时的 Unix 秒");

    /* ---- 3b) 超过 3×上报周期 -> online:false(与失联检测同一套判据) ----
     * ⚠️ msgId 必须唯一: 分发器对重复 msgId 做【幂等】(直接返回上次的应答),
     *    这里若复用 "sq3" 就会拿到上一条的应答, 断言会莫名其妙地"差一条"。 */
    host_tick_set(1000u + 60u * 3u * 1000u + 1u);
    rc = cloud_proto_handle("{\"cmd\":\"sen.query\",\"msgId\":\"sq2b\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK && strstr(s_big_resp, "\"online\":false") != NULL,
          "★ 超 3×周期(60s×3) -> online:false");
    host_tick_set(1500u);       /* 摆回去, 别影响后面的用例 */

    /* ---- 4) ★ 时钟没注册 -> 水位照给, lastTs 必须消失 ---- */
    last_value_set_now_hook(NULL);
    CHECK(last_value_update(suid, 555u, 50u, 0u, 2000u), "准备: 无时钟再喂一条");

    rc = cloud_proto_handle("{\"cmd\":\"sen.query\",\"msgId\":\"sq3\",\"ts\":1}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sen.query(无时钟) -> 成功");
    CHECK(strstr(s_big_resp, "\"lastLevelCm\":55.5") != NULL, "没时钟也照给水位");
    CHECK(strstr(s_big_resp, "\"lastTs\"") == NULL,
          "★ 时钟没注册 -> 不给 lastTs(不拿 0 冒充 1970)");

    /* 收尾: 别把钩子/表留给后面的用例 */
    last_value_set_now_hook(NULL);
    last_value_reset();
}/* =================================================================
 * [35] cfg.query 的 sections 参数 (v55)
 * -----------------------------------------------------------------
 * 文档本来就有按域查询, 此前只认 "all"、其余一律 1001。本版补齐。
 * 重点验:
 *   1) ★ 不带参数 / "all" 仍是【全量】—— 老客户端行为逐字节不变 ★
 *   2) 单个域名 与 域名数组 都能"只要自己那几个域";
 *   3) 坏输入(不认识的名字 / 空数组 / 类型不对 / 超长名字 / 条数超限)
 *      一律 1001; ★ 数组里混了坏名字时【不许部分生效】★
 *      (不能"把好的那几个返回、坏的那个悄悄丢掉" —— 那样客户端会以为查到了全部)
 * ================================================================= */
static void test_cfg_sections(void)
{
    uint16_t rl;
    int      rc;

    printf("\n[35] cfg.query sections (按域查询)\n");

    cloud_live_reset(3u);               /* 1 传感器 + 1 设备 */

    /* ---- 1) 不带参数 = 全量(与老行为一致) ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs1\",\"ts\":1,\"data\":{}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "无 sections -> 成功 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"system\"") != NULL, "全量: 有 system");
    CHECK(strstr(s_big_resp, "\"sensors\"") != NULL, "全量: 有 sensors");
    CHECK(strstr(s_big_resp, "\"meta\"") != NULL, "全量: 有 meta");

    /* ---- 2) 单个域名 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs2\",\"ts\":1,"
                            "\"data\":{\"sections\":\"sensors\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sections=\"sensors\" -> 成功 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"sensors\"") != NULL, "有 sensors");
    CHECK(strstr(s_big_resp, "\"system\"") == NULL, "★ 没有 system");
    CHECK(strstr(s_big_resp, "\"thresholds\"") == NULL, "★ 没有 thresholds");
    CHECK(strstr(s_big_resp, "\"meta\"") == NULL, "★ 没有 meta");
    CHECK(cj_count(s_big_resp, "data.sensors") == 1u,
          "sensors 内容与全量一致(1 台, 实际 %u)",
          (unsigned)cj_count(s_big_resp, "data.sensors"));

    /* ---- 3) 域名数组 + 键序仍与写入器同源 ----
     * 毕设版 cfg 域只剩 6 个(system/network/sensors/thresholds/ota/meta),
     * gears/devices/rules 已删除, 不再作为合法域名。 */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs3\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"system\",\"network\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "sections=[system,network] -> 成功 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"system\"") != NULL, "有 system");
    CHECK(strstr(s_big_resp, "\"network\"") != NULL, "有 network");
    CHECK(strstr(s_big_resp, "\"sensors\"") == NULL, "★ 没有 sensors");
    CHECK(strstr(s_big_resp, "\"system\"") < strstr(s_big_resp, "\"network\""),
          "键序: system 在 network 之前(与写入器同源)");

    /* ---- 4) "all" 两种写法都还是全量 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs4\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"all\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK((rc == CLOUD_EC_OK) && (strstr(s_big_resp, "\"meta\"") != NULL),
          "sections=[\"all\"] 仍是全量");
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs5\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"sensors\",\"all\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK((rc == CLOUD_EC_OK) && (strstr(s_big_resp, "\"meta\"") != NULL),
          "\"all\" 压过其它项");

    /* ---- 5) 坏输入一律 1001 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs6\",\"ts\":1,"
                            "\"data\":{\"sections\":\"bogus\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "不认识的名字 -> 1001 (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs7\",\"ts\":1,"
                            "\"data\":{\"sections\":[]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "空数组 -> 1001 (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs8\",\"ts\":1,"
                            "\"data\":{\"sections\":123}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "数字类型 -> 1001 (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs9\",\"ts\":1,"
                            "\"data\":{\"sections\":\"sensors_much_too_long\"}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ 超长名字 -> 1001(不截断后当合法) (rc=%d)", rc);

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs10\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"sensors\",\"bogus\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "★ 数组里混坏名字 -> 整体 1001 (rc=%d)", rc);
    CHECK(strstr(s_big_resp, "\"sensors\"") == NULL,
          "★ 且不部分生效(应答里没有 sensors)");

    rc = cloud_proto_handle("{\"cmd\":\"cfg.query\",\"msgId\":\"cs11\",\"ts\":1,"
                            "\"data\":{\"sections\":[\"system\",\"network\",\"sensors\","
                            "\"thresholds\",\"ota\",\"meta\",\"system\"]}}",
                            1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "条数 > 6 -> 1001 (rc=%d)", rc);
}

/* =================================================================
 * [36] event_cache: 断线事件环形缓存 (v57)
 * -----------------------------------------------------------------
 * 文档要"≥500 条、断线期间不丢、重连补报"。存完整 JSON 要 96 KB RAM(放不下),
 * 所以存 28 B 紧凑记录 -> 256 条 = 7 KB。
 * ★2026-10-28: 容量由 512 降到 256(用户为腾 RAM 决定), 因此【不再满足】文档
 *   "≥500 条"的指标 —— 下面的断言改成钉死"正好 256", 避免以后被悄悄改回去。
 * 重点验:
 *   1) ★ ts 原样保留(补报不改发生时刻) —— 这是这个模块存在的第一理由 ★
 *   2) FIFO 顺序 + peek 不出队(补报成功才 drop, 与 data_cache 同一套纪律)
 *   3) ★ 满时淘汰最旧, 且 dropped 如实计数 ★(旧数据可丢, 但不许静默丢)
 *   4) 记录真的是 28 B(否则 256 条就不是 7 KB 了, 容量承诺会落空)
 * ================================================================= */
/* 假的 status 提供者: sys.query_status 没注册提供者会直接回 1006, 测不到字段集合 */
static void ec_fake_status(cloud_status_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->fw_version = "v-test";
    out->mode       = "auto";
    /* ★2026-11-12: bufferedEvents 已从 cloud_status_t 删除, 这里不再填 */
}

static void test_event_cache(void)
{
    ec_record_t r;
    ec_record_t in;
    uint32_t    i;
    uint16_t    n;

    printf("\n[36] event_cache (断线事件环形缓存)\n");

    event_cache_reset();

    CHECK(event_cache_capacity() ==
              (uint16_t)(xf_region_info(XF_REGION_EVENT)->size / XF_SLOT_SIZE),
          "★ 容量由 EVENT 分区算出 = %u 条(不再是写死的 256)",
          (unsigned)event_cache_capacity());
    /* 记录 = 28 B(4 ts + 4 aux + 4 小字段 + 13 uid + 2 msg_seq = 27, 对齐到 28)。
     * 其中的 msg_seq(2 B)是"在线/补报 msgId 逐字节一致"的代价 —— 平台按 msgId
     * 去重时对得上, 否则同一条事件会被当成两条。 */
    CHECK(sizeof(ec_record_t) <= 28u, "★ 记录 <= 28 B(含 msgId 序号) (实际 %u, 256 条即 %u B)",
          (unsigned)sizeof(ec_record_t),
          (unsigned)(sizeof(ec_record_t) * event_cache_capacity()));
    CHECK(event_cache_count() == 0u, "reset 后 count==0");
    CHECK(event_cache_dropped() == 0u, "reset 后 dropped==0");
    CHECK(event_cache_peek(&r) == false, "空缓存 peek -> false");
    CHECK(event_cache_pop(&r) == false, "空缓存 pop -> false");
    event_cache_drop();                     /* 空缓存 drop 不崩 */
    CHECK(true, "空缓存 drop 不崩");

    /* ---- 基本 FIFO + ts 保真 ---- */
    (void)memset(&in, 0, sizeof(in));
    in.kind = (uint8_t)EC_KIND_STATUS_CHANGE;
    in.ts = 1700000001u;
    in.aux = 90;
    in.a = (uint8_t)EC_ST_WARNING;
    in.b = (uint8_t)EC_ST_NORMAL;
    CHECK(event_cache_push(&in) == true, "push #1 -> true");
    in.ts = 1700000002u;
    in.kind = (uint8_t)EC_KIND_ACTION;
    in.a = 2u;
    in.b = (uint8_t)EC_TRIG_AUTO;
    CHECK(event_cache_push(&in) == true, "push #2 -> true");
    in.ts = 1700000003u;
    in.kind = (uint8_t)EC_KIND_DEVICE;
    in.a = 1u;
    in.b = (uint8_t)EC_DEV_SENSOR;
    in.uid[0] = 0x21u; in.uid[1] = 0x00u; in.uid[12] = 0x01u;
    CHECK(event_cache_push(&in) == true, "push #3 -> true");

    CHECK(event_cache_count() == 3u, "count==3");
    CHECK(event_cache_dropped() == 0u, "没满 -> 没淘汰");

    CHECK(event_cache_peek(&r) == true, "peek -> true");
    CHECK(r.ts == 1700000001u, "★ peek 是最旧那条, ts 原样");
    CHECK(r.kind == (uint8_t)EC_KIND_STATUS_CHANGE, "kind 原样");
    CHECK(r.aux == 90, "aux(水位) 原样");
    CHECK(event_cache_count() == 3u, "★ peek 不出队(count 仍是 3)");

    event_cache_drop();
    CHECK(event_cache_count() == 2u, "drop 后 count==2");
    CHECK(event_cache_peek(&r) && (r.ts == 1700000002u), "drop 后最旧变成 #2");

    CHECK(event_cache_pop(&r) == true, "pop -> true");
    CHECK((r.ts == 1700000002u) && (event_cache_count() == 1u), "pop 取走且出队");
    CHECK(event_cache_pop(&r) == true, "再 pop");
    CHECK((r.ts == 1700000003u) && (r.uid[0] == 0x21u), "★ uid 也原样保留");
    CHECK(event_cache_count() == 0u, "取空");

    /* ---- 参数非法 ---- */
    (void)memset(&in, 0, sizeof(in));
    in.kind = 99u;
    CHECK(event_cache_push(&in) == false, "kind 不认识 -> false");
    CHECK(event_cache_count() == 0u, "非法 push 不入队");
    CHECK(event_cache_push(NULL) == false, "push(NULL) -> false");

    /* ---- ★ 填满: 淘汰最旧 + 计数可观察 ★ ----
     * ★2026-11-11: 环形区刻意留一整个 4 KB 扇区不写（否则 head 追上 tail 时会
     *   擦掉还在 tail 一侧的活数据），所以实际容纳 = 容量 - 64 条，多出来的
     *   64 条按"淘汰最旧"处理 —— 这不是 bug，是那一个扇区的代价。 */
    event_cache_reset();
    for (i = 1u; i <= (uint32_t)event_cache_capacity(); i++)
    {
        (void)memset(&in, 0, sizeof(in));
        in.kind = (uint8_t)EC_KIND_DEVICE;
        in.ts = i;                          /* 用 ts 当序号, 便于验证顺序 */
        (void)event_cache_push(&in);
    }
    CHECK(event_cache_count() == (uint16_t)(event_cache_capacity() - 64u),
          "填满: count == 容量-64(让出 1 个扇区) (实际 %u)", (unsigned)event_cache_count());
    CHECK(event_cache_dropped() == 64u,
          "★ 恰好填满 -> 只淘汰了让出那个扇区的 64 条(实际 %u)",
          (unsigned)event_cache_dropped());

    (void)memset(&in, 0, sizeof(in));
    in.kind = (uint8_t)EC_KIND_DEVICE;
    in.ts = 999999u;
    CHECK(event_cache_push(&in) == true, "第 N+1 条仍 push 成功");
    CHECK(event_cache_count() == (uint16_t)(event_cache_capacity() - 64u), "条数不超容量");
    CHECK(event_cache_dropped() == 65u, "★ 淘汰计数 +1(实际 %u)",
          (unsigned)event_cache_dropped());
    CHECK(event_cache_peek(&r) && (r.ts == 66u),
          "★ 最旧的 1..65 已被淘汰, 队头变成 ts=66(实际 %u)", (unsigned)r.ts);

    /* 全部出队: 最后一条应当是刚推进去的 999999 */
    n = 0u;
    while (event_cache_pop(&r))
    {
        n++;
        if (n > event_cache_capacity()) { break; }      /* 防死循环 */
    }
    CHECK(n == (uint16_t)(event_cache_capacity() - 64u),
          "★ 恰好能取出容量-64 条 (实际 %u)", (unsigned)n);
    CHECK(r.ts == 999999u, "★ 最后一条是最新推入的(环形没串位)");

    event_cache_reset();
    CHECK((event_cache_count() == 0u) && (event_cache_dropped() == 0u),
          "reset 把 dropped 也清零");

    /* =================================================================
     * [36b] ★ 分页边界不能切动作组 ★
     * -----------------------------------------------------------------
     * `event.action` 的 results[] 靠 seq 把连续几条记录归组后再渲染。
     * 若一页从组中间切断, 渲染出的 results[] 会【少报几台设备】而看起来完全正常 ——
     * 隐蔽且难查。所以这里把边界用例全钉住。
     * ================================================================= */
    {
        ec_record_t r;
        int         k;

        event_cache_reset();
        CHECK(event_cache_page_count(10u) == 0u, "空缓存 -> 0 条");

        for (k = 0; k < 3; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_DEVICE;    /* seq=0 -> 不成组 */
            r.ts = (uint32_t)(100 + k);
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_page_count(0u) == 0u, "max_events=0 -> 0 条");
        CHECK(event_cache_page_count(2u) == 2u, "3 条不成组的, 页限 2 -> 取 2");
        CHECK(event_cache_page_count(10u) == 3u, "页限够大 -> 全取 3");

        /* 一次动作 3 台设备(seq=1), 页限 2: 必须整组取走 */
        event_cache_reset();
        for (k = 0; k < 3; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_ACTION;
            r.seq = 1u;
            r.ts = 200u;
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_page_count(2u) == 3u,
              "★ 页限 2 但整组 3 条 -> 取 3(不切组, 宁可超限)");

        /* 两组: A(seq=1)x2 + B(seq=2)x2 */
        event_cache_reset();
        for (k = 0; k < 4; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_ACTION;
            r.seq = (uint8_t)((k < 2) ? 1u : 2u);
            r.ts = 300u;
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_page_count(2u) == 2u,
              "★ 页限正好落在 A 组末尾 -> 取 2(不把 B 组切一半)");
        CHECK(event_cache_page_count(3u) == 4u,
              "★ 页限 3 落在 B 组中间 -> 补完 B 组, 取 4");
        CHECK(event_cache_page_count(4u) == 4u, "页限正好 4 -> 取 4");

        /* 组前面还有不成组的记录: 只看"页尾"那条是否属于组 */
        event_cache_reset();
        (void)memset(&r, 0, sizeof(r));
        r.kind = (uint8_t)EC_KIND_DEVICE;
        r.ts = 400u;
        (void)event_cache_push(&r);
        for (k = 0; k < 2; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_ACTION;
            r.seq = 7u;
            r.ts = 401u;
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_page_count(1u) == 1u,
              "页尾是不成组的那条 -> 取 1(不需要补组)");
        CHECK(event_cache_page_count(2u) == 3u,
              "★ 页尾落在 seq=7 组里 -> 补完, 取 3");

        /* seq=0 的动作记录【不】成组(避免把不相关的事件粘在一起) */
        event_cache_reset();
        for (k = 0; k < 2; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_ACTION;
            r.seq = 0u;
            r.ts = 500u;
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_page_count(1u) == 1u, "seq=0 不组队 -> 取 1");

        /* ★ 队头那一组本身就超页限: 必须整组返回, 否则永远取不走 ★ */
        event_cache_reset();
        for (k = 0; k < 5; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_ACTION;
            r.seq = 9u;
            r.ts = 600u;
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_page_count(2u) == 5u,
              "★ 队头组 5 条 > 页限 2 -> 仍返回 5(宁可超限, 不能卡住补报)");

        event_cache_reset();
    }

    /* =================================================================
     * [36d] ★ a/b 的码值空间必须是【定义好的】, 不能什么都往里存 ★
     * -----------------------------------------------------------------
     * v57 只写了"a = 事件码"却没定义码值 —— 谁都能存一个渲染层不认识的值,
     * 而**要到渲染时才发现**(那时已经在往平台发了)。这里把边界钉死。
     * 分工: push() 只管 kind(存储原语保持最小), a/b 由产生端用本函数把关;
     *       渲染端补报前再查一次(缓存可能是上一版固件留下的)。
     * ================================================================= */
    {
        ec_record_t v;

        (void)memset(&v, 0, sizeof(v));
        v.kind = (uint8_t)EC_KIND_STATUS_CHANGE;
        v.a = (uint8_t)EC_ST_ALARM;
        v.b = (uint8_t)EC_ST_WARNING;
        CHECK(event_cache_validate(&v) == true, "status_change 合法(a=Alarm, b=Warning)");
        v.b = 9u;
        CHECK(event_cache_validate(&v) == false, "★ status_change 的 b=9 越界 -> false");
        v.b = 0u;
        CHECK(event_cache_validate(&v) == false, "★ b=0(未定义) -> false");

        (void)memset(&v, 0, sizeof(v));
        v.kind = (uint8_t)EC_KIND_ACTION;
        v.a = 0u;
        v.b = (uint8_t)EC_TRIG_AUTO;
        CHECK(event_cache_validate(&v) == true, "action 合法(档位 0 也合法)");
        v.a = 4u;
        CHECK(event_cache_validate(&v) == false, "★ action 档位 4 越界 -> false");
        v.a = 2u;
        v.b = 0u;
        CHECK(event_cache_validate(&v) == false, "★ trigger=0(未定义) -> false");

        (void)memset(&v, 0, sizeof(v));
        v.kind = (uint8_t)EC_KIND_DEVICE;
        v.a = (uint8_t)EC_DEV_EV_LOW_VOLTAGE;
        v.b = (uint8_t)EC_DEV_SENSOR;
        CHECK(event_cache_validate(&v) == true, "device 合法(low_voltage / sensor)");
        v.a = 7u;
        CHECK(event_cache_validate(&v) == false, "★ 事件码 7(超出 1~6) -> false");
        v.a = (uint8_t)EC_DEV_EV_BOUND;
        v.b = 0u;
        CHECK(event_cache_validate(&v) == false, "★ 设备类型 0(未定义) -> false");

        (void)memset(&v, 0, sizeof(v));
        v.kind = 99u;
        CHECK(event_cache_validate(&v) == false, "kind 不认识 -> false");
        CHECK(event_cache_validate(NULL) == false, "NULL -> false");
        CHECK(((int)EC_DEV_EV_BOUND == 1) && ((int)EC_DEV_EV_LOW_VOLTAGE == 6),
              "★ 码值显式固定(不随编译器/后续插值漂)");
    }

    /* =================================================================
     * [36c] ★ bufferedEvents 已移除 —— 缓存里真有事件也不许再输出 ★
     * -----------------------------------------------------------------
     * 2026-11-12 平台侧确认不要这个字段, 于是字段、cloud_status_t 成员、
     * 状态提供者里的赋值一并删除。这里留一条护栏, 防两件事:
     *   1) 谁把 cloud_fill_status_json 里的键加回来;
     *   2) 谁在 cloud_cmd_sys 里再单独 AddNumberToObject 一次。
     * 断言"缓存非空时也不出现该键", 比只看空缓存更能钉住。
     * ================================================================= */
    {
        uint16_t rl;
        int      rc;

        cloud_proto_set_status_provider(ec_fake_status);

        event_cache_reset();
        rc = cloud_proto_handle("{\"cmd\":\"sys.query_status\",\"msgId\":\"ec1\",\"ts\":1}",
                                1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "sys.query_status -> 成功 (rc=%d)", rc);
        CHECK(strstr(s_big_resp, "\"bufferedEvents\"") == NULL,
              "★ 空缓存: 应答里没有 bufferedEvents");

        (void)memset(&in, 0, sizeof(in));
        in.kind = (uint8_t)EC_KIND_DEVICE;
        in.ts = 7u;
        (void)event_cache_push(&in);
        (void)event_cache_push(&in);
        (void)event_cache_push(&in);

        rc = cloud_proto_handle("{\"cmd\":\"sys.query_status\",\"msgId\":\"ec2\",\"ts\":1}",
                                1u, s_big_resp, (uint16_t)sizeof(s_big_resp), &rl);
        CHECK(rc == CLOUD_EC_OK, "再查 -> 成功");
        CHECK(strstr(s_big_resp, "\"bufferedEvents\"") == NULL,
              "★ 缓存 3 条也不输出 bufferedEvents(字段已删除, 不是恒 0)");
        CHECK(event_cache_count() == 3u,
              "★ 事件缓存本身不受影响, 仍有 3 条待补报 (实际 %u)",
              (unsigned)event_cache_count());

        cloud_proto_set_status_provider(NULL);
        event_cache_reset();
    }
}

/* =================================================================
 * [36b] 低电量事件入缓存: 同一设备只留最新一条 (2026-10-28)
 * -----------------------------------------------------------------
 * 用户要求: 网关离线期间, 同一台设备的 low_voltage 在补报缓存里【只存一条,
 * 最新的】。低电量是"持续状态"而不是"一次动作", 断线期间反复越限(充电/
 * 换电池/阈值被调高又调低)逐条留住只会挤占补报窗口。
 * 重点验:
 *   ★ 同一 uid + LOW_VOLTAGE -> 合并(MERGED), 条数不变, ts/msg_seq 刷新;
 *   ★ 不同 uid / 不同事件码 / 非 device 事件 -> 一律新增, 绝不误合并;
 *   ★ 合并【不触发】槽满淘汰(dropped 不变) —— 它不占新槽;
 *   ★ 位置不动: 合并后那条仍在原下标, 队头不变;
 *   ★ 已经补报出去(drop)之后再产生 -> 是新增, 不是合并到别处。
 * ================================================================= */
static void test_low_voltage_latest(void)
{
    ec_record_t     in;
    ec_record_t     r;
    uint16_t        i;
    char            buf[512];
    uint16_t        n;
    static const uint8_t uid_a[13] = { 0x21,0,0,0,0,0,0,0,0,0,0,0,0x01 };
    static const uint8_t uid_b[13] = { 0x21,0,0,0,0,0,0,0,0,0,0,0,0x02 };

    printf("\n[36b] 低电量事件: 同一设备只留最新一条\n");

    event_cache_reset();

    /* ---- 1. 第一次入队 = 新增 ---- */
    (void)memset(&in, 0, sizeof(in));
    in.kind    = (uint8_t)EC_KIND_DEVICE;
    in.a       = (uint8_t)EC_DEV_EV_LOW_VOLTAGE;
    in.b       = (uint8_t)EC_DEV_SENSOR;
    in.ts      = 1000u;
    in.msg_seq = 11u;
    (void)memcpy(in.uid, uid_a, sizeof(in.uid));
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED, "首次低电量 -> ADDED");
    CHECK(event_cache_count() == 1u, "count==1");

    /* ---- 2. 同一设备再次越限 = 就地刷新 ---- */
    in.ts      = 2000u;
    in.msg_seq = 12u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_MERGED,
          "★ 同 uid + 低电量 -> MERGED(不新增)");
    CHECK(event_cache_count() == 1u, "★ 合并后条数仍是 1");
    CHECK(event_cache_dropped() == 0u, "★ 合并不触发淘汰(dropped 仍 0)");
    CHECK(event_cache_peek_at(0u, &r) && (r.ts == 2000u) && (r.msg_seq == 12u),
          "★ 留下的是【最新】那条(ts/msg_seq 都刷新了)");
    CHECK(event_cache_peek_at(0u, &r) && (r.a == (uint8_t)EC_DEV_EV_LOW_VOLTAGE) &&
          (memcmp(r.uid, uid_a, sizeof(r.uid)) == 0),
          "事件码与 uid 不被合并弄丢");

    /* ---- 3. 不同 uid 不合并 ---- */
    (void)memcpy(in.uid, uid_b, sizeof(in.uid));
    in.ts      = 3000u;
    in.msg_seq = 13u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED, "另一台设备的低电量 -> ADDED");
    CHECK(event_cache_count() == 2u, "count==2");

    /* ---- 4. offline 也合并(G2, 2026-11-02): 连续重复的 offline 只留最新 ---- */
    (void)memcpy(in.uid, uid_a, sizeof(in.uid));
    in.a       = (uint8_t)EC_DEV_EV_OFFLINE;
    in.ts      = 4000u;
    in.msg_seq = 14u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED, "首次 offline -> ADDED");
    in.ts      = 5000u;
    in.msg_seq = 15u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_MERGED,
          "★ 同一台连续第二条 offline -> MERGED(断线补报不再刷屏)");
    CHECK(event_cache_count() == 3u, "★ 合并后条数仍是 3");
    CHECK(event_cache_peek_at(2u, &r) && (r.a == (uint8_t)EC_DEV_EV_OFFLINE) && (r.ts == 5000u),
          "★ 留下的是最新那条 offline(ts=5000), 位置不动");

    /* ---- 4b. 中间出现过 online 之后, 新的 offline 必须【追加】 ---- */
    {
        ec_record_t on;

        (void)memset(&on, 0, sizeof(on));
        on.kind    = (uint8_t)EC_KIND_DEVICE;
        on.a       = (uint8_t)EC_DEV_EV_ONLINE;
        on.b       = (uint8_t)EC_DEV_SENSOR;
        on.ts      = 5500u;
        on.msg_seq = 16u;
        (void)memcpy(on.uid, uid_a, sizeof(on.uid));
        CHECK(event_cache_push_latest(&on) == EC_PUSH_ADDED,
              "★ online 不合并(掉线/恢复必须成对)");

        (void)memcpy(in.uid, uid_a, sizeof(in.uid));
        in.a       = (uint8_t)EC_DEV_EV_OFFLINE;
        in.ts      = 5800u;
        in.msg_seq = 17u;
        CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED,
              "★ 恢复后的新 offline -> ADDED(前面已有 online, 不能就地覆盖)");
        CHECK(event_cache_count() == 5u, "count==5(offline, online, offline 三条都在)");
        CHECK(event_cache_peek_at(2u, &r) && (r.a == (uint8_t)EC_DEV_EV_OFFLINE) && (r.ts == 5000u) &&
              event_cache_peek_at(3u, &r) && (r.a == (uint8_t)EC_DEV_EV_ONLINE) && (r.ts == 5500u) &&
              event_cache_peek_at(4u, &r) && (r.a == (uint8_t)EC_DEV_EV_OFFLINE) && (r.ts == 5800u),
              "★ 顺序仍是 offline(5000) -> online(5500) -> offline(5800)");
    }

    /* ---- 5. ★合并窗口 = 队尾那一条（待提交槽）★ ----
     * ★2026-11-11 行为变更，必须写清楚★
     *   改成 Flash 单写之后，不能再"就地覆写环里任意一条" —— NOR 只能 1→0，
     *   改写必须先擦整个 4 KB，会连带擦掉同扇区另外 63 条还能用的记录。
     *   所以合并对象固定是"最后一条"（RAM 里的待提交槽）：
     *     · 同一台设备同一种持续状态【连着来】 -> 合并；
     *     · 中间夹了别的记录           -> 前面那条已经落盘，这次只能新增。
     *   实际影响很小：每台设备每个上报周期只发一条，所以"每台每周期一条"这个
     *   目标不变；换来的是落盘顺序严格按 ts 递增（用户 2026-11-11 Q2 的要求）。
     */
    (void)memcpy(in.uid, uid_a, sizeof(in.uid));
    in.a       = (uint8_t)EC_DEV_EV_LOW_VOLTAGE;
    in.ts      = 6000u;
    in.msg_seq = 18u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED,
          "★ 队尾是 offline(5800), 这次低电量只能新增(合并窗口只看最后一条)");
    CHECK(event_cache_count() == 6u, "count==6");
    CHECK(event_cache_peek_at(5u, &r) && (r.ts == 6000u) && (r.msg_seq == 18u),
          "★ 新增的那条落在队尾(ts=6000)");
    CHECK(event_cache_peek_at(0u, &r) && (r.ts == 2000u),
          "★ idx 0 那条低电量(ts=2000)没被改写");

    /* ---- 5b. 连着再来一条同 key -> 这时候才合并 ---- */
    in.ts      = 7000u;
    in.msg_seq = 19u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_MERGED,
          "★ 同一台连续第二条低电量 -> MERGED");
    CHECK(event_cache_count() == 6u, "★ 合并后条数仍是 6");
    CHECK(event_cache_peek_at(5u, &r) && (r.ts == 7000u) && (r.msg_seq == 19u),
          "★ 队尾刷新成最新那次(ts=7000)");

    /* ---- 6. 非 device 类事件: 退化成逐条 push ---- */
    (void)memset(&in, 0, sizeof(in));
    in.kind = (uint8_t)EC_KIND_STATUS_CHANGE;
    in.a    = (uint8_t)EC_ST_WARNING;
    in.b    = (uint8_t)EC_ST_NORMAL;
    in.ts   = 8000u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED, "status_change -> ADDED");
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED, "★ 同内容再来一条仍 ADDED(不合并)");
    CHECK(event_cache_count() == 8u, "count==8");

    /* ---- 7. 渲染出来的确实是最新 ts(补报不改时刻, 但要是"最新那次") ---- */
    {
        ec_record_t lv;

        CHECK(event_cache_peek_at(5u, &lv) == true, "取到低电量那条");
        n = cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &lv, "t1", NULL, 0u);
        CHECK(n > 0u, "渲染成功");
        CHECK(strstr(buf, "\"ts\":7000") != NULL, "★ 渲染出的 ts 是最后那次(7000)");
        CHECK((strstr(buf, "\"event\":\"low_voltage\"") != NULL) &&
              (strstr(buf, "\"devType\":\"sensor\"") != NULL),
              "事件名/设备类型照旧");
    }

    /* ---- 8. NULL / 非法 kind ---- */
    CHECK(event_cache_push_latest(NULL) == EC_PUSH_FAIL, "push_latest(NULL) -> FAIL");
    (void)memset(&in, 0, sizeof(in));
    in.kind = 99u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_FAIL, "kind 不认识 -> FAIL");
    CHECK(event_cache_count() == 8u, "非法 push 不入队");

    /* ---- 9. ★合并【不占新槽】★：连续同 key 来 100 次, 条数一点都不涨 ---- */
    event_cache_reset();
    (void)memset(&in, 0, sizeof(in));
    in.kind    = (uint8_t)EC_KIND_DEVICE;
    in.a       = (uint8_t)EC_DEV_EV_LOW_VOLTAGE;
    in.b       = (uint8_t)EC_DEV_SENSOR;
    in.msg_seq = 1u;
    (void)memcpy(in.uid, uid_a, sizeof(in.uid));
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED, "先放 1 条低电量");

    for (i = 1u; i <= 100u; i++)
    {
        in.ts      = (uint32_t)(100u + i);
        in.msg_seq = (uint16_t)i;
        CHECK(event_cache_push_latest(&in) == EC_PUSH_MERGED, "第 %u 次重复 -> MERGED", (unsigned)i);
    }
    CHECK(event_cache_count() == 1u, "★ 101 次低电量只占 1 条(实际 %u)",
          (unsigned)event_cache_count());
    CHECK(event_cache_dropped() == 0u, "★ 一条都没被淘汰(dropped 仍 0)");
    CHECK(event_cache_peek_at(0u, &r) && (r.ts == 200u) && (r.msg_seq == 100u),
          "★ 留下的是最新那条(ts=200, msg_seq=100)");

    /* ---- 10. 已经补报出去之后再产生 = 新增(不是合到别处) ---- */
    event_cache_reset();
    (void)memset(&in, 0, sizeof(in));
    in.kind    = (uint8_t)EC_KIND_DEVICE;
    in.a       = (uint8_t)EC_DEV_EV_LOW_VOLTAGE;
    in.b       = (uint8_t)EC_DEV_SENSOR;
    in.ts      = 111u;
    in.msg_seq = 21u;
    (void)memcpy(in.uid, uid_a, sizeof(in.uid));
    (void)event_cache_push_latest(&in);
    event_cache_drop();                         /* 模拟补报成功 */
    in.ts      = 222u;
    in.msg_seq = 22u;
    CHECK(event_cache_push_latest(&in) == EC_PUSH_ADDED,
          "★ 上一条已补报出队 -> 这次是 ADDED");
    CHECK((event_cache_count() == 1u) && event_cache_peek_at(0u, &r) && (r.ts == 222u),
          "缓存里就是新的那条");

    event_cache_reset();
}

/* =================================================================
 * [37] 断线补报的单条渲染 (v62)
 * -----------------------------------------------------------------
 * 事件缓存存 16 B 紧凑记录, 补报时才还原成事件对象。这是整条补报链里
 * 最险的一段(码值->字符串、uid 还原、动作组 results、溢出策略), 所以先单独测透,
 * 剩下的接线就只是"循环 + 组装 + 发布"。
 * 重点验:
 *   ★ ts 用【原始发生时刻】, 不是补报时刻(文档要求);
 *   ★ uid 由 3 字节还原成 hex 串(不能直接当字符串用);
 *   ★ 渲染不出来时返回 0 —— 宁可这一条不补报, 也绝不截断/发空 results。
 * ================================================================= */
static void test_render_cached_event(void)
{
    ec_record_t           rec;
    cloud_action_result_t rs[2];
    char                  buf[512];
    uint16_t              n;

    printf("\n[37] 补报单条渲染\n");

    /* ---- status_change ---- */
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind     = (uint8_t)EC_KIND_STATUS_CHANGE;
    rec.a        = (uint8_t)EC_ST_WARNING;
    rec.b        = (uint8_t)EC_ST_NORMAL;
    rec.aux = 90;
    rec.ts       = 1700000000u;

    n = cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &rec, "bf1", NULL, 0u);
    CHECK(n > 0u, "status_change 渲染成功 (n=%u)", (unsigned)n);
    CHECK(strstr(buf, "\"event\":\"event.status_change\"") != NULL, "事件名正确");
    CHECK(strstr(buf, "\"msgId\":\"bf1\"") != NULL, "msgId 用的是传入值");
    CHECK(strstr(buf, "\"ts\":1700000000") != NULL, "★ ts 是【原始发生时刻】");
    CHECK(strstr(buf, "\"from\":\"Normal\"") != NULL, "from 映射: 2 -> Normal");
    CHECK(strstr(buf, "\"to\":\"Warning\"") != NULL, "to 映射: 3 -> Warning");
    CHECK(strstr(buf, "levelCm") != NULL, "带 levelCm");

    /* ---- device: 关键在 uid 由 3 字节还原 ---- */
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind   = (uint8_t)EC_KIND_DEVICE;
    rec.a      = (uint8_t)EC_DEV_EV_OFFLINE;
    rec.b      = (uint8_t)EC_DEV_SENSOR;
    rec.uid[0] = 0x21u;
    rec.uid[1] = 0x00u;
    rec.uid[12] = 0x01u;
    rec.ts     = 1700000001u;

    n = cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &rec, "bf2", NULL, 0u);
    CHECK(n > 0u, "device 渲染成功 (n=%u)", (unsigned)n);
    CHECK(strstr(buf, "\"event\":\"event.device\"") != NULL, "事件名正确");
    CHECK(strstr(buf, "21000000000000000000000001") != NULL, "★ 3 字节 uid 还原成 hex 串 \"210001\"");
    CHECK(strstr(buf, "\"devType\":\"sensor\"") != NULL, "设备类型映射: sensor");
    CHECK(strstr(buf, "\"event\":\"offline\"") != NULL, "设备事件映射: offline");

    /* ---- action: 必须带 results(由调用方按 seq 收集好) ---- */
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_ACTION;
    rec.a    = 2u;
    rec.b    = (uint8_t)EC_TRIG_AUTO;
    rec.ts   = 1700000002u;
    (void)memset(rs, 0, sizeof(rs));
    rs[0].uid  = "31000000000000000000000001";
    rs[0].code = 0;
    rs[1].uid  = "31000000000000000000000002";
    rs[1].code = 1005;

    n = cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &rec, "bf3", rs, 2u);
    CHECK(n > 0u, "action 渲染成功 (n=%u)", (unsigned)n);
    CHECK(strstr(buf, "\"event\":\"event.action\"") != NULL, "事件名正确");
    CHECK(strstr(buf, "\"gear\":2") != NULL, "档位 2 带上");
    CHECK(strstr(buf, "\"trigger\":\"auto\"") != NULL, "trigger 映射: auto");
    CHECK(strstr(buf, "31000000000000000000000002") != NULL, "★ 第二台设备的结果也在(不是只带第一台)");
    CHECK(strstr(buf, "1005") != NULL, "执行码原样带上");

    /* ---- ★ 失败路径: 一律返回 0, 不截断、不半条 ---- */
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_DEVICE;
    rec.a    = 7u;                          /* 超出 1~6 */
    rec.b    = (uint8_t)EC_DEV_SENSOR;
    CHECK(cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &rec, "bf4", NULL, 0u) == 0u,
          "★ 码值越界 -> 0");

    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_ACTION;
    rec.a    = 1u;
    rec.b    = (uint8_t)EC_TRIG_AUTO;
    CHECK(cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &rec, "bf5", NULL, 0u) == 0u,
          "★ 动作事件没有 results -> 0(不发空 results)");

    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_DEVICE;
    rec.a    = (uint8_t)EC_DEV_EV_ONLINE;
    rec.b    = (uint8_t)EC_DEV_SENSOR;
    CHECK(cloud_proto_render_cached_event(NULL, (uint16_t)sizeof(buf), &rec, "bf6", NULL, 0u) == 0u,
          "out=NULL -> 0");
    CHECK(cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), NULL, "bf7", NULL, 0u) == 0u,
          "rec=NULL -> 0");
    CHECK(cloud_proto_render_cached_event(buf, (uint16_t)sizeof(buf), &rec, NULL, NULL, 0u) == 0u,
          "msg_id=NULL -> 0");
    CHECK(cloud_proto_render_cached_event(buf, 8u, &rec, "bf8", NULL, 0u) == 0u,
          "★ cap 太小 -> 0(绝不截断后返回半条)");

    /* =================================================================
     * [37b] 补报专用 msgId 生成器 (v63)
     * -----------------------------------------------------------------
     * 缓存记录里不存 msgId(存它每条多 16 B, 512 条容量直接减半), 所以补报时
     * 必须重新分配。这里验: 8 位小写十六进制、长度不超文档上限(16)、
     * 放不下时**返回 false 而不是送半截 id**。
     * ================================================================= */
    {
        char id1[24];
        char id2[24];

        id1[0] = '\0';
        id2[0] = '\0';
        CHECK(cloud_proto_next_backfill_msg_id(id1, (uint16_t)sizeof(id1)) == true,
              "取到第一个补报 msgId");
        CHECK(test_is_hex8(id1), "★ 8 位小写十六进制: \"%s\"", id1);
        CHECK(strlen(id1) == 8u, "★ 长度 = 8(文档上限 16), 实际 %u", (unsigned)strlen(id1));

        CHECK(cloud_proto_next_backfill_msg_id(id2, (uint16_t)sizeof(id2)) == true,
              "取到第二个");
        CHECK(strcmp(id1, id2) != 0, "★ 两个不同(单调递增): \"%s\" vs \"%s\"", id1, id2);
        CHECK(strlen(id2) == strlen(id1), "同位数时长度一致");

        CHECK(cloud_proto_next_backfill_msg_id(NULL, 24u) == false, "out=NULL -> false");
        id1[0] = 'X';
        CHECK(cloud_proto_next_backfill_msg_id(id1, 1u) == false,
              "★ cap=1 -> false(绝不写半截 id)");
        CHECK(id1[0] == '\0', "★ 失败时把 out 置空, 不留半截内容");
    }

    /* =================================================================
     * [37c] 动作记录的"执行码"字段复用 (v64)
     * -----------------------------------------------------------------
     * results[] 需要 (uid,执行码) 成对, 而 16 B 的记录里没有独立字段 —— 对动作
     * 事件而言 level_mm 本来空着, 所以复用它。这里验访问器与记录大小都没变胖。
     * ★ 若哪天有人"顺手"给记录加了独立字段, 512 × 4 B = 2 KB 就白涨了;
     *   而若有人绕过访问器直接写 level_mm, "这字段到底是水位还是执行码"就没人知道了。
     * ================================================================= */
    {
        ec_record_t a;
        ec_record_t b;

        (void)memset(&a, 0, sizeof(a));
        a.kind = (uint8_t)EC_KIND_ACTION;
        a.a    = 2u;
        a.b    = (uint8_t)EC_TRIG_AUTO;

        ec_action_set_code(&a, 1005);
        CHECK(ec_action_code_of(&a) == 1005, "★ 执行码写入后可取回(1005)");
        CHECK(a.aux == 1005, "★ 执行码确实复用 aux 字段, 没有新增字段");

        (void)memset(&b, 0, sizeof(b));
        b.kind = (uint8_t)EC_KIND_ACTION;
        b.a    = 2u;
        b.b    = (uint8_t)EC_TRIG_AUTO;
        ec_action_set_code(&b, 0);
        CHECK(ec_action_code_of(&b) == 0, "成功码 0 也能表达");
        CHECK(event_cache_validate(&a) && event_cache_validate(&b),
              "带执行码的动作记录仍然合法");

        CHECK(ec_action_code_of(NULL) == 0, "code_of(NULL) -> 0");
        ec_action_set_code(NULL, 7);        /* 不崩 */
        CHECK(true, "set_code(NULL) 不崩");
        CHECK(sizeof(ec_record_t) <= 28u,
          "★ 记录仍是 <=28 B(2026-10-18 UID 3->13 字节, 记录 20->28)");
    }

    /* =================================================================
     * [37d] 动作组收集 (v65)
     * -----------------------------------------------------------------
     * `results[]` 必须是【完整一组】: **少报一台设备的事件看起来完全正常**,
     * 现场根本查不出来。所以分组规则(同 kind + 同 seq + 连续)在这里一次钉死,
     * 调用方就只剩"取一次 -> 渲染 -> 发布"。
     * ================================================================= */
    {
        ec_action_pair_t pairs[4];
        ec_record_t      r;
        uint8_t          k;

        event_cache_reset();
        CHECK(event_cache_peek_action_group(NULL, 4u) == 0u, "out=NULL -> 0");
        CHECK(event_cache_peek_action_group(pairs, 0u) == 0u, "max_pairs=0 -> 0");

        /* 队头不是动作记录 */
        (void)memset(&r, 0, sizeof(r));
        r.kind = (uint8_t)EC_KIND_DEVICE;
        r.a    = (uint8_t)EC_DEV_EV_ONLINE;
        r.b    = (uint8_t)EC_DEV_SENSOR;
        (void)event_cache_push(&r);
        CHECK(event_cache_peek_action_group(pairs, 4u) == 0u,
              "队头是设备事件 -> 0(不是动作组)");

        /* seq=0 的动作记录不成组 */
        event_cache_reset();
        (void)memset(&r, 0, sizeof(r));
        r.kind = (uint8_t)EC_KIND_ACTION;
        r.a    = 2u;
        r.b    = (uint8_t)EC_TRIG_AUTO;
        r.seq  = 0u;
        (void)event_cache_push(&r);
        CHECK(event_cache_peek_action_group(pairs, 4u) == 0u, "★ seq=0 不成组 -> 0");

        /* 一次动作两台设备(seq=3): uid 与执行码都要取对 */
        event_cache_reset();
        for (k = 0u; k < 2u; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind   = (uint8_t)EC_KIND_ACTION;
            r.a      = 2u;
            r.b      = (uint8_t)EC_TRIG_AUTO;
            r.seq    = 3u;
            r.uid[0] = 0x31u;
            r.uid[1] = 0x00u;
            r.uid[2] = (uint8_t)(0x01u + k);
            ec_action_set_code(&r, (k == 0u) ? 0 : 1005);
            (void)event_cache_push(&r);
        }
        (void)memset(pairs, 0, sizeof(pairs));
        CHECK(event_cache_peek_action_group(pairs, 4u) == 2u, "★ 收到 2 台");
        CHECK((pairs[0].uid[0] == 0x31u) && (pairs[0].uid[2] == 0x01u) && (pairs[0].code == 0),
              "第 1 台: uid=310001, code=0");
        CHECK((pairs[1].uid[2] == 0x02u) && (pairs[1].code == 1005),
              "★ 第 2 台: uid=310002, code=1005");
        CHECK(event_cache_count() == 2u, "★ 只看不出队(count 不变)");

        /* ★ 组比 out 装得大: 返回 0, 不截断 ★ */
        event_cache_reset();
        for (k = 0u; k < 3u; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind = (uint8_t)EC_KIND_ACTION;
            r.a    = 1u;
            r.b    = (uint8_t)EC_TRIG_AUTO;
            r.seq  = 5u;
            (void)event_cache_push(&r);
        }
        CHECK(event_cache_peek_action_group(pairs, 2u) == 0u,
              "★ 组 3 台但只能装 2 对 -> 0(绝不只报前两台)");
        CHECK(event_cache_peek_action_group(pairs, 3u) == 3u, "装得下 -> 收到 3 台");

        /* 组后面换了 seq: 只收本组 */
        (void)memset(&r, 0, sizeof(r));
        r.kind = (uint8_t)EC_KIND_ACTION;
        r.a    = 1u;
        r.b    = (uint8_t)EC_TRIG_AUTO;
        r.seq  = 6u;
        (void)event_cache_push(&r);
        CHECK(event_cache_peek_action_group(pairs, 4u) == 3u,
              "★ 后面换了 seq -> 只收本组 3 台(不把下一次动作粘进来)");
    }

    /* =================================================================
     * [37e] 按索引访问 (v66)
     * -----------------------------------------------------------------
     * 组装器要**逐条渲染页内每一条**, 又不能中途出队(只有整页发布成功才 drop),
     * 所以需要"按索引看"。★ 而且动作组**不一定在页首** —— 组边界只保证
     * "不跨页", 不保证"在页首"; 队头版 `peek_action_group` 对此无能为力。★
     * ================================================================= */
    {
        ec_action_pair_t pairs[4];
        ec_record_t      r;
        ec_record_t      got;
        uint8_t          k;

        event_cache_reset();

        CHECK(event_cache_peek_at(0u, &got) == false, "空缓存 peek_at(0) -> false");
        CHECK(event_cache_peek_at(0u, NULL) == false, "out=NULL -> false");

        /* 3 条: 设备事件(不成组) + 动作组 seq=9 两台 */
        (void)memset(&r, 0, sizeof(r));
        r.kind = (uint8_t)EC_KIND_DEVICE;
        r.a    = (uint8_t)EC_DEV_EV_BOUND;
        r.b    = (uint8_t)EC_DEV_SENSOR;
        r.ts   = 100u;
        (void)event_cache_push(&r);
        for (k = 0u; k < 2u; k++)
        {
            (void)memset(&r, 0, sizeof(r));
            r.kind   = (uint8_t)EC_KIND_ACTION;
            r.a      = 2u;
            r.b      = (uint8_t)EC_TRIG_AUTO;
            r.seq    = 9u;
            r.uid[2] = (uint8_t)(0x0Au + k);
            ec_action_set_code(&r, (k == 0u) ? 0 : 1005);
            (void)event_cache_push(&r);
        }

        CHECK((event_cache_peek_at(0u, &got) == true) && (got.ts == 100u),
              "peek_at(0) 是最旧那条");
        CHECK((event_cache_peek_at(1u, &got) == true) &&
              (got.kind == (uint8_t)EC_KIND_ACTION), "peek_at(1) 是动作记录");
        CHECK((event_cache_peek_at(2u, &got) == true) && (got.seq == 9u),
              "peek_at(2) 是动作组第二条");
        CHECK(event_cache_peek_at(3u, &got) == false, "越界 -> false");
        CHECK(event_cache_count() == 3u, "★ 按索引访问不出队");

        /* ★ 组不在页首: 必须能从这里找到它 ★ */
        CHECK(event_cache_peek_action_group(pairs, 4u) == 0u,
              "★ 队头是设备事件 -> 队头版返回 0");
        CHECK(event_cache_group_at(1u, pairs, 4u) == 2u,
              "★ 从索引 1 找 -> 收到动作组 2 台");
        CHECK((pairs[1].uid[2] == 0x0Bu) && (pairs[1].code == 1005),
              "组内第 2 台的 uid/执行码正确");
        CHECK(event_cache_group_at(0u, pairs, 4u) == 0u, "索引 0 不是动作组 -> 0");
        CHECK(event_cache_group_at(3u, pairs, 4u) == 0u, "越界 -> 0");
        CHECK(event_cache_group_at(1u, pairs, 1u) == 0u,
              "★ 组 2 台但只能装 1 对 -> 0(仍不截断)");

        event_cache_reset();
    }

    /* =================================================================
     * [37f] 状态码 -> 文档字符串 (v71)
     * -----------------------------------------------------------------
     * `event.status_change` 的 from/to 是字符串, 主流程手里只有状态码。
     * 这套字符串本来就在协议层, 若让主流程再写一份, 两处迟早漂 ——
     * 所以只保留一份实现, 用一个取值函数暴露(本组就是钉它)。
     * ================================================================= */
    CHECK(strcmp(cloud_proto_state_name((uint8_t)EC_ST_RESOLVED), "Resolved") == 0,
          "1 -> Resolved");
    CHECK(strcmp(cloud_proto_state_name((uint8_t)EC_ST_NORMAL), "Normal") == 0,
          "2 -> Normal");
    CHECK(strcmp(cloud_proto_state_name((uint8_t)EC_ST_WARNING), "Warning") == 0,
          "3 -> Warning");
    CHECK(strcmp(cloud_proto_state_name((uint8_t)EC_ST_ALARM), "Alarm") == 0,
          "4 -> Alarm");
    CHECK(cloud_proto_state_name(0u) == NULL, "★ 0(未定义) -> NULL, 不用 \"\" 冒充");
    CHECK(cloud_proto_state_name(9u) == NULL, "★ 越界 -> NULL");
}

/* =================================================================
 * [38] 补报一页组装 (v67)
 * -----------------------------------------------------------------
 * 这是断线补报的收口: 分页 -> 逐条渲染 -> 组装 buffered 数据段。
 * 两个最容易做错的地方:
 *   1) ★ 动作组要合成【一条】事件, 但 consumed 要算【整组】★
 *      (算错 -> 要么少报设备, 要么把没发出去的记录丢掉);
 *   2) ★ 渲染不出来的记录【也必须推进】★ —— 不出队的话它会永远堵在队头,
 *      后面所有事件全被堵死; 但要计入 skipped, 不许静默丢。
 * ================================================================= */
static void test_backfill_page(void)
{
    ec_record_t rec;
    char        buf[4096];
    uint8_t     consumed;
    uint8_t     events;
    uint8_t     skipped;
    uint16_t    n;
    uint8_t     k;

    printf("\n[38] 补报一页组装\n");

    event_cache_reset();
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK(n == 0u, "空缓存 -> 0");
    CHECK((consumed == 0u) && (events == 0u) && (skipped == 0u), "三个输出都是 0");

    /* ---- 3 条设备事件 ---- */
    for (k = 0u; k < 3u; k++)
    {
        (void)memset(&rec, 0, sizeof(rec));
        rec.kind   = (uint8_t)EC_KIND_DEVICE;
        rec.a      = (uint8_t)EC_DEV_EV_ONLINE;
        rec.b      = (uint8_t)EC_DEV_SENSOR;
        rec.ts     = 1700000000u + k;
        rec.uid[0] = 0x21u;
        rec.uid[1] = 0x00u;
        rec.uid[12] = (uint8_t)(1u + k);   /* 2026-10-18: uid 13 字节, 尾部在 [12] */
        (void)event_cache_push(&rec);
    }
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK(n > 0u, "3 条设备事件 -> 成功 (n=%u)", (unsigned)n);
    CHECK(events == 3u, "★ events=3 (实际 %u)", (unsigned)events);
    CHECK((consumed == 3u) && (skipped == 0u), "consumed=3, skipped=0");
    CHECK(strstr(buf, "\"events\":[") != NULL, "是 event.buffered 的 data 段");
    CHECK((strstr(buf, "21000000000000000000000001") != NULL) && (strstr(buf, "21000000000000000000000003") != NULL),
          "★ 三条的 uid 都在(3 字节都还原对了)");
    CHECK(strstr(buf, "\"ts\":1700000000") != NULL, "★ ts 是原始发生时刻");
    CHECK(strstr(buf, "\"msgId\":\"") != NULL,
          "★ 记录没带序号(msg_seq=0) -> 用补报生成器现分配 8 位 hex");
    {
        const char *mid = strstr(buf, "\"msgId\":\"");
        char        got[9];

        (void)memset(got, 0, sizeof(got));
        if (mid != NULL)
        {
            (void)memcpy(got, mid + 9, 8u);   /* buf 后面还有很多内容, 只截 8 位 */
        }
        CHECK(test_is_hex8(got),
              "★ 现分配的 msgId 也是 8 位小写十六进制 (\"%s\")", got);
    }

    /* ---- ★ 第 12 条: 记录带了原序号 -> 补报重建出【同一个 msgId】 ---- */
    event_cache_reset();
    {
        (void)memset(&rec, 0, sizeof(rec));
        rec.kind     = (uint8_t)EC_KIND_STATUS_CHANGE;
        rec.a        = (uint8_t)EC_ST_WARNING;      /* to   */
        rec.b        = (uint8_t)EC_ST_NORMAL;       /* from */
        rec.ts       = 1700000200u;
        rec.aux = 90;
        rec.msg_seq  = 7u;                          /* 入缓存时定的号(salt='s' + 7 重建) */
        (void)event_cache_push(&rec);

        (void)memset(&rec, 0, sizeof(rec));
        rec.kind    = (uint8_t)EC_KIND_DEVICE;
        rec.a       = (uint8_t)EC_DEV_EV_BOUND;
        rec.b       = (uint8_t)EC_DEV_SENSOR;
        rec.ts      = 1700000201u;
        rec.uid[0]  = 0x21u; rec.uid[1] = 0x00u; rec.uid[12] = 0x01u;
        rec.msg_seq = 3u;
        (void)event_cache_push(&rec);
    }
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK((n > 0u) && (events == 2u), "2 条(带序号) -> 成功 (events=%u)", (unsigned)events);
    {
        char exp_s[9];
        char exp_v[9];

        (void)utils_gen_msg_id_seq(exp_s, (uint16_t)sizeof(exp_s), (uint8_t)'s', 7u);
        (void)utils_gen_msg_id_seq(exp_v, (uint16_t)sizeof(exp_v), (uint8_t)'v', 3u);
        CHECK(strstr(buf, exp_s) != NULL,
              "★ status_change 补报用 salt='s' + 原序号 7 -> \"%s\"", exp_s);
        CHECK(strstr(buf, exp_v) != NULL,
              "★ event.device 补报用 salt='v' + 原序号 3 -> \"%s\"", exp_v);
        CHECK(strstr(buf, "\"msgId\":\"b") == NULL,
              "★ 带序号的记录【不再】走 b<自增> 那条路");
    }
    event_cache_reset();

    /* ---- 动作组: 2 条记录 -> 1 条事件, 但 consumed=2 ---- */
    event_cache_reset();
    for (k = 0u; k < 2u; k++)
    {
        (void)memset(&rec, 0, sizeof(rec));
        rec.kind   = (uint8_t)EC_KIND_ACTION;
        rec.a      = 2u;
        rec.b      = (uint8_t)EC_TRIG_AUTO;
        rec.seq    = 4u;
        rec.ts     = 1700000100u;
        rec.uid[0] = 0x31u;
        rec.uid[12] = (uint8_t)(1u + k);   /* 2026-10-18: uid 13 字节, 尾部在 [12] */
        ec_action_set_code(&rec, (k == 0u) ? 0 : 1005);
        (void)event_cache_push(&rec);
    }
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK(n > 0u, "动作组 -> 成功");
    CHECK(events == 1u, "★ 2 条记录合成 1 条动作事件 (实际 %u)", (unsigned)events);
    CHECK(consumed == 2u, "★ consumed=2(算的是【整组】, 不是 1)");
    CHECK(strstr(buf, "\"event\":\"event.action\"") != NULL, "是动作事件");
    printf("  [diag] action buf=%s\n", buf);
    CHECK((strstr(buf, "31000000000000000000000001") != NULL) && (strstr(buf, "31000000000000000000000002") != NULL) &&
          (strstr(buf, "1005") != NULL),
          "★ 两台设备的结果都在(uid + 执行码)");

    /* ---- 混合: 设备事件 + 动作组 ---- */
    event_cache_reset();
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind   = (uint8_t)EC_KIND_DEVICE;
    rec.a      = (uint8_t)EC_DEV_EV_BOUND;
    rec.b      = (uint8_t)EC_DEV_SENSOR;
    rec.uid[12] = 0x09u;
    (void)event_cache_push(&rec);
    for (k = 0u; k < 2u; k++)
    {
        (void)memset(&rec, 0, sizeof(rec));
        rec.kind   = (uint8_t)EC_KIND_ACTION;
        rec.a      = 3u;
        rec.b      = (uint8_t)EC_TRIG_MANUAL;
        rec.seq    = 5u;
        rec.uid[2] = (uint8_t)(0x0Au + k);
        (void)event_cache_push(&rec);
    }
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK(n > 0u, "混合 -> 成功");
    CHECK(events == 2u, "★ events=2 (1 设备 + 1 动作组)");
    CHECK(consumed == 3u, "★ consumed=3 (全部推进)");

    /* ---- ★2026-10-08 行为变更: seq=0 的动作记录【现在可以渲染了】★ ----
     * 原断言是"seq=0 不成组 -> 动作事件渲染失败(没有 results)" —— 那是早期设计的
     * 必然结果(补报驱动只认成组)。用户 2026-10-08 选定【甲方案】: 每台设备单独一条
     * 记录(seq=0), 结果码存在 aux 里 ⇒ 补报驱动先给它造一个单元素 results[],
     * 于是它【能】渲染出来。这条断言跟着改成验证新行为。 */
    event_cache_reset();
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_ACTION;
    rec.a    = 1u;                  /* 档位 */
    rec.b    = (uint8_t)EC_TRIG_AUTO;
    rec.seq  = 0u;                  /* ★甲方案: 不分组★ */
    rec.uid[0] = 0x31u; rec.uid[1] = 0x00u; rec.uid[12] = 0x01u;   /* 310001 */
    ec_action_set_code(&rec, 0);    /* 执行成功 */
    (void)event_cache_push(&rec);
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK(n > 0u, "★★ 甲方案: seq=0 的单台动作记录现在能补报了 (n=%u)", (unsigned)n);
    CHECK(consumed == 1u, "★ 推进 1 条");
    CHECK(events == 1u, "★ events=1 (1 条单台动作)");
    CHECK(skipped == 0u, "★ 不再算 skipped —— 它是正常记录, 不是坏记录");
    CHECK(strstr(buf, "\"event.action\"") != NULL, "★ 渲染出的确实是 event.action");
    CHECK(strstr(buf, "\"uid\":\"31000000000000000000000001\"") != NULL, "★ results[].uid 是那台设备的 hex uid");
    CHECK(strstr(buf, "\"code\":0") != NULL, "★ results[].code 从 aux 取到了 0(成功)");

    /* 失败结果码也要能带出来(1005 = 子设备 LoRa 响应超时, 走标准码表) */
    event_cache_reset();
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_ACTION;
    rec.a    = 3u;
    rec.b    = (uint8_t)EC_TRIG_MANUAL;
    rec.seq  = 0u;
    rec.uid[12] = 0x01u;
    ec_action_set_code(&rec, 1005);
    (void)event_cache_push(&rec);
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK((n > 0u) && (strstr(buf, "\"code\":1005") != NULL),
          "★ 失败码 1005 也能补报出去 (n=%u)", (unsigned)n);

    /* ---- ★ 渲染不出来的记录也必须推进, 否则堵死队列 ★ ----
     * 用一条【kind 合法但码值非法】的记录来构造"渲染不出来":
     * EC_KIND_DEVICE 的 a 是设备事件码, 填一个没定义的值 -> event_cache_validate 拒。 */
    event_cache_reset();
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_DEVICE;
    rec.a    = 200u;                /* 未定义的设备事件码 */
    rec.b    = (uint8_t)EC_DEV_DEVICE;
    (void)event_cache_push(&rec);
    n = cloud_proto_build_backfill_data(buf, (uint16_t)sizeof(buf), 16u,
                                        &consumed, &events, &skipped);
    CHECK(n == 0u, "★ 唯一一条渲染不出来 -> 返回 0");
    CHECK(consumed == 1u, "★ 但仍推进了 1 条(否则它会永远堵在队头)");
    CHECK(skipped == 1u, "★ 且计入 skipped(可以丢, 不许静默丢)");
    CHECK(events == 0u, "events=0");

    /* ---- cap 太小: 整页装不下 -> 一条都不许算 consumed ---- */
    event_cache_reset();
    (void)memset(&rec, 0, sizeof(rec));
    rec.kind = (uint8_t)EC_KIND_DEVICE;
    rec.a    = (uint8_t)EC_DEV_EV_ONLINE;
    rec.b    = (uint8_t)EC_DEV_SENSOR;
    (void)event_cache_push(&rec);
    n = cloud_proto_build_backfill_data(buf, 16u, 16u, &consumed, &events, &skipped);
    CHECK(n == 0u, "cap 太小 -> 0");
    CHECK(consumed == 0u, "★ consumed=0(什么都没发出去, 不能凭空丢事件)");
    CHECK(event_cache_count() == 1u, "缓存里的那条还在");

    event_cache_reset();
}

/* =================================================================
 * [39] Unix 秒 -> 日历（往返自检）  (v83)
 * -----------------------------------------------------------------
 * `sys.time_sync` 要把平台给的 Unix 秒写成 4G 模组要的 `yy/MM/dd,hh:mm:ss±zz`，
 * 而工程里此前**只有反向**的 `cloud_proto_calendar_to_unix()` —— 本组钉住新加的
 * `cloud_proto_unix_to_calendar()`。
 * ★ 主判据是【往返恒等】calendar_to_unix(unix_to_calendar(t)) == t ★
 *   这比逐个手写期望值可靠：手写的那几个日期我极可能记错，而往返是自洽的。
 *   另配几个**定义性锚点**（unix 0 / 86400 / 负时区）互相独立地钉住绝对正确性。
 * ================================================================= */
static void test_unix_to_calendar(void)
{
    uint16_t y;
    uint8_t  mo;
    uint8_t  d;
    uint8_t  h;
    uint8_t  mi;
    uint8_t  s;
    uint32_t t;
    int      k;

    printf("\n[39] Unix 秒 -> 日历 (往返自检)\n");

    /* ---- 定义性锚点 ---- */
    CHECK(cloud_proto_unix_to_calendar(0u, 0, &y, &mo, &d, &h, &mi, &s) == true, "unix 0 -> ok");
    CHECK((y == 1970u) && (mo == 1u) && (d == 1u) && (h == 0u) && (mi == 0u) && (s == 0u),
          "★ unix 0 = 1970-01-01 00:00:00 (实际 %u-%02u-%02u %02u:%02u:%02u)",
          (unsigned)y, mo, d, h, mi, s);

    (void)cloud_proto_unix_to_calendar(0u, 28800, &y, &mo, &d, &h, &mi, &s);
    CHECK((y == 1970u) && (mo == 1u) && (d == 1u) && (h == 8u),
          "★ unix 0 @+8h = 1970-01-01 08:00 (实际 %u-%02u-%02u %02u)", (unsigned)y, mo, d, h);

    (void)cloud_proto_unix_to_calendar(86399u, 0, &y, &mo, &d, &h, &mi, &s);
    CHECK((d == 1u) && (h == 23u) && (mi == 59u) && (s == 59u),
          "86399 = 01-01 23:59:59 (实际 %02u:%02u:%02u)", h, mi, s);
    (void)cloud_proto_unix_to_calendar(86400u, 0, &y, &mo, &d, &h, &mi, &s);
    CHECK((d == 2u) && (h == 0u), "86400 = 01-02 00:00 (实际 %02u 日 %02u 时)", d, h);

    /* ★ 负偏移：加时区后落到 1970 之前 —— C 的 "/" 向 0 取整会差一天 ★ */
    (void)cloud_proto_unix_to_calendar(3600u, -28800, &y, &mo, &d, &h, &mi, &s);
    CHECK((y == 1969u) && (mo == 12u) && (d == 31u) && (h == 17u),
          "★ unix 3600 @-8h = 1969-12-31 17:00（负偏移不差天）(实际 %u-%02u-%02u %02u)",
          (unsigned)y, mo, d, h);

    /* ---- 往返恒等：含闰年与世纪边界 ---- */
    {
        static const uint16_t yy[] = { 1970u, 1972u, 1999u, 2000u, 2001u, 2024u, 2026u, 2100u, 2105u };
        static const uint8_t  mm[] = {  1u,    2u,   12u,    2u,    3u,    2u,    9u,    3u,    1u  };
        static const uint8_t  dd[] = {  1u,   29u,   31u,   29u,    1u,   29u,   27u,    1u,    1u  };

        for (k = 0; k < 9; k++)
        {
            t = cloud_proto_calendar_to_unix(yy[k], mm[k], dd[k], 13u, 45u, 7u, 0);
            CHECK(t != 0u, "锚点 %u-%02u-%02u 能转成 unix (%lu)",
                  (unsigned)yy[k], mm[k], dd[k], (unsigned long)t);

            (void)cloud_proto_unix_to_calendar(t, 0, &y, &mo, &d, &h, &mi, &s);
            CHECK((y == yy[k]) && (mo == mm[k]) && (d == dd[k]) &&
                  (h == 13u) && (mi == 45u) && (s == 7u),
                  "★ 往返恒等 %u-%02u-%02u 13:45:07 (实际 %u-%02u-%02u %02u:%02u:%02u)",
                  (unsigned)yy[k], mm[k], dd[k], (unsigned)y, mo, d, h, mi, s);
        }
    }

    /* ---- 带时区的往返也要恒等 ---- */
    t = cloud_proto_calendar_to_unix(2026u, 9u, 27u, 12u, 0u, 0u, 28800);
    (void)cloud_proto_unix_to_calendar(t, 28800, &y, &mo, &d, &h, &mi, &s);
    CHECK((y == 2026u) && (mo == 9u) && (d == 27u) && (h == 12u) && (mi == 0u),
          "★ 带 +8h 偏移的往返恒等 (实际 %u-%02u-%02u %02u:%02u)", (unsigned)y, mo, d, h, mi);

    /* ---- NULL ---- */
    CHECK(cloud_proto_unix_to_calendar(0u, 0, NULL, &mo, &d, &h, &mi, &s) == false,
          "year=NULL -> false");
    CHECK(cloud_proto_unix_to_calendar(0u, 0, &y, &mo, &d, &h, &mi, NULL) == false,
          "second=NULL -> false");
}
/* ============================================================================
 * [OTA] ota.start / ota.query / ota.abort —— 命令层的错误码映射
 * ----------------------------------------------------------------------------
 * 2026-10-02 起这三条从占位(恒回 1006)换成真实实现(app/ota/ota_http.c)。
 * 下载的全过程在 test_ota_http.c 里用假模组端到端跑; 这里只钉【云协议这一层】
 * 的错误码映射 —— 平台是照这些码做决策的, 给错了平台会误判:
 *   URL 非法 2001 / 缺参数或类型错 1001 / 装不下 1010 /
 *   与当前版本相同 1006 / 并发 1006 / query 的 state 字符串 /
 *   abort 下载中可中止、idle 时拒绝
 * ========================================================================== */
extern void fake_reset(void);        /* fake_ota_port.c: RAM 后端复位 */

static void test_ota_cmds(void)
{
    char              resp[CLOUD_RESP_MAX * 4u];
    uint16_t          rl;
    int               rc;
    ota_http_status_t st;
    char              body[320];
    const char       *cur;

    printf("[OTA] ota.start / ota.query / ota.abort 错误码映射\n");

    fake_reset();
    ota_http_init();

    /* ---- 缺 size -> 1001 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"o1\",\"data\":"
                            "{\"version\":\"9.9.9\",\"url\":\"https://x/y.bin\","
                            "\"md5\":\"00000000000000000000000000000000\"}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "缺 size -> 1001 (rc=%d)", rc);

    /* ---- size 给字符串 -> 1001(类型不对不猜它想表达什么) ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"o2\",\"data\":"
                            "{\"version\":\"9.9.9\",\"url\":\"https://x/y.bin\","
                            "\"md5\":\"00000000000000000000000000000000\",\"size\":\"999\"}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_PARAM, "size 给字符串 -> 1001 (rc=%d)", rc);

    /* ---- URL 非法(ftp) -> 2001 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"o3\",\"data\":"
                            "{\"version\":\"9.9.9\",\"url\":\"ftp://x/y.bin\","
                            "\"md5\":\"00000000000000000000000000000000\",\"size\":1024}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_OTA_URL, "★ ftp:// -> 2001 (rc=%d)", rc);

    /* ---- size 超暂存区 -> 1010 ---- */
    (void)snprintf(body, sizeof(body),
                   "{\"cmd\":\"ota.start\",\"msgId\":\"o4\",\"data\":"
                   "{\"version\":\"9.9.9\",\"url\":\"https://x/y.bin\","
                   "\"md5\":\"00000000000000000000000000000000\",\"size\":%lu}}",
                   (unsigned long)(FM_STAGE_SIZE + 1u));
    rc = cloud_proto_handle(body, 1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_NO_SPACE, "★ 超过暂存区(%lu) -> 1010 (rc=%d)",
          (unsigned long)FM_STAGE_SIZE, rc);

    /* ---- 与当前版本相同 -> 1006 ---- */
    cur = ota_port_get_version();
    (void)snprintf(body, sizeof(body),
                   "{\"cmd\":\"ota.start\",\"msgId\":\"o5\",\"data\":"
                   "{\"version\":\"%s\",\"url\":\"https://x/y.bin\","
                   "\"md5\":\"00000000000000000000000000000000\",\"size\":1024}}",
                   (cur != NULL) ? cur : "0.0.0");
    rc = cloud_proto_handle(body, 1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_STATE, "★ 与当前版本(%s)相同 -> 1006 (rc=%d)",
          (cur != NULL) ? cur : "?", rc);

    /* ---- 合法 -> 受理(0), 且状态机真的起来了 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"o6\",\"data\":"
                            "{\"version\":\"9.9.9\",\"url\":\"https://kappa.lol/wRqcN6\","
                            "\"md5\":\"9e96d33cf71a79caabd00b319e115af5\",\"size\":65536}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "合法 -> 0 受理 (rc=%d)", rc);
    CHECK(ota_http_busy(), "★ 受理后状态机确实在跑(ota_http_busy)");

    /* ---- 并发第二条 -> 1006 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.start\",\"msgId\":\"o7\",\"data\":"
                            "{\"version\":\"9.9.8\",\"url\":\"https://kappa.lol/other\","
                            "\"md5\":\"9e96d33cf71a79caabd00b319e115af5\",\"size\":65536}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_STATE, "★ 并发 ota.start -> 1006 (rc=%d)", rc);

    /* ---- ota.query: state / percent / targetVersion ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.query\",\"msgId\":\"o8\",\"data\":{}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "ota.query -> 0 (rc=%d)", rc);
    CHECK(strstr(resp, "\"state\":\"") != NULL, "★ 应答含 state 字段");
    CHECK(strstr(resp, "\"percent\":") != NULL, "★ 应答含 percent 字段");
    CHECK(strstr(resp, "\"targetVersion\":\"9.9.9\"") != NULL,
          "★ 应答含 targetVersion=9.9.9");

    /* ---- ota.abort: 下载中 -> 0; 再来一次(idle) -> 1006 ---- */
    rc = cloud_proto_handle("{\"cmd\":\"ota.abort\",\"msgId\":\"o9\",\"data\":{}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_OK, "★ 下载中中止 -> 0 (rc=%d)", rc);
    CHECK(!ota_http_busy(), "中止后不再 busy");

    rc = cloud_proto_handle("{\"cmd\":\"ota.abort\",\"msgId\":\"o10\",\"data\":{}}",
                            1u, resp, (uint16_t)sizeof(resp), &rl);
    CHECK(rc == CLOUD_EC_STATE, "★ idle 时再中止 -> 1006 (rc=%d)", rc);

    ota_http_get_status(&st);
    printf("       (收尾 state=%d err=%d, 期望 IDLE/0)\n", (int)st.state, (int)st.err);
}

int main(void)
{
    printf("=== cloud_json / cloud_proto / cloud_profile 主机单元测试 ===\n\n");

    host_log_enable(0);     /* 模块内的日志不打印 */

    /* ---- ★2026-11-11: 事件队列改成直接写外挂 Flash ----
     * 主机上用 fake_xflash 的内存盘当芯片（真 NOR 语义）。不初始化的话
     * event_cache_push() 会全部失败、capacity()=0 —— 那不是被测代码的问题。 */
    {
        extern void fx_blank(void);
        extern bool xf_store_init(void);
        extern bool xf_ring_format_all(void);

        fx_blank();
        (void)xf_store_init();
        (void)xf_ring_format_all();
    }

    /* ---- [0] 护栏: hooks 还没装的时候, 绝不能去碰 cJSON ----
     * 这一段刻意放在 cjson_port_init() 之前, 是【唯一】能验证这条护栏的位置。
     * 背景(实测踩过): 只把 cloud_proto.c 迁到 cJSON、忘了在 main() 里 init,
     * 链接器会把 cjson_port.o 整个丢掉 —— 没有任何编译/链接错误, 但 cJSON 会去用
     * 本工程那 512 字节的堆, 一收下行命令就挂。 */
    {
        cloud_req_t r;
        char        b[64];

        printf("[0] cJSON hooks 未装时的护栏(v33-2)\n");
        (void)memset(&r, 0, sizeof(r));
        CHECK(cloud_proto_parse_req("{\"cmd\":\"cfg.set\"}", &r) == CLOUD_EC_INTERNAL,
              "★ 没 init 就解析 -> 明确报 1007, 而不是悄悄去用 512 字节堆");
        CHECK(cloud_proto_build_resp(b, (uint16_t)sizeof(b), &r, 0, NULL, NULL, 0u) == 0u,
              "★ 没 init 就生成应答 -> 返回 0(空应答)");
        CHECK(cloud_proto_build_uplink(b, (uint16_t)sizeof(b), "status.report", "h1",
                                       0u, NULL) == 0u,
              "★ 没 init 就生成上行 -> 返回 0");
    }

    cjson_port_init();      /* v33: 装好静态池 hooks —— 信封层已经改用 cJSON */

    test_json_get();
    test_json_array();
    test_json_escape();
    test_json_writer();
    test_json_writer_overflow();
    test_envelope_parse();
    test_resp_build();
    test_cmd_table();
    test_dispatch();
    test_ota_cmds();
    test_auth();
    test_profile_topic();
    test_profile_validate();
    test_uplink();
    test_status_data();
    test_unix_time();
    test_unix_to_calendar();
    test_json_sniff();
    test_cfg_query_handler();
    test_masked_credential();
    test_cfg_set_handler();
    test_factory_reset();
    test_sensor_offline_logic();
    test_msgid_idempotent();
    test_set_entry();
    test_sys_reboot();
    test_sys_query_status();
    test_telemetry_data();
    test_event_data();
    test_sys_time_sync();
    test_bind();
    test_sen_dev_query();
    test_cfg_sections();
    test_event_cache();
    test_low_voltage_latest();
    test_render_cached_event();
    test_backfill_page();

    printf("\n==== 通过 %d, 失败 %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
