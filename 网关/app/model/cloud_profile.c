/**
 * @file  cloud_profile.c
 * @brief 云平台连接档位实现(见 cloud_profile.h 的说明)
 */
#include "cloud_profile.h"
#include "param_save.h"
#include <string.h>

/* ==================================================================
 * 常量表
 * ================================================================== */

/* ★2026-11-12 任务三: 管理面前缀固定, 不受 p->topic_prefix / 档位影响。
 * 业务面仍走 p->topic_prefix(默认 "rt/gw")。 */
#define CLOUD_MGMT_PREFIX   "rt/mgmt"

/* 0 = 业务面, 1 = 管理面。与下面四张表【一一对应】。
 * ★加枚举项时必须同步补 domain / suffix / local / qos / retained 五张表★
 *   C 对"初始化器比数组短"不报错(会悄悄补 0), 所以用下面这条断言把
 *   CLOUD_TOPIC_COUNT 钉死 —— 以后加 topic 忘了补表, 这里直接编译失败。 */
typedef char cloud_topic_tables_complete[(CLOUD_TOPIC_COUNT == 10) ? 1 : -1];

static const uint8_t s_topic_domain[CLOUD_TOPIC_COUNT] =
{
    0u, 0u, 0u, 0u, 0u, 0u,       /* 业务面 6 条: rt/gw/{gwId}/...  */
    1u, 1u, 1u, 1u                /* 管理面 4 条: rt/mgmt/{gwId}/... */
};

/* 日志/#topic 里显示的短名。管理面加 "mgmt/" 前缀, 否则两个 "cmd"
 * (业务面 cmd 与管理面 cmd)在诊断输出里长得一模一样, 分不清谁是谁。 */
static const char *const s_mgmt_topic_name[4] =
{
    "mgmt/cmd", "mgmt/cmdresp", "mgmt/checkin", "mgmt/ota.progress"
};

/* topic 后缀(业务面拼成 {prefix}/{gwId}/<后缀>, 管理面拼成 rt/mgmt/{gwId}/<后缀>) */
static const char *const s_topic_suffix[CLOUD_TOPIC_COUNT] =
{
    "cmd",          /* CLOUD_TOPIC_CMD       */
    "cmdresp",      /* CLOUD_TOPIC_CMDRESP   */
    "telemetry",    /* CLOUD_TOPIC_TELEMETRY */
    "event",        /* CLOUD_TOPIC_EVENT     */
    "status",       /* CLOUD_TOPIC_STATUS    */
    "lwt",          /* ★2026-10-08 新增★ 遗嘱专用, 与 status 隔离(理由见 .h) */
    "cmd",          /* CLOUD_TOPIC_MGMT_CMD          ★2026-11-12 任务三★ */
    "cmdresp",      /* CLOUD_TOPIC_MGMT_CMDRESP      (后缀与业务面相同, 靠前缀分面) */
    "checkin",      /* CLOUD_TOPIC_MGMT_CHECKIN */
    "ota.progress"  /* CLOUD_TOPIC_MGMT_OTA_PROGRESS (最长: 8+26+13 = 47 字符) */
};

/* 本地/历史档位的 topic 表。
 * ★2026-10-08 起【不再是出厂默认】(默认已切到文档档位 rt/gw/{gwId}/xxx),
 *   保留它只为了"万一要回退"以及主机单测覆盖两套拼装逻辑。
 * 注: 早期这里的下行叫 "mod/pc_down"、四个上行**全塌成** "mod/mod_upload";
 *     v93 已按平台新建的 5 个主题换成下面这组; 那两个遗留主题在 2026-10-08
 *     已按甲方要求【整体删除】(连编译期宏一起), 不要再引用它们。 */
static const char *const s_local_topic[CLOUD_TOPIC_COUNT] =
{
    "mod/cmd",          /* 下行(平台 -> 网关) */
    "mod/cmdresp",      /* 上行(命令应答)     */
    "mod/telemetry",    /* 上行(水位遥测)     */
    "mod/event",        /* 上行(事件)         */
    "mod/status",       /* 上行(状态/心跳)    */
    "mod/lwt",          /* ★2026-10-08 新增★ 遗嘱专用 */
    "", "", "", ""      /* 管理面 4 条: 本地档位【不启用】, build_topic 直接返回 0
                         * (这几行只是占位, 不会被读到) */
};

/* 文档 V0.2 起: 下行命令/应答/遥测/事件/状态全部 QoS1(遥测由 0 改为 1, 弱网丢包太多)。
 * ★lwt 用 QoS1★: 遗嘱只在掉电/断链那一刻发一条, 是最不能丢的消息,
 *   QoS0 的话"发出去就不管了" —— broker 没收到就永久丢了, 平台再也等不到掉线通知。 */
