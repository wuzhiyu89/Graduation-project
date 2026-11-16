/**
 * @file  cloud_cmd_devices.c
 * @brief 传感器类命令（文档 5.3）
 *
 *   sen.bind / sen.unbind / sen.set / sen.query   —— 液位传感器
 *
 * 毕设版只有液位传感器；dev.* 联动设备命令与 dev.ctrl 已随一体机删除。
 */
#include <string.h>

#include "cloud_proto.h"
#include "cloud_internal.h"
#include "cloud_json.h"
#include "config_store.h"
#include "last_value.h"
#include "sensor_offline.h"
#include "bsp_tim.h"   /* get_tick_ms */
#include "log.h"
#include "lora_business.h"

/**
 * @brief sen.bind / dev.bind 的配对窗口上限(秒)
 *   2026-10-28 定案: 按《网关使用手册》§4.3 收敛到 1~300。
 *   此前只判 !=0 && <=65535, 平台发 3600 也会被接受, 与手册口径不符。
 */
#define CLOUD_PAIR_WINDOW_MAX   300u

/**
 * @brief sen.unbind / dev.unbind —— 文档 5.3 / 5.4
 *   参数: uid(本工程口径; 文档写的是 addr, 按用户决策一律映射成 uid)
 *   响应: code 0(没有 data 字段)
 *
 * 【P5 改为异步(2026-09-29)】:
 *   不再立刻改 config_store —— 先调 lora_business_unbind() 向设备发 0x05 解绑命令;
 *   网关在收到设备回的 0x80 ACK(或重试耗尽)时才真正删除 config_store + 从机表,
 *   并通过事件回调触发 event.device{unbound} 上报云端。本函数只负责发起解绑请求,
 *   立即返回 code 0(异步), 让云端靠"unbound 事件"确认完成, 而不是靠命令响应。
 *
 *   uid 没绑过 -> 1003(目标设备不存在); 已有解绑在进行 -> 1006(状态不允许)。
 *
 *   is_sensor 在异步化后已不需要(lora_business 内部按从机表 dev_type 自查),
 *   保留参数是为了让 sen.unbind / dev.unbind 两个分发表项共用本函数。
 */
static int cloud_cmd_unbind(const cloud_req_t *req, cJSON *data, bool is_sensor)
{
    char uid[CFG_UID_STR_MAX];

    (void)data;                             /* 文档这条响应没有 data */
    (void)is_sensor;                        /* 异步化后 lora_business 自查 dev_type */

    if (req->data == NULL)
    {
        return CLOUD_EC_PARAM;
    }
    if (!cj_get_str(req->data, "uid", uid, (uint16_t)sizeof(uid)))
    {
        return CLOUD_EC_PARAM;
    }
    if (!cfg_uid_valid(uid))
    {
        return CLOUD_EC_PARAM;
    }

    int r = lora_business_unbind(uid);
    switch (r)
    {
    case 0:
        return CLOUD_EC_OK;                 /* 异步发起, 真正删除在收到 ACK 后 */
    case -1:
        return CLOUD_EC_PARAM;              /* uid 非法(理论上前面已校验) */
    case -2:
        return CLOUD_EC_NO_DEVICE;          /* 1003: 不在从机表 */
    case -3:
        return CLOUD_EC_STATE;              /* 1006: 已有解绑在进行 */
    default:
        return CLOUD_EC_INTERNAL;
    }
}

int cloud_cmd_sen_unbind(const cloud_req_t *req, cJSON *data)
{
    return cloud_cmd_unbind(req, data, true);
}


/*!
 * \brief 某一行的字段是否被给了(没给就不动该字段)
 * \note  不能直接拿 cj_get_str() 的存在性判断(找不到就会把 out 置空),
 *        直接调会把没给的字段清成空 —— 那是静默丢数据。 */
static bool cloud_field_given(const char *json, const char *key)
{
    return (cj_get(json, key, NULL, 0u) != CJ_NOTFOUND);
}

/*!
 * \brief 取一个字符串字段, 且【超长就报错】—— 不是截断
 * \note  【这里有个坑, 实测踩过】cloud_json 的 cj_get_str() 在装不下时是
 *        【截断并返回 true】(_host_test/test_cloud.c 的用例 [3]), 所以直接用它会
 *        把平台的 name 写成 40 字节"被静默截成"32 字节 —— 平台以为设成功了。
 *        做法: 先取进一个足够大的临时缓冲, 再按目标缓冲的真实上限判长度。
 *        tmp 是 static: 本工程栈只有 4KB; 单线程轮询, 不会重入。 */
static bool cloud_take_str_field(const char *json, const char *key,
                                 char *dst, uint16_t dst_cap)
{
    static char tmp[192];

    if (!cj_get_str(json, key, tmp, (uint16_t)sizeof(tmp)))
    {
        return false;                       /* 缺失/非字符串 */
    }
    if (strlen(tmp) >= (size_t)dst_cap)
    {
        return false;                       /* 超长(或已被临时缓冲截断) -> 让调用方回 1001 */
    }
    (void)strcpy(dst, tmp);
    return true;
}

/** "level"/"distance" 映射成枚举, 非法返回 -1 */
static int cloud_measure_mode_of(const char *s)
{
    if (strcmp(s, "level") == 0)    { return (int)CFG_MEASURE_LEVEL; }
    if (strcmp(s, "distance") == 0) { return (int)CFG_MEASURE_DISTANCE; }
    return -1;
}

/**
 * @brief sen.set / dev.set —— 文档 5.3 第 9 条 / 5.4 第 13 条
 *   参数: uid(本工程口径) + 该域的可写字段(给了才改, 没给的不动)
 *   响应: code 0(文档这两条没有 data 字段)
 *   uid 没绑过 -> 1003; 值非法(越界/超长/枚举不认) -> 1001
 *
 * \note 范围校验不在这里重写一遍: 改完交给 config_store_validate() —— 它已经覆盖了
 *       文档 3.3 / 3.6 的全部取值范围(安装高度/校准偏移/周期/低电/超时/音量/闪光)。
 *       校验不过就当参数错返回, 副本丢掉, 配置里一个字都不动。 */
/* ==================== ★2026-10-05: sen.set -> LoRa 0x01 的默认值 ★ ====================
 * 用户口径: 平台只下发一个字段(上报周期), 另外几个用固定值。
 *   相同液位也上报 = 1               (2026-10-22 起协议固定, 不再是可配项)
 *   液位告警阈值 = 50mm = 5cm       (云端不下发, 设备侧固定用这个值)
 *   ★2026-10-22: 检测间隔字段已从 0x01 协议取消, 不再下发。
 * ⚠ 这两个是【网关侧填给设备】的值, 不是平台的配置字段。
 *   ⚠ 单位是【秒】(2026-10-05 起协议字段就是秒, 网关不换算)。
 *   ★2026-10-22: 固定值上移到 lora_business.h(LORA_SENSOR_*)★
 *     绑定成功后的"首次配置同步"也走同一组值 —— 两处各写一份迟早会漂。 */

/*!
 * \brief 把上报周期打成 0x01 帧投入 LoRa 下发缓存
 * \note  sen.set 与 cfg.set(sensors[]) 共用 —— 云端已改用 cfg.set 控制传感器,
 *        两条路径必须同口径, 否则走 cfg.set 时传感器本身收不到新周期。
 */
bool cloud_push_sensor_params(const char *uid, uint16_t report_period_s)
{
    if (!lora_business_set_sensor_params(uid,
                                         report_period_s,
                                         (uint16_t)LORA_SENSOR_ALARM_DEFAULT_MM))
    {
        log_info("[CLOUD] !! %s 的参数已落盘, 但 0x01 投递给 LoRa 失败"
                 "(见上一行 [CLOUD->LoRa] 的原因) —— 设备侧参数没变\r\n", uid);
        return false;
    }

    log_info("[CLOUD] %s 已落盘, 且 0x01 参数已投入下发缓存"
             "(上报=%us + 测量模式/安装高度/校准偏移/低电量阈值), 等它下次上报时搭车下发\r\n",
             uid, (unsigned)report_period_s);
    return true;
}