static const uint8_t s_doc_qos[CLOUD_TOPIC_COUNT] =
{
    1u, 1u, 1u, 1u, 1u, 1u,    /* 业务面 6 条 */
    1u, 1u, 1u, 1u             /* ★2026-11-12 管理面 4 条: 规范表格全 QoS1 */
};

/* ★★2026-11-04(1103 联调 §六 已拍板): 全线弃用 retained, 一律 retained=0★★
 *
 * 【改了什么】原来只有 status / lwt 两个 topic 是 retained, 现在六个全部 false。
 *   仍保留这张表(而不是把调用点改成常量 false): #topic 诊断、g4_at 的逐条发布
 *   都还走 cloud_profile_topic_retained(), 留一个统一口径比散落常量好改。
 *
 * 【为什么弃用】(详见 1103 联调记录 §六, 结论是"无不可替代场景")
 *   1. 平台 7×24 常驻订阅且状态落库, "订阅即得快照"的唯一受益窗口是平台重启后的
 *      一个心跳周期, 由 DB 兜底即可; 在线判定本就以最新消息为准, 不信 retained 快照;
 *   2. retained 会把固件偶发故障【持久化】: N9/C3 事故里被 AT 调试日志污染的、
 *      JSON 截断的脏报文被 broker 长期驻留, 每个新订阅者必读, 平台被迫加容错;
 *      非 retained 报文一闪而过, 影响不驻留;
 *   3. 命令类主题本就不能 retained —— msgId 幂等只记最近 4 条且重启易失,
 *      网关重启重订阅会重复执行旧命令;
 *   4. 设备退役/换 gwId/改主题后, retained 要逐主题发空 payload 清理, 且对 broker
 *      有额外存储与投递开销。
 *
 * 【代价(已与平台确认)】lwt 不再 retained 后, "掉线通知"只有【当时在线订阅】的
 *   订阅者能看到; 平台重启期间错过的掉线, 由超时窗口 + DB 最后状态兜底。
 *   ⚠️ 存量 broker 上已有的 retained 脏报文【不会】因为本改动消失, 需要运维在
 *      切版本时对 rt/gw/{gwId}/status、rt/gw/{gwId}/lwt 各发一条空 payload
 *      retained 清一次(固件不做, 否则等于自己又发了一条 retained)。 */
static const bool s_doc_retained[CLOUD_TOPIC_COUNT] =
{
    false, false, false, false, false, false,   /* 业务面 6 条 */
    false, false, false, false                  /* ★2026-11-12 管理面 4 条: 同样不 retained */
};

/* ==================================================================
 * 内部状态(懒加载)
 * ================================================================== */

static cloud_profile_t s_profile;
static bool            s_loaded = false;

/* 字段是否是可打印 ASCII 且以 '\0' 结尾 */
static bool str_is_printable(const char *s, uint16_t cap)
{
    uint16_t i;

    for (i = 0u; i < cap; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == 0u)
        {
            return true;
        }
        if ((c < 0x20u) || (c > 0x7Eu))
        {
            return false;
        }
    }
    return false;                       /* 字段内找不到 '\0' */
}

void cloud_profile_set_factory_default(cloud_profile_t *p)
{
    if (p == NULL)
    {
        return;
    }
    (void)memset(p, 0, sizeof(*p));

    /* ★2026-10-08 出厂默认 = 文档档位; 2026-10-28 按 13 字节 UID 口径复核★
     * 甲方平台(223.75.207.139)约定:
     *   · 主题形如 rt/gw/{gwId}/cmd ... —— 就是文档定义的这套 {prefix}/{gwId}/{suffix};
     *   · {gwId} = 设备 ID = 26 位 hex("11" + 芯片 96 位 UID), 由配置注入, 云端可改;
     *   · ★MQTT 用户名【不是】gwId★: 出厂是 "tuguan", 由 network.username 决定,
     *     平台可在云端配置里改(2026-10-28 用户确认)。clientID 才等于 gwId。
     *
     * ⚠️ gw_id 的【真正真相源是 config_store.system.gwId】,
     *    main.c 启动时会把配置里的值灌进来(cloud_profile_set_gw_id) ——
     *    这里只是"配置还没加载时的兜底"。
     *    主题与 clientID 都用这个值: 若两处不一致, 会连得上但收不到指令。
     * ⚠️ 2026-10-28 更正: 原注释写"出厂 "110001"、且这个值同时是 MQTT 用户名",
     *    三处都不对(3 字节地址已废弃、用户名是 tuguan)。 */
    p->mode           = (uint8_t)CLOUD_MODE_DOC;
    p->id_map_enable  = 0u;
    p->auth_enable    = 0u;

    /* ⚠️ host/port 这两个字段【当前全工程没有消费者】(见偏差记录 D4):
     *    真正连哪个 broker 由 `config_store.network.host`(非空时) 或 g4_at 的编译期
     *    默认值决定。这里填成实际值有两个作用:
     *      ① 让 cloud_profile_validate() 通过(它要求 host 非空);
     *      ② 万一以后有人把它接上, 取到的也是正确的地址, 不会指向一个废弃的 broker。 */
    (void)memcpy(p->host, "223.75.207.139", sizeof("223.75.207.139"));
    p->port = 1883u;

    /* ★2026-10-08: 空 = 【不参与 clientID 决策】★
     * clientID 由 g4_at 管, 取【cloud_profile 的 gwId】(= 设备 ID, 云端可改),
     * 特殊值 "IMEI" = 用模组 IMEI。本字段当前【没有消费者】, 留空以免出现
     * "两个地方都能配 clientID、谁生效说不清"。
     * ⚠️ 2026-10-28 更正: 原写"编译期默认 110001" —— 那是 3 字节地址时代的固定值,
     *    现在是 gwId(26 位 hex), 且可云端修改。 */
    p->client_id[0] = '\0';
    (void)memcpy(p->gw_id, "11000000000000000000000000", sizeof("11000000000000000000000000"));   /* 2026-10-18: 26 位; 真实值由 config_store 注入 */
    (void)memcpy(p->topic_prefix, "rt/gw", sizeof("rt/gw"));
    p->auth[0] = '\0';
}

int cloud_profile_validate(const cloud_profile_t *p)
{
    if (p == NULL)
    {
        return 1;
    }
    if ((p->mode != (uint8_t)CLOUD_MODE_LOCAL) && (p->mode != (uint8_t)CLOUD_MODE_DOC))
    {
        return 1;
    }
    if ((p->host[0] == '\0') || (!str_is_printable(p->host, CLOUD_HOST_MAX)))
    {
        return 2;
    }
    if (p->port == 0u)
    {
        return 3;
    }
    /* gwId 会被用作 MQTT clientId 与 topic 段: 非空、<=26 字节、不含 topic 通配符
     * ⚠️ 2026-10-28 更正: 原注释写 "<=20", 而下一行实际判的是 `> 26`
     *    (CLOUD_GWID_MAX = 27)。现行 gwId 是 26 位 hex, 20 字节根本装不下。 */
    if ((p->gw_id[0] == '\0') || (!str_is_printable(p->gw_id, CLOUD_GWID_MAX)))
    {
        return 4;
    }
    if ((strlen(p->gw_id) > 26u) || (strpbrk(p->gw_id, "/+#") != NULL))
    {
        return 4;
    }
    if ((p->topic_prefix[0] == '\0') || (!str_is_printable(p->topic_prefix, CLOUD_PREFIX_MAX)))
    {
        return 5;
    }
    if (strpbrk(p->topic_prefix, "+#") != NULL)
    {
        return 5;
    }
    if (!str_is_printable(p->auth, CLOUD_AUTH_MAX))
    {
        return 6;
    }
    return 0;
}

static void cloud_profile_ensure_loaded(void)
{
    device_param_t *dp;

    if (s_loaded)
    {
        return;
    }
    s_loaded = true;

    dp = param_get_ptr();
    if (dp != NULL)
    {
        if (cloud_profile_validate(&dp->cloud) == 0)
        {
            s_profile = dp->cloud;
            return;
        }
    }
    cloud_profile_set_factory_default(&s_profile);
}

const cloud_profile_t *cloud_profile_get(void)
{
    cloud_profile_ensure_loaded();
    return &s_profile;
}

int cloud_profile_set(const cloud_profile_t *p)
{
    device_param_t *dp;
    int             rc;

    if (p == NULL)
    {
        return 1;
    }
    rc = cloud_profile_validate(p);
    if (rc != 0)
    {
        return rc;
    }

    s_profile = *p;
    s_loaded  = true;

    dp = param_get_ptr();
    if (dp != NULL)
    {
        dp->cloud = *p;
        (void)param_save_store();       /* 落 Flash */
    }
    return 0;
}

void cloud_profile_reset(void)
{
    cloud_profile_set_factory_default(&s_profile);
    s_loaded = true;
}