static int cloud_cmd_set_entry(const cloud_req_t *req, cJSON *data, bool is_sensor)
{
    char    uid[CFG_UID_STR_MAX];
    uint8_t i;
    int     idx  = -1;

    /* ★2026-10-05: 是否给了"设备侧参数" —— 决定要不要下发 0x01;
     *   offlineTimeoutSeconds 只在网关侧生效, 单独记一个标志。 */
    bool     device_dirty  = false;
    bool     offline_given = false;
    bool     report_given  = false;      /* 只用于 offlineTimeoutSeconds 的 3 倍自动对齐 */

    (void)data;                             /* 文档这两条响应没有 data */

    if (req->data == NULL)
    {
        return CLOUD_EC_PARAM;
    }
    if (!cj_get_str(req->data, "uid", uid, (uint16_t)sizeof(uid)) || !cfg_uid_valid(uid))
    {
        return CLOUD_EC_PARAM;
    }

    (void)memcpy(&cloud_cfg_staging_buf, config_store_get(), sizeof(cloud_cfg_staging_buf));

    for (i = 0u; i < cloud_cfg_staging_buf.sensor_count; i++)
    {
        if (strcmp(cloud_cfg_staging_buf.sensors[i].uid, uid) == 0) { idx = (int)i; break; }
    }
    (void)is_sensor;    /* 毕设只有液位；保留形参仅为减少调用点改动 */

    if (idx < 0)
    {
        return CLOUD_EC_NO_DEVICE;          /* 1003: set 只能改已绑定的, 不能凭空造一台 */
    }

    if (is_sensor)
    {
        cfg_sensor_t *s = &cloud_cfg_staging_buf.sensors[idx];

        /* 【字符串】给了才改, 超长要报错 —— 不是悄悄截断(平台会以为设成功) */
        if (cloud_field_given(req->data, "name") &&
            !cloud_take_str_field(req->data, "name", s->name, (uint16_t)sizeof(s->name)))
        {
            return CLOUD_EC_PARAM;
        }
        /* serial 是【只读】字段(文档 3.3: 绑定时录入)。给了就明确拒绝 ——
         * 静默忽略会让平台以为改成功了, 那是更坏的结果。 */
        if (cloud_field_given(req->data, "serial"))
        {
            return CLOUD_EC_PARAM;
        }
        if (cloud_field_given(req->data, "measureMode"))
        {
            char mm[12];
            int  m;

            if (!cj_get_str(req->data, "measureMode", mm, (uint16_t)sizeof(mm)))
            {
                return CLOUD_EC_PARAM;
            }
            m = cloud_measure_mode_of(mm);
            if (m < 0) { return CLOUD_EC_PARAM; }
            s->measure_mode = (uint8_t)m;
            device_dirty = true;        /* ★ 测量模式要下发给设备 ★ */
        }

        /* 【数值】先取 uint32, 再夹回目标宽度 —— 越界交给 validate(), 不在这里判 */
        if (cloud_field_given(req->data, "installHeightCm"))
        {
            uint32_t v = cj_get_uint(req->data, "installHeightCm", 0u);
            if (v > 0xFFFFu) { return CLOUD_EC_PARAM; }
            s->install_height_cm = (uint16_t)v;
            device_dirty = true;        /* ★ 安装高度要下发给设备 ★ */
        }
        if (cloud_field_given(req->data, "calibOffsetMm"))
        {
            int32_t v = cj_get_int(req->data, "calibOffsetMm", 0);
            if ((v < -32768) || (v > 32767)) { return CLOUD_EC_PARAM; }
            s->calib_offset_mm = (int16_t)v;
            device_dirty = true;        /* ★ 校准偏移要下发给设备 ★ */
        }
        if (cloud_field_given(req->data, "reportPeriodSeconds"))
        {
            uint32_t v = cj_get_uint(req->data, "reportPeriodSeconds", 0u);
            if (v > 0xFFFFu) { return CLOUD_EC_PARAM; }
            s->report_period_s = (uint16_t)v;
            device_dirty = true;        /* ★ 决定改完要不要下发 0x01 给传感器 ★ */
            report_given = true;        /* 自动对齐 offlineTimeoutSeconds 用 */
        }
        if (cloud_field_given(req->data, "lowBatteryPercent"))
        {
            uint32_t v = cj_get_uint(req->data, "lowBatteryPercent", 0u);
            /* ★2026-10-28: 0xFF -> 100 ★
             *   以前只拦 >255, 于是平台传 150 也能落盘; 而判定是
             *   `bat_pct <= 阈值`, 150 意味着【永远成立】= 告警常开。
             *   文档 §3.3 的取值范围是 0~100, config_parse 那边也一直是
             *   cfg_get_u8(..., 0u, 100u) —— 这里必须同口径。 */
            if (v > 100u) { return CLOUD_EC_PARAM; }
            s->low_battery_pct = (uint8_t)v;
            device_dirty = true;        /* ★ 低电量阈值要下发给设备 ★ */
        }
        if (cloud_field_given(req->data, "offlineTimeoutSeconds"))
        {
            uint32_t v = cj_get_uint(req->data, "offlineTimeoutSeconds", 0u);
            if (v > 0xFFFFu) { return CLOUD_EC_PARAM; }
            s->offline_timeout_s = (uint16_t)v;
            offline_given = true;
        }
        if (cloud_field_given(req->data, "enabled"))
        {
            s->enabled = cj_get_bool(req->data, "enabled", true) ? 1u : 0u;
        }
    }

    /* ---- ★2026-10-28: 平台只给一个字段(reportPeriodSeconds)时, 自动把
     *      offlineTimeoutSeconds 填成 3 倍★
     *
     * 【为什么要有这一步】用户口径是"平台只下发上报周期, 另一个字段用默认值"。
     *   而 offlineTimeoutSeconds 在本工程里【没有任何消费者】—— 失联判定用的是
     *   【3 × reportPeriodSeconds】(sensor_offline.c)。所以它只是个"给人看"的字段,
     *   但留着一个和上报周期明显不匹配的旧值(比如报 3600s 却写着 offline 90s)
     *   会让人误判失联逻辑。
     *   ★平台【显式给了】offlineTimeoutSeconds 就完全以平台为准, 这里一个字不改。★
     *   (config_store_validate 里"offline 必须 > report"那条校验已经去掉,
     *    所以这里不是"为了过校验", 纯粹是不留误导性的旧值。)
     *
     * ★2026-10-28 由 2 倍改 3 倍★ —— 甲方 V0.5 把约束改成"≥ 3×period",
     *   而实际判定本来就是 3×; 再写 2 倍会存出一个【文档非法、又比真实
     *   阈值小一半】的数(period=300 存 600, 文档要求 ≥900)。 */
    if (is_sensor && report_given && !offline_given)
    {
        /* report_period_s 上限 3600(config_store_validate), ×3 = 10800, uint16 装得下 */
        cloud_cfg_staging_buf.sensors[idx].offline_timeout_s =
            (uint16_t)(cloud_cfg_staging_buf.sensors[idx].report_period_s * 3u);
    }

    /* ---- ★2026-10-28(A2): 目标传感器离线 -> 1004, 且配置一个字都不写 ----
     * 依据: 协议 §5.3 第 7 条说明("目标传感器离线时回 1004", 1027 联调拍板)、
     *       协议 §4 错误码表 1004 = "目标设备离线"。
     * 【以前】1004 定义了但全工程没有任何一处返回: sen.set 一律先写配置再尝试下发,
     *   平台按文档写的离线分支永远走不到(实际表现是一直等到 1005 或超时)。
     *
     * 【判定范围: 只在"要下发给设备"时判】
     *   判据就是 device_dirty —— 本条命令带了 上报周期/测量模式/安装高度/校准偏移/
     *   低电量阈值 中的任意一项, 这些必须经 LoRa 下发到传感器才有意义。
     *   只改 name/enabled 这类【网关侧才有意义】的字段不判在线:
     *   否则设备一离线, 平台连改名、停用都做不了 —— 那是明显的过度拦截。
     *
     * 【判定口径: 与失联事件、sen.query.online 完全同源】
     *   距上次上报 >= 3 × reportPeriodSeconds 即离线(sensor_offline 那一套), 不另立标准。
     *   ★2026-11-02 修: 周期取 old / new 里【较大】的那个★
     *   原来取 staging(新周期)算阈值, 于是"把周期从 300 调成 5"这种操作会被
     *   当场判成离线(3×5=15s, 而上次上报已过去几十秒) -> 稳定回 1004,
     *   周期【只能调大不能调小】。现在用 max(old,new): 改配置本身永远不会把
     *   一台活着的设备判成离线, 只有设备真的沉默才会。
     *   从没上报过(没有快照) 按离线处理 —— 不下发到一台从没上线过的设备。
     *
     * 【为什么不判 dev.set】声光报警器从不发 0x02 周期上报, last_value 里没有快照,
     *   按上报时间判会把在线的报警器全判成离线; 它的在线状态只能问业务层
     *   (lora_business_device_state)。本期只按文档要求管 sen.set。
     *
     * ⚠️ 顺序: 必须在 cloud_commit_config() 之前 ——
     *   1004 的语义是"这条命令没执行", 不能出现"应答说离线、配置却已落盘"。
     *   staging 是副本, 这里 return 不会污染当前配置(下次进来重新 memcpy)。 */
    if (is_sensor && device_dirty)
    {
        uint8_t      uid_b[CFG_UID_BYTES];
        last_value_t lv;
        bool         online = false;

        /* ① 先判配置本身合不合法 —— 参数错必须回 1001, 不能被"离线"抢先答成 1004。
         *   否则平台改完安装高度(比如误填 3000)重试还是失败, 会一直以为是设备掉线。
         *   这里只是【预检】, 真正的落盘由下面的 cloud_commit_config 再做一次
         *   (它内部也调 validate, 重复一次是为了让本函数的返回码分得清)。 */
        if (config_store_validate(&cloud_cfg_staging_buf) != CFG_OK)
        {
            return CLOUD_EC_PARAM;
        }

        /* ② 再判在线
         * ★★2026-11-02 修: 不能再拿【staging 里的新周期】判在线★★
         * 【现场】平台把 reportPeriodSeconds 从 300 改成 5 —— 网关按 3×5=15s 算,
         *   而该传感器上一次上报是 30s 前 ⇒ 当场判"离线"、回 1004、配置不写。
         *   结果: **周期只能调大, 不能调小**, 而调小恰恰是最常见的需求
         *   (现场"间歇慢样本"排查时就要把周期压到 5~10s)。
         * 【现在】取 old / new 里【较大】的那个周期来算阈值:
         *   判据的含义变成"设备在它【已知的】任一周期下都算沉默", 于是
         *   —— 改配置本身永远【不会】把人判成离线, 只有设备真的不说话才会。
         *   注意: 新周期要等设备下一次上报、网关把 0x01 带下去之后才真正生效。 */
        if (cfg_uid_to_bytes(uid, uid_b) && last_value_get(uid_b, &lv))
        {
            uint16_t p_old = config_store_get()->sensors[idx].report_period_s;
            uint16_t p_new = cloud_cfg_staging_buf.sensors[idx].report_period_s;
            uint16_t p_use = (p_new > p_old) ? p_new : p_old;

            online = ((uint32_t)(get_tick_ms() - lv.rx_tick_ms)
                      < sensor_offline_threshold_ms(p_use));
        }

        if (!online)
        {
            log_info("[CLOUD] !! sen.set 目标 %s 离线 -> 1004(配置未写入, 也未下发)\r\n", uid);
            return CLOUD_EC_DEV_OFFLINE;
        }
    }

    {
        int rc = cloud_commit_config(&cloud_cfg_staging_buf);

        if (rc != CLOUD_EC_OK)
        {
            return rc;      /* 落盘失败: 配置一个字没动, 更不该去改设备 */
        }
    }

    /* ---- ★2026-10-05: 网关负责把参数打包成 LoRa 0x01 帧下发给传感器本身★
     *   顺序很关键: 必须【落盘成功之后】才动设备 —— 否则会出现
     *   "设备参数改了、网关配置没改"的分裂状态, 而且平台收到 1007 以为没生效。
     *   ★2026-10-22: 只要给了任一设备侧参数(上报周期/测量模式/安装高度/校准偏移/
     *   低电量阈值)就下发 0x01 —— 0x01 从 config_store 取完整参数, 不在这里逐个透传。
     *   改 name/enabled 这类【网关侧才有意义】的字段不下发。
     *   单位【秒】, 网关不换算。真正上空中继在传感器下次上报的应答窗口里(异步)。 */
    if (is_sensor && device_dirty)
    {
        const cfg_sensor_t *s = &config_store_get()->sensors[idx];

        (void)cloud_push_sensor_params(s->uid, s->report_period_s);
    }

    return CLOUD_EC_OK;
}

/* ★★2026-10-09 新增: 挡掉只读字段 serial★★
 *
 * 【为什么】`serial` 是【网关在绑定/注册时分配的】只读字段(配置里 devices[i].serial),
 *   平台不该写它 —— 写了我们也不认, 但**必须明确回 1001**, 不能静默忽略:
 *   静默忽略的话平台会以为设置成功了, 然后在 sen.query 里发现 serial 没变,
 *   又回来报"设置不生效"。
 *
 * 【A 层测试计划 A4 就是验这一条】("再带 serial 字段发一次" -> 期望 1001)。
 *
 * ⚠️ 覆盖 sen.set 与 dev.set 两条: serial 对两者都是网关分配的只读字段。
 *   (测试计划只点名 sen.set, 但性质相同, 一起挡更一致。)
 *
 * 【只挡顶层 serial】`data` 是平的: {"uid":"...","name":"...","serial":"..."}。
 *   如果 serial 嵌在子对象里(当前协议没有), 这里挡不住 —— 那时要按实际结构调整。 */
/* ⚠️ 参数是【原始 JSON 文本】不是 cJSON * —— 本工程的 cj_get() 系列工作在文本上
 *    (与 cloud_cfg_sections_mask 里的用法一致: cj_get(req->data, ...))。 */
static int cloud_cmd_check_readonly(const char *json)
{
    if ((json != NULL) && (cj_get(json, "serial", NULL, 0u) != CJ_NOTFOUND))
    {
        log_info("[CLOUD] !! 写命令带了只读字段 serial(网关分配, 平台不可写) -> 回 1001\r\n");
        return CLOUD_EC_PARAM;
    }
    return CLOUD_EC_OK;
}