/**
 * @brief ★2026-10-08 新增★ 把 Flash 里的"遗留 LOCAL 档"一次性迁移到 DOC 档
 *
 * 【为什么必须做这件事 —— 现场真的踩了】
 *   `cloud_profile` 存在 `param_save` 的 Flash 页里(不在程序区!), 而：
 *     · `param_load_from_flash()` 只要那页 CRC 是好的, 就【用存下来的】参数;
 *     · `cloud_profile_ensure_loaded()` 只要存下来的那份 validate 通过, 就【用它】,
 *       出厂默认只是"没存过/坏了"时的兜底。
 *   ⇒ 旧固件(出厂默认 = LOCAL 档)跑过一次就会把 LOCAL 写进 Flash;
 *     **之后无论怎么重烧 App 都带不走它** —— 网关仍然订阅 mod/cmd,
 *     而甲方平台往 rt/gw/{gwId}/cmd 发, 结果就是"心跳看得到(发到 mod/status)、
 *     指令一条都收不到(订的是 mod/cmd)"。现场表现极具迷惑性。
 *   而且 `cloud_profile_reset()` 在此之前【没有任何调用者】, sys.factory_reset
 *   也清不掉它 —— 现场没有办法恢复。
 *
 * 【为什么是"迁移"而不是每次开机强制】
 *   只在"读到的不是 DOC 档"时改一次并落盘, 之后就不再干预 ——
 *   万一将来要保留 LOCAL 档做台架, 也有地方下手。
 *
 * @return true = 刚刚做了迁移(调用方应该打一条显眼的日志); false = 本来就是 DOC 档
 */
bool cloud_profile_migrate_to_doc(void)
{
    device_param_t *dp;

    if (cloud_profile_is_doc())
    {
        return false;                       /* 已经是文档档位, 什么都不做 */
    }

    s_profile        = *cloud_profile_get();    /* 先确保已加载(保留 gw_id / prefix / auth) */
    s_profile.mode   = (uint8_t)CLOUD_MODE_DOC;
    s_loaded         = true;

    dp = param_get_ptr();
    if (dp != NULL)
    {
        dp->cloud = s_profile;
        (void)param_save_store();               /* 落盘: 下次开机不再需要迁移 */
    }
    return true;
}

int cloud_profile_set_ram(const cloud_profile_t *p)
{
    int rc;

    if (p == NULL)
    {
        return 1;
    }
    rc = cloud_profile_validate(p);
    if (rc != 0)
    {
        return rc;
    }
    s_profile = *p;
    s_loaded  = true;
    return 0;                           /* 刻意不写 Flash */
}

bool cloud_profile_is_doc(void)
{
    return (cloud_profile_get()->mode == (uint8_t)CLOUD_MODE_DOC);
}

/**
 * @brief ★2026-10-08 新增: 用配置里的"设备 ID"(system.gwId) 覆盖档位的 gwId
 *
 * 【为什么需要】topic 里的 {gwId} 与 MQTT clientID 必须是【同一个值】,
 * 而这个值的真相源是 `config_store.system.gwId`(出厂 = "11" + 芯片 96 位 UID,
 * 26 位 hex; 可用 cfg.set 在云端改)。
 * ⚠️ MQTT【用户名】是另一回事: 出厂 "tuguan", 由 network.username 决定(2026-10-28 确认)。
 * 本模块不依赖 config_store(要保持能在主机上脱机单测), 所以由 main.c 启动时注入,
 * 与既有的 g4_at_set_sub_topic() / g4_at_set_broker() 是同一个套路。
 *
 * @note 只改 RAM, 【不写 Flash】—— 持久化由 config_store 负责, 这里写盘会造成
 *       两个真相源(下次启动谁覆盖谁就不确定了)。
 * @note 只在【下一次 MQTT 建链】时生效(topic 是建链前拼好的)。
 */
void cloud_profile_set_gw_id(const char *gw_id)
{
    if ((gw_id == NULL) || (gw_id[0] == '\0'))
    {
        return;                     /* 空 = 不改, 保留兜底值 */
    }
    s_profile = *cloud_profile_get();       /* 先确保已加载 */
    strncpy(s_profile.gw_id, gw_id, CLOUD_GWID_MAX - 1u);
    s_profile.gw_id[CLOUD_GWID_MAX - 1u] = '\0';
}

bool cloud_profile_auth_required(void)
{
    const cloud_profile_t *p = cloud_profile_get();
    return (p->auth_enable != 0u) && (p->auth[0] != '\0');
}

/* ==================================================================
 * Topic 拼装
 * ================================================================== */