int cloud_cmd_sen_set(const cloud_req_t *req, cJSON *data)
{
    int rc = cloud_cmd_check_readonly(req->data);

    if (rc != CLOUD_EC_OK)
    {
        return rc;
    }
    return cloud_cmd_set_entry(req, data, true);
}


/* ==================================================================
 * sen.query —— 文档 5.3 第 10 条 / dev.query —— 文档 5.4 第 15 条
 * ==================================================================
 * 【取本域配置的办法与 rule.query 完全一致】
 *   复用 config_store_write_members() 把 9 个域全写出来, 再把本条命令不该带的域删掉。
 *   这样 sensors[]/devices[] 的字段名与键序与 cfg.query 里同名对象**同源**,
 *   不会因为抄一遍字段表而漂(抄一遍 = 以后改配置结构要改两处, 两处必然不一致)。
 *
 * 【本工程补的运行时字段】
 *   文档要求这两条能带"最近一次上报"。该 uid 有上报记录时:
 *       lastLevelCm    最近水位(cm, = mm/10)   —— 只有传感器补(见下方 504 行说明)
 *       batteryPercent 电量百分比(0~100)
 *       lastTs         网关收到该帧时的 Unix 秒
 *   没有上报记录、或网关时钟未同步时, 这几个键【根本不出现】:
 *   不用 0 冒充 —— 平台会把 0 读成"水位 0cm / 电量 0% / 时间 1970",
 *   这比"这个字段没有"更糟, 也和我们别处"不拿默认值冒充真实值"的做法一致。
 *   uid 匹配: 配置里是 hex 串, 而快照表按键 13 字节, 用 cfg_uid_to_bytes() 转,
 *   转不出来就当没匹配上(跳过), 不猜。
 * ⚠️ 2026-10-28 更正: 这里原写 `levelCm`, 实际字段名是 `lastLevelCm`
 *    (`levelCm` 是 telemetry 的字段名, 两处口径不同, 别抄错)。
 * ================================================================== */
static void cloud_inject_last_values(cJSON *arr, bool with_level)
{
    cJSON *it;

    if ((arr == NULL) || (arr->type != cJSON_Array))
    {
        return;                         /* 该域不存在或不是数组: 不是错误 */
    }

    for (it = arr->child; it != NULL; it = it->next)
    {
        cJSON       *u;
        uint8_t      uid[CFG_UID_BYTES];
        last_value_t v;

        if (it->type != cJSON_Object)
        {
            continue;
        }
        u = cJSON_GetObjectItemCaseSensitive(it, "uid");
        if ((u == NULL) || (u->type != cJSON_String) || (u->valuestring == NULL))
        {
            continue;
        }
        if (!cfg_uid_to_bytes(u->valuestring, uid))
        {
            continue;                   /* uid 串非法, 跳过 */
        }

        /* ---- online(文档 5.3/5.4: sen.query / dev.query 每一项都要带) ----
         * 判据与失联检测用的是完全同一套: 距上次上报 < 3 × 该设备上报周期即在线。
         * 以前这里没有 online 字段 —— 平台只能靠"有没有 lastTs"去猜设备死活。
         * 从没上报过的设备也要回一条(online=false + 只有配置字段), 不能整项不出:
         * 平台"列表里没有它"会以为是配置丢了。 */
        {
            uint16_t period_s = sensor_offline_period_for_uid(config_store_get(),
                                                              u->valuestring);
            bool     have     = last_value_get(uid, &v);
            bool     is_on    = false;

            if (have)
            {
                is_on = ((uint32_t)(get_tick_ms() - v.rx_tick_ms)
                         < sensor_offline_threshold_ms(period_s));
            }
            else
            {
                /* ★2026-10-18: 没有快照(声光从不发 0x02, 永远不进快照表)
                 *   => 问业务层的真实状态。声光按"是否被判过离线"算,
                 *      否则它的 online 会恒为 false, 平台一直误报掉线。 */
                is_on = (lora_business_device_state(u->valuestring)
                         == LORA_DEV_STATE_ONLINE);
            }
            (void)cJSON_AddBoolToObject(it, "online", is_on ? 1 : 0);
            if (!have)
            {
                continue;               /* 从没上报过: 只有 online=false + 配置字段 */
            }
        }

        if (with_level)
        {
            /* 字段名按文档 5.3 第 10 条: 【lastLevelCm】
             * (2026-09-30 用户确认: 对云端来说每次收到的都是"最新一条", 所以用 last 前缀。
             *  以前发的是 levelCm —— 那是 telemetry 的字段名, 两处口径不同。) */
            (void)cJSON_AddNumberToObject(it, "lastLevelCm", (double)v.level_mm / 10.0);
        }
        (void)cJSON_AddNumberToObject(it, "batteryPercent", (double)v.bat_pct);
        if (v.rx_unix != 0u)
        {
            (void)cJSON_AddNumberToObject(it, "lastTs", (double)v.rx_unix);
        }
        /* 加不进去(池满)时只丢这几个运行时字段, 不让整条命令失败 ——
         * 配置部分是主体且已经写好了; cJSON_Add* 失败返回 NULL, 不会破坏对象。 */
    }
}