static bool app_str(char *out, uint16_t cap, uint16_t *n, const char *s)
{
    uint16_t l = (uint16_t)strlen(s);

    if ((uint16_t)(*n + l) >= cap)
    {
        return false;
    }
    (void)memcpy(out + *n, s, l);
    *n = (uint16_t)(*n + l);
    out[*n] = '\0';
    return true;
}

uint16_t cloud_profile_build_topic(cloud_topic_t t, char *out, uint16_t cap)
{
    const cloud_profile_t *p;
    uint16_t               n = 0u;

    if ((out == NULL) || (cap == 0u) || (t >= CLOUD_TOPIC_COUNT))
    {
        return 0u;
    }
    out[0] = '\0';
    p = cloud_profile_get();

    if (p->mode == (uint8_t)CLOUD_MODE_LOCAL)
    {
        const char *s;
        uint16_t    l;

        /* ★2026-11-12 任务三: 本地/历史档位没有管理面。
         * 返回 0 表示【不可用】(不是"拼出空串"), 调用方一律判 0 跳过 ——
         * 绝不悄悄回落到业务面 topic, 否则管理命令会混进业务通道。 */
        if (s_topic_domain[t] != 0u) { return 0u; }

        s = s_local_topic[t];
        l = (uint16_t)strlen(s);
        if (l >= cap)
        {
            return 0u;
        }
        (void)memcpy(out, s, (size_t)l + 1u);
        return l;
    }

    /* 文档模式: {prefix}/{gwId}/{suffix}
     * ★管理面固定用 rt/mgmt, 忽略 p->topic_prefix(改业务面前缀不应影响管理面)★ */
    {
        const char *prefix = (s_topic_domain[t] != 0u)
                           ? CLOUD_MGMT_PREFIX : p->topic_prefix;
        if (!app_str(out, cap, &n, prefix))
        {
            out[0] = '\0';
            return 0u;
        }
    }
    if ((!app_str(out, cap, &n, "/")) ||
        (!app_str(out, cap, &n, p->gw_id)) ||
        (!app_str(out, cap, &n, "/")) ||
        (!app_str(out, cap, &n, s_topic_suffix[t])))
    {
        out[0] = '\0';
        return 0u;
    }
    return n;
}

uint8_t cloud_profile_topic_qos(cloud_topic_t t)
{
    if (t >= CLOUD_TOPIC_COUNT)
    {
        return 0u;
    }
    if (cloud_profile_get()->mode == (uint8_t)CLOUD_MODE_LOCAL)
    {
        return 1u;                      /* 现网全 QoS1, 保持不变 */
    }
    return s_doc_qos[t];
}

bool cloud_profile_topic_retained(cloud_topic_t t)
{
    if (t >= CLOUD_TOPIC_COUNT)
    {
        return false;
    }
    if (cloud_profile_get()->mode == (uint8_t)CLOUD_MODE_LOCAL)
    {
        /* ★★2026-11-04(1103 联调 §六): 现场档位也一律 retained=0★★
         *   早先(v94, 2026-09-27)为"平台一订阅就拿到最后状态"把 status 也发成
         *   retained; 现在按拍板结论取消 —— 理由与 s_doc_retained 上方注释相同:
         *   平台常驻订阅 + 落库, retained 只带来脏数据驻留与清理负担。
         *   两个分支(现场/文档)现在语义一致: 任何 topic 都不 retained。 */
        return false;
    }
    return s_doc_retained[t];
}

const char *cloud_profile_topic_name(cloud_topic_t t)
{
    if (t >= CLOUD_TOPIC_COUNT)
    {
        return "?";
    }
    /* 管理面返回 "mgmt/xxx" 而不是光秃秃的 "cmd"/"cmdresp" —— #topic 诊断里
     * 两条 cmd 必须能一眼分开(完整 topic 那一列本来就带前缀, 这里不重复)。 */
    if (s_topic_domain[t] != 0u)
    {
        return s_mgmt_topic_name[(uint8_t)t - CLOUD_TOPIC_BIZ_COUNT];
    }
    return s_topic_suffix[t];
}

bool cloud_profile_is_mgmt_topic(cloud_topic_t t)
{
    if (t >= CLOUD_TOPIC_COUNT)
    {
        return false;
    }
    return (s_topic_domain[t] != 0u);
}

bool cloud_profile_check_auth(const char *auth_from_msg)
{
    const cloud_profile_t *p = cloud_profile_get();

    if (!cloud_profile_auth_required())
    {
        return true;                    /* 未启用 -> 放行 */
    }
    if (auth_from_msg == NULL)
    {
        return false;
    }
    return (strcmp(p->auth, auth_from_msg) == 0);
}