int cloud_cmd_sen_query(const cloud_req_t *req, cJSON *data)
{
    static const char *const drop[] =
    {
        "system", "network", "thresholds", "ota", "meta"
    };
    uint8_t i;

    (void)req;                          /* 文档这条没有参数 */

    if (!config_store_write_members(data, config_store_get()))
    {
        return CLOUD_EC_INTERNAL;       /* 池满 */
    }

    for (i = 0u; i < (uint8_t)(sizeof(drop) / sizeof(drop[0])); i++)
    {
        cJSON_DeleteItemFromObject(data, drop[i]);
    }

    cloud_inject_last_values(cJSON_GetObjectItemCaseSensitive(data, "sensors"), true);
    return CLOUD_EC_OK;
}



/* ==================================================================
 * sen.bind / dev.bind(文档 5.3 第 7 条 / 5.4 第 11 条) + 配对后端钩子
 * ==================================================================
 * 文档语义:
 *   参数 serial(可选, 8~16 字节) + name(可选) + pairWindowSeconds(可选, 默认 60,
 *        范围 1~300 —— 2026-10-28 收敛, 见 CLOUD_PAIR_WINDOW_MAX);
 *   响应 code 0 + data:{"pairWindowSeconds":N} —— 只表示"窗口已开";
 *   真实绑定结果【异步】经 event.device(bound / bind_failed)上报;
 *   已有 5 个传感器时拒绝(错误码 1009)。
 * ⚠️ 2026-10-28 更正: serial 自 2026-10-08 起【不再必填】(用户口径"这个字段直接丢"),
 *    给了也只是存作备注; 原注释写"必填"会让平台多做无用的字段校验。
 *    字段本身仍在配置结构里(弃用但未删除), 平台侧同样弃用但保留。
 * ================================================================== */
static cloud_pair_open_fn s_pair_fn;

void cloud_proto_set_pair_hook(cloud_pair_open_fn fn)
{
    s_pair_fn = fn;
}

static int cloud_cmd_bind(const cloud_req_t *req, cJSON *data)
{
    char     serial[CFG_SERIAL_MAX + 1u];
    char     name[CFG_NAME_MAX + 1u];
    uint32_t window_s = 60u;            /* 文档默认 60 */

    if (req->data == NULL)
    {
        return CLOUD_EC_PARAM;
    }

    /* ★★2026-10-08: `serial` 不再必填 —— 用户口径"这一个字段直接丢"★★
     *
     * 原实现要求 serial 必须存在且 8~16 字节, 缺了直接 1001。但平台侧的绑定请求
     * 里根本不带这个字段(用户给的样例):
     *     {"cmd":"sen.bind","msgId":"f4-01",...,"data":{"name":"西入口液位","pairWindowSeconds":60}}
     * ⇒ 原实现会让【每一次绑定都失败】, 而且错误码只有 1001, 现场很难看出是 serial 的锅。
     *
     * 现在的语义: 给了就当备注存下来(仍然超长报 1001, 不静默截断), 没给就留空。
     * 绑定的身份完全由 `uid` 决定(见 §1.2: addr 与 serial 都不参与寻址)。
     * 注: config_store 里 sensors[].serial / devices[].serial 仍然存在(留作备注,
     *     也兼容旧配置), 只是不再被本命令强制要求。 */
    serial[0] = '\0';
    if (cloud_field_given(req->data, "serial") &&
        (!cloud_take_str_field(req->data, "serial", serial, (uint16_t)sizeof(serial))))
    {
        return CLOUD_EC_PARAM;          /* 给了但超长/非字符串 -> 仍然 1001 */
    }

    name[0] = '\0';                     /* name 可选 */
    if (cloud_field_given(req->data, "name") &&
        !cloud_take_str_field(req->data, "name", name, (uint16_t)sizeof(name)))
    {
        return CLOUD_EC_PARAM;          /* 给了但超长/非字符串 -> 报错, 不静默丢 */
    }

    if (cloud_field_given(req->data, "pairWindowSeconds"))
    {
        if (cj_get(req->data, "pairWindowSeconds", NULL, 0u) != CJ_NUM)
        {
            return CLOUD_EC_PARAM;
        }
        /* ★2026-10-28: 范围收敛到 1~300(手册 §4.3)★
         * 原来只判 !=0 且 <=65535, 平台发 3600 也会被收下 —— 与手册口径不符。
         * 缺省仍是 60s(cj_get_uint 的默认值), 不传不报错。 */
        window_s = cj_get_uint(req->data, "pairWindowSeconds", 60u);
        if ((window_s < 1u) || (window_s > CLOUD_PAIR_WINDOW_MAX))
        {
            return CLOUD_EC_PARAM;          /* 0 或 >300 -> 1001 */
        }
    }

    /* 容量上限与运行期 0x04 绑定握手里的
     * config_store_can_add 同口径。这条是纯配置判断, 不需要 LoRa —— 先在这里拦住,
     * 别等到配对成功了才发现放不下。
     * 【注意】这里只判"还剩不剩位置", 真正的绑定由 config_store 在收到设备 ACK 后
     *   经 lora_business 的 config_store_bind_device 完成(即 0x04 握手路径里再判一次)。 */
    if (!config_store_can_add(true))
    {
        return CLOUD_EC_LIMIT;          /* 1009 */
    }

    if (s_pair_fn == NULL)
    {
        log_info("[CLOUD] sen.bind: 本机还没注册配对后端, 回 1006\r\n");
        return CLOUD_EC_STATE;
    }

    if (!s_pair_fn(serial, name, (uint16_t)window_s))
    {
        return CLOUD_EC_INTERNAL;       /* 开窗口失败 -> 1007 */
    }

    if (cJSON_AddNumberToObject(data, "pairWindowSeconds", (double)window_s) == NULL)
    {
        return CLOUD_EC_INTERNAL;       /* 池满 */
    }
    return CLOUD_EC_OK;
}

int cloud_cmd_sen_bind(const cloud_req_t *req, cJSON *data)
{
    return cloud_cmd_bind(req, data);
}

