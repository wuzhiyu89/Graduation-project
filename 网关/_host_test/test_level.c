/**
 * \file  test_level.c
 * \brief 边缘水位评估纯逻辑的主机单测(见 app/service/level_eval.h)
 *
 * 这一组测试的价值在于【边界等号】: 文档 3.4 的表里
 *   Warning 是 warningCm < level <  alarmCm  (两头都是开区间)
 *   Normal  是 resolveCm < level <= warningCm(右边是闭区间!)
 *   Resolved 是 level <= resolveCm
 * 少一个等号, 现象是"水位正好停在阈值上时档位不对" —— 现场极难复现, 只在主机上能一次测完。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "log.h"
#include "level_eval.h"

static int g_fail;
static int g_pass;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) { g_pass++; printf("  ok   : "); }             \
        else      { g_fail++; printf("  FAIL : "); }             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    } while (0)

/* 文档默认: 告警 10cm / 预警 7cm / 解除 2cm; 最小变化 1cm; 事件间隔 60s; confirmCount 1 */
static cfg_thresholds_t make_t(void)
{
    cfg_thresholds_t t;

    (void)memset(&t, 0, sizeof(t));
    t.alarm_cm             = 10u;
    t.warning_cm           = 7u;
    t.resolve_cm           = 2u;
    t.min_change_cm        = 1u;
    t.event_min_interval_s = 60u;
    t.confirm_count        = 1u;
    return t;
}

/* ============================================================================
 * 档位 -> LoRa 动作载荷(自治"最后一跳"的组帧, 用户 2026-09-30 第 2 条)
 * ----------------------------------------------------------------------------
 * 这些字节最终会原样进 LoRa 帧发给声光/LED 设备, 错了就是"设备乱码/不动作",
 * 现场很难查 —— 所以布局与截断规则全部在这里钉死。
 * ⚠️ 布局本身"待声光设备协议确认"(见 level_eval.h), 定稿后本组用例要跟着改。
 * ========================================================================== */
static void test_gear_payload(void)
{
    cfg_gear_t g;
    uint8_t    buf[LEVEL_GEAR_PAYLOAD_MAX];
    uint8_t    n;

    printf("\n[gear] 档位动作组帧(最后一跳)\n");

    memset(&g, 0, sizeof(g));

    /* ---- 档位 0(解除): 总电源必须关 ---- */
    g.gear = 0u; g.sound_mode = 0u;
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK((n == 8u) && (buf[0] == 0u) && (buf[3] == 50u),
          "★ 档位0: 总电源=0(关) 音量=50, 三段长度域都是0 (n=%u)", (unsigned)n);
    CHECK((buf[5] == 0u) && (buf[6] == 0u) && (buf[7] == 0u),
          "★ 三个长度字节都在 [5][6][7] 且为 0");

    /* ---- ★2026-11-02 现场 bug: 档位 0 配了文本却关屏 ---- */
    memset(&g, 0, sizeof(g));
    g.gear = 0u; g.sound_mode = 0u;
    n = level_gear_build_payload(&g, 50u, "没有水", "", "", buf, sizeof(buf));
    CHECK((n > 9u) && (buf[0] == 1u),
          "★★ 档位0 配了文本 -> 总电源=1(不能因 gear==0 就关屏) (n=%u buf[0]=%u)",
          (unsigned)n, (unsigned)buf[0]);

    memset(&g, 0, sizeof(g));
    g.gear = 0u; g.led_color[0] = 1u;      /* 只配了颜色, 没文本 */
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK((n == 8u) && (buf[0] == 1u),
          "★ 档位0 只配了非 off 的颜色 -> 也开屏 (buf[0]=%u)", (unsigned)buf[0]);

    memset(&g, 0, sizeof(g));
    g.gear = 0u; g.sound_mode = 2u;        /* 只要闪灯也要开屏 */
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK((n == 8u) && (buf[0] == 1u), "★ 档位0 配了闪灯 -> 开屏 (buf[0]=%u)", (unsigned)buf[0]);

    /* ---- off fail-safe 故障画面: 电源开/不闪/音量0/竖屏/居中/黄黄 ---- */
    memset(&g, 0, sizeof(g));
    g.gear = 1u; g.sound_mode = 0u; g.led_show_mode = 1u;
    g.led_text_show_mode[0] = 0u; g.led_text_show_mode[1] = 0u;
    g.led_color[0] = (uint8_t)CFG_COLOR_YELLOW;
    g.led_color[1] = (uint8_t)CFG_COLOR_YELLOW;
    n = level_gear_build_payload(&g, 0u, "设备故障", "勿依赖提示", "", buf, sizeof(buf));
    CHECK((n == 35u) && (buf[0] == 1u) && (buf[1] == 0u) && (buf[3] == 0u),
          "★ off 故障画面: 电源开/不闪/音量0 (n=%u)", (unsigned)n);
    CHECK(buf[4] == 0x8Fu, "★ off 故障画面: 竖屏+两行居中+黄黄 => 0x8F (实际 0x%02X)",
          (unsigned)buf[4]);
    CHECK((buf[5] == 12u) && (buf[18] == 15u) && (buf[34] == 0u),
          "★ off 故障画面: 两行长度/无TTS 正确");


    /* ---- ★2026-11-11 新增: [2] 音色 voice 必须真进载荷★ ----
     * 背景: voice 从 11-03 起只在配置结构里, 载荷一直没这一字节 ——
     *   平台"改音色成功"实际不改变设备播报。本条把"真的发出去了"钉死。
     * ⚠ 位置是 [2](报警灯光模式之后), 音量在 [3] —— 与设备侧偏移表一致;
     *   两者互换会让设备把音量当音色播、把音色值当音量。 */
    memset(&g, 0, sizeof(g));
    g.gear = 1u; g.voice = 5u;                 /* 5 = 唐老鸭 */
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK((buf[2] == 5u) && (buf[3] == 50u) && (n == 8u),
          "★★ [2]=voice=5(唐老鸭), [3]=音量=50, 固定头仍 5 字节 "
          "(buf[2]=%u buf[3]=%u n=%u)",
          (unsigned)buf[2], (unsigned)buf[3], (unsigned)n);
    g.voice = 0u;
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK(buf[2] == 0u, "★ 缺省 voice=0(艾佳女声) (实际 %u)", (unsigned)buf[2]);
    /* 脏值兜底: 验证期已拦 0~5, 这里 &0x07 保证不会把高位打给设备 */
    g.voice = 0xFFu;
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK(buf[2] == 7u, "★ 越界 voice=0xFF -> &0x07 = 7, 不打坏下游 (实际 %u)",
          (unsigned)buf[2]);

    /* ---- ★[4] LED屏模式 位定义(最容易错的一字节)★ ----
     * bit7 横竖 | bit6 预留0 | bit5 半屏1对齐 | bit4 半屏2对齐
     * | bit3,2 半屏1颜色 | bit1,0 半屏2颜色
     * 对接文档的例子: 1001 0101 = 竖屏 + 预留0 + 居中 + 对齐 + 左红 + 右红 */
    g.gear = 1u; g.sound_mode = 1u;
    g.led_show_mode = 1u;                      /* 竖屏 */
    g.led_text_show_mode[0] = 2u;              /* 居中 -> bit5 = 0 */
    g.led_text_show_mode[1] = 1u;              /* 上/左对齐 -> bit4 = 1 */
    g.led_color[0] = (uint8_t)CFG_COLOR_RED;   /* 01 */
    g.led_color[1] = (uint8_t)CFG_COLOR_RED;   /* 01 */
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK(buf[4] == 0x95u,
          "★★ 对接文档的例子: 竖屏/居中/对齐/左红/右红 => [4] = 0x95 (实际 0x%02X)",
          (unsigned)buf[4]);

    /* 逐位拆开验证, 出错时能一眼看出是哪一位 */
    g.led_show_mode = 0u;
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK(buf[4] == 0x15u, "★ bit7: 横屏 => 0x15 (实际 0x%02X)", (unsigned)buf[4]);
    g.led_text_show_mode[0] = 0u; g.led_text_show_mode[1] = 0u;
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK(buf[4] == 0x05u, "★ bit5/bit4: 都居中 => 0x05 (实际 0x%02X)", (unsigned)buf[4]);
    g.led_color[0] = (uint8_t)CFG_COLOR_GREEN;  /* 10 */
    g.led_color[1] = (uint8_t)CFG_COLOR_YELLOW; /* 11 */
    n = level_gear_build_payload(&g, 50u, "", "", "", buf, sizeof(buf));
    CHECK(buf[4] == 0x0Bu,
          "★ bit3,2=绿(10) bit1,0=黄(11) => 0x0B (实际 0x%02X) —— 红绿不能反",
          (unsigned)buf[4]);

    /* ---- 档位 1: 只有文本1, 4 汉字 = 12 字节 ---- */
    memset(&g, 0, sizeof(g));
    g.gear = 1u;
    n = level_gear_build_payload(&g, 50u, "缓慢通行", "", "", buf, sizeof(buf));
    CHECK((n == 20u) && (buf[0] == 1u) && (buf[5] == 12u),
          "★ 档位1: 总电源=1, 文本1 4 汉字=12 字节 (n=%u buf[5]=%u)",
          (unsigned)n, (unsigned)buf[5]);
    CHECK(memcmp(&buf[6], "缓慢通行", 12u) == 0, "★ 文本1 原样(UTF-8 汉字 3 字节)");
    CHECK((buf[18] == 0u) && (buf[19] == 0u),
          "★ 文本2/TTS 长度域都是 0 (文本2在[18], TTS在[19])");
    CHECK(n == (uint8_t)(LEVEL_GEAR_HDR_LEN + 1u + 12u + 1u + 1u),
          "★ 总长 = 固定头5 + 1+12(文本1) + 1(文本2空) + 1(TTS空)");

    /* ---- 档位 3: 两段 LED + TTS 全给 ---- */
    memset(&g, 0, sizeof(g));
    g.gear = 3u; g.sound_mode = 3u;             /* 3 = 快闪 */
    n = level_gear_build_payload(&g, 80u, "禁止通行", "水深危险", "请注意", buf, sizeof(buf));
    CHECK((buf[1] == 3u) && (buf[3] == 80u), "★ 灯光模式=3(快闪) 音量=80");
    CHECK((buf[5] == 12u) && (buf[18] == 12u) && (buf[31] == 9u),
          "★ 三个长度域: [5]=12 [18]=12(7+11) [31]=9(请注意=3汉字) (实际 %u/%u/%u)",
          (unsigned)buf[5], (unsigned)buf[18], (unsigned)buf[31]);
    CHECK(memcmp(&buf[6],  "禁止通行", 12u) == 0, "★ 文本1 在原位");
    CHECK(memcmp(&buf[19], "水深危险", 12u) == 0, "★ 文本2 在原位(起点 7+N = 19)");
    CHECK(memcmp(&buf[32], "请注意",   9u)  == 0, "★ TTS 在原位(起点 8+N+M = 32)");
    CHECK(n == 41u, "★ 总长 = 5 + 13 + 13 + 10 = 41 (实际 %u)", (unsigned)n);

    /* ---- ★TTS 空 = 不播报(与 soundMode 解耦)★ ---- */
    memset(&g, 0, sizeof(g));
    g.gear = 2u; g.sound_mode = 2u;             /* 慢闪 */
    n = level_gear_build_payload(&g, 50u, "A", "", "", buf, sizeof(buf));
    CHECK((n == 9u) && (buf[7] == 0u),
          "★ soundMode=2 但 TTS 为空 => TTS 长度=0, 不播报 (n=%u)", (unsigned)n);
    /* 反过来: soundMode=0(关闭) 但给了 TTS -> 照编进去(语音不再看 soundMode) */
    g.sound_mode = 0u;
    n = level_gear_build_payload(&g, 50u, "A", "", "说话", buf, sizeof(buf));
    /* 布局: [5]=N=1 | [6]=A | [7]=M=0 | [8]=K=6 | [9..14]=说话 */
    CHECK(buf[8] == 6u, "★ soundMode=0 但 TTS 非空 => 照编进去 (K=%u)", (unsigned)buf[8]);

    /* ---- 边界: 文本放不下时【在字符边界截断】, 绝不出现半个汉字 ----
     * 2026-10-28 起三段【各自独立】限长(不再有合计预算):
     *     文本1 ≤ 48、文本2 ≤ 48、TTS ≤ 88 字节。 */
    memset(&g, 0, sizeof(g));
    g.gear = 1u;
    {
        /* 68 个汉字 = 204 字节 > 单段上限 48 -> 必须截到 48 字节(16 个汉字) */
        static char s68[68 * 3 + 1];
        uint8_t i;
        for (i = 0u; i < 68u; i++) { memcpy(&s68[i * 3], "一", 3u); }
        s68[68 * 3] = '\0';

        n = level_gear_build_payload(&g, 50u, s68, "", "", buf, sizeof(buf));
        CHECK((buf[5] == 48u) && (n == 56u),
              "★ 204 字节文本 -> 按单段上限 48 截断(16 个汉字), 总长 56 "
              "(头5 + 3 个长度域 + 48) (长=%u n=%u)", (unsigned)buf[5], (unsigned)n);
        CHECK((n <= (uint8_t)LEVEL_GEAR_PAYLOAD_MAX),
              "★ 绝不越界(<= %u)", (unsigned)LEVEL_GEAR_PAYLOAD_MAX);
    }

    /* ---- 三段同时用满: 48 + 48 + 88 = 184, 总长 192, 必须仍在 206 以内 ---- */
    {
        static char s48[49];
        static char s88[89];
        uint8_t i;

        for (i = 0u; i < 16u; i++) { memcpy(&s48[i * 3], "一", 3u); }
        s48[48] = '\0';                                     /* 16 汉字 = 48 字节 */
        for (i = 0u; i < 29u; i++) { memcpy(&s88[i * 3], "一", 3u); }
        s88[87] = 'A'; s88[88] = '\0';                      /* 29 汉字 + 1 ASCII = 88 */

        n = level_gear_build_payload(&g, 50u, s48, s48, s88, buf, sizeof(buf));
        CHECK((buf[5] == 48u) && (buf[54] == 48u) && (buf[103] == 88u) && (n == 192u),
              "★ 三段分别用满 48/48/88: 总长 192 (n=%u)", (unsigned)n);
        CHECK((n <= (uint8_t)LEVEL_GEAR_PAYLOAD_MAX),
              "★ 三段用满也不越界(<= %u)", (unsigned)LEVEL_GEAR_PAYLOAD_MAX);
    }

    /* ---- 参数健壮性 ---- */
    CHECK(level_gear_build_payload(NULL, 50u, "x", "", "", buf, sizeof(buf)) == 0u,
          "g=NULL -> 0");
    CHECK(level_gear_build_payload(&g, 50u, "x", "", "", NULL, sizeof(buf)) == 0u,
          "out=NULL -> 0");
    CHECK(level_gear_build_payload(&g, 50u, "x", "", "", buf, 4u) == 0u,
          "cap < 固定头5 -> 0");
    CHECK(level_gear_build_payload(&g, 50u, NULL, NULL, NULL, buf, sizeof(buf)) > 0u,
          "文本可 NULL(当作空文本)");
}

static void test_gear_targets(void)
{
    config_store_t cfg;
    uint8_t        idx[CFG_DEVICE_MAX];
    uint8_t        n;

    printf("\n[gear] 目标设备选择\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.device_count = 3u;
    (void)strcpy(cfg.devices[0].uid, "31000000000000000000000001"); cfg.devices[0].enabled = 1u;
    (void)strcpy(cfg.devices[1].uid, "31000000000000000000000002"); cfg.devices[1].enabled = 0u;  /* 停用 */
    (void)strcpy(cfg.devices[2].uid, "31000000000000000000000003"); cfg.devices[2].enabled = 1u;

    /* targets = "all" -> 只有 2 台启用 */
    cfg.gears[1].gear = 1u; cfg.gears[1].targets_all = 1u;
    n = level_gear_collect_targets(&cfg, 1u, NULL, idx, CFG_DEVICE_MAX);
    CHECK((n == 2u) && (idx[0] == 0u) && (idx[1] == 2u),
          "★ all: 选中 2 台(跳过停用的 devices[1]) (n=%u)", (unsigned)n);

    /* targets = 列表 -> 只发列出的, 且停用的仍被跳过 */
    cfg.gears[2].gear = 2u; cfg.gears[2].targets_all = 0u;
    cfg.gears[2].target_count = 2u;
    (void)strcpy(cfg.gears[2].target_uid[0], "31000000000000000000000002");   /* 停用 -> 跳过 */
    (void)strcpy(cfg.gears[2].target_uid[1], "31000000000000000000000003");
    n = level_gear_collect_targets(&cfg, 2u, NULL, idx, CFG_DEVICE_MAX);
    CHECK((n == 1u) && (idx[0] == 2u),
          "★ 列表: 只选 310003(310002 虽在表里但已停用) (n=%u)", (unsigned)n);

    /* 列表里写了一个不存在的 uid -> 没有目标 */
    cfg.gears[3].gear = 3u; cfg.gears[3].targets_all = 0u;
    cfg.gears[3].target_count = 1u;
    (void)strcpy(cfg.gears[3].target_uid[0], "999999");
    n = level_gear_collect_targets(&cfg, 3u, NULL, idx, CFG_DEVICE_MAX);
    CHECK(n == 0u, "★ 目标 uid 不存在 -> 0 台(不瞎发)");

    /* 容量: cap 小于实际台数时计数仍然正确(调用方能察觉) */
    cfg.gears[1].targets_all = 1u;
    n = level_gear_collect_targets(&cfg, 1u, NULL, idx, 1u);
    CHECK(n == 2u, "★ cap=1 时返回值仍是 2(台数不被 cap 掩盖) (n=%u)", (unsigned)n);

    /* ---- only_uid(dev.ctrl 手动控制的定位口径) ---- */
    n = level_gear_collect_targets(&cfg, 1u, "31000000000000000000000003", idx, CFG_DEVICE_MAX);
    CHECK((n == 1u) && (idx[0] == 2u),
          "★ only_uid=310003: 只选它那一台(即使档位是 all) (n=%u)", (unsigned)n);
    n = level_gear_collect_targets(&cfg, 1u, "31000000000000000000000002", idx, CFG_DEVICE_MAX);
    CHECK(n == 0u, "★ only_uid 指向已停用的设备 -> 0 台(停用的不接动作)");
    n = level_gear_collect_targets(&cfg, 1u, "999999", idx, CFG_DEVICE_MAX);
    CHECK(n == 0u, "★ only_uid 不存在 -> 0 台");

    /* ---- sensorOfflinePolicy -> 档位(第 7 条 fail-safe 的映射) ---- */
    printf("\n[gear] fail-safe 策略映射\n");
    CHECK(level_failsafe_gear((uint8_t)CFG_OFFLINE_ALARM, 1u) == 3u,
          "★ alarm -> 强制档位 3");
    CHECK(level_failsafe_gear((uint8_t)CFG_OFFLINE_OFF, 3u) == 0u,
          "★ off -> 档位 0");
    CHECK(level_failsafe_gear((uint8_t)CFG_OFFLINE_KEEP, 2u) == 2u,
          "★ keep -> 保持当前档位(原样返回)");
    CHECK(level_failsafe_gear(99u, 2u) == 2u,
          "★ 策略越界 -> 按 keep 处理(不猜、不误动作)");

    CHECK(level_gear_collect_targets(NULL, 1u, NULL, idx, CFG_DEVICE_MAX) == 0u, "cfg=NULL -> 0");
    CHECK(level_gear_collect_targets(&cfg, 1u, NULL, NULL, CFG_DEVICE_MAX) == 0u, "out=NULL -> 0");
    CHECK(level_gear_collect_targets(&cfg, 99u, NULL, idx, CFG_DEVICE_MAX) == 0u, "档位越界 -> 0");
}

int main(void)
{
    cfg_thresholds_t t = make_t();
    level_eval_t     e;
    level_event_gate_t g;
    level_state_t    st;
    int32_t          out;
    int32_t          lv[3];
    uint8_t          sel[3];

    printf("=== 边缘水位评估纯逻辑测试 ===\n\n");
    host_log_enable(0);

    /* ================= [1] 阈值 -> 状态: 边界等号 ================= */
    printf("\n[1] level_state_of 边界(10/7/2 cm -> 100/70/20 mm)\n");
    CHECK(level_state_of(100, &t) == LEVEL_ST_ALARM,   "100mm(=10.0cm) >= alarmCm -> Alarm(闭)");
    CHECK(level_state_of(101, &t) == LEVEL_ST_ALARM,   "101mm -> Alarm");
    CHECK(level_state_of(99,  &t) == LEVEL_ST_WARNING, "99mm 在 (70,100) 开区间 -> Warning");
    CHECK(level_state_of(71,  &t) == LEVEL_ST_WARNING, "71mm -> Warning");
    CHECK(level_state_of(70,  &t) == LEVEL_ST_NORMAL,  "★70mm(=7.0cm) == warningCm -> Normal(右闭)");
    CHECK(level_state_of(21,  &t) == LEVEL_ST_NORMAL,  "21mm -> Normal");
    CHECK(level_state_of(20,  &t) == LEVEL_ST_RESOLVED,"★20mm(=2.0cm) == resolveCm -> Resolved(闭)");
    CHECK(level_state_of(0,   &t) == LEVEL_ST_RESOLVED,"0mm -> Resolved");
    CHECK(level_state_of(-5,  &t) == LEVEL_ST_RESOLVED,"负值(传感器异常)也不能崩, 按 Resolved");
    CHECK(level_state_of(100, NULL) == LEVEL_ST_UNKNOWN, "阈值 NULL -> UNKNOWN(不猜)");

    /* ================= [2] 状态 -> 档位 ================= */
    printf("\n[2] 状态 -> 档位(文档 3.4 固定映射)\n");
    CHECK(level_state_to_gear(LEVEL_ST_ALARM)    == 3u, "Alarm -> 3");
    CHECK(level_state_to_gear(LEVEL_ST_WARNING)  == 2u, "Warning -> 2");
    CHECK(level_state_to_gear(LEVEL_ST_NORMAL)   == 1u, "Normal -> 1");
    CHECK(level_state_to_gear(LEVEL_ST_RESOLVED) == 0u, "Resolved -> 0");
    CHECK(level_state_to_gear(LEVEL_ST_UNKNOWN)  == 0u, "Unknown -> 0(还没测到水位)");

    /* ================= [3] 有效水位: 在线且 enabled 的取 max ================= */
    printf("\n[3] 有效水位(文档 3.3 末: 取最大值)\n");
    lv[0] = 30; lv[1] = 120; lv[2] = 80;
    sel[0] = 1u; sel[1] = 1u; sel[2] = 1u;
    CHECK(level_effective(lv, sel, 3u, &out) && (out == 120), "三台都有效 -> max = 120");
    sel[1] = 0u;                                    /* 最高的那台离线/停用 */
    CHECK(level_effective(lv, sel, 3u, &out) && (out == 80), "★离线/停用的不参与 -> max = 80");
    sel[0] = 0u; sel[2] = 0u;
    CHECK(!level_effective(lv, sel, 3u, &out), "★一台都没有 -> false(交给 fail-safe)");
    CHECK(!level_effective(NULL, sel, 3u, &out), "levels=NULL -> false");
    CHECK(!level_effective(lv, sel, 0u, &out), "n=0 -> false");

    /* ================= [4] confirmCount 防抖 ================= */
    printf("\n[4] confirmCount: 连续 N 次一致才确认\n");
    t.confirm_count = 3u;
    level_eval_init(&e);

    CHECK(!level_eval_feed(&e, &t, 80, &st), "第 1 次 Warning(80mm) -> 还没到 3 次, 不迁移");
    CHECK(!level_eval_feed(&e, &t, 80, &st), "第 2 次 -> 仍不迁移");
    CHECK(level_eval_feed(&e, &t, 80, &st) && (st == LEVEL_ST_WARNING),
          "★第 3 次一致 -> 确认迁移到 Warning");
    CHECK(level_state_to_gear(st) == 2u, "迁移后的档位是 2");
    CHECK(!level_eval_feed(&e, &t, 80, &st), "已确认后再喂同样状态 -> 不再迁移(档位未变不重发)");

    CHECK(!level_eval_feed(&e, &t, 150, &st), "改成 Alarm, 第 1 次 -> 不迁移");
    CHECK(!level_eval_feed(&e, &t, 80,  &st), "★中途跳回 Warning -> 计数被打断, 不迁移");
    CHECK(!level_eval_feed(&e, &t, 150, &st), "再 Alarm 第 1 次");
    CHECK(!level_eval_feed(&e, &t, 150, &st), "第 2 次");
    CHECK(level_eval_feed(&e, &t, 150, &st) && (st == LEVEL_ST_ALARM),
          "★连续 3 次 Alarm -> 确认迁移(中间的打断确实把计数清了)");

    printf("\n[5] confirmCount = 1(出厂默认) 等价于立刻迁移\n");
    t.confirm_count = 1u;
    level_eval_init(&e);
    CHECK(level_eval_feed(&e, &t, 10, &st) && (st == LEVEL_ST_RESOLVED),
          "★上电第一次也【算迁移】: 网关要按当前水位把档位摆正");
    CHECK(level_eval_feed(&e, &t, 150, &st) && (st == LEVEL_ST_ALARM), "1 次就迁移到 Alarm");
    CHECK(level_eval_feed(&e, &t, 20, &st) && (st == LEVEL_ST_RESOLVED), "落回 Resolved");

    /* ================= [6] 事件去抖: minChangeCm + 间隔 ================= */
    printf("\n[6] 事件去抖(文档 3.4: minChangeCm=1cm / eventMinIntervalSeconds=60s)\n");
    level_event_gate_init(&g);
    CHECK(level_event_should_fire(&g, &t, 80, 0u), "★第一次总是要出事件");
    level_event_mark(&g, 80, 0u);

    CHECK(!level_event_should_fire(&g, &t, 80, 100000u), "水位没变 -> 不出(变化量 < 1cm)");
    CHECK(!level_event_should_fire(&g, &t, 85, 1000u),  "变化只有 5mm < minChangeCm(10mm) -> 不出");
    CHECK(!level_event_should_fire(&g, &t, 95, 1000u),  "★变化够(15mm>=10mm)但间隔不够(1s<60s) -> 不出");
    CHECK(level_event_should_fire(&g, &t, 95, 60000u),  "★变化够 + 间隔够(60s) -> 出");
    level_event_mark(&g, 95, 60000u);

    CHECK(level_event_should_fire(&g, &t, 85, 120000u), "★水位下降也算变化(取绝对值)");
    level_event_mark(&g, 85, 120000u);

    /* 毫秒回绕: (uint32)now - last 的无符号差值口径 */
    level_event_gate_init(&g);
    level_event_mark(&g, 100, 0xFFFFFF00u);
    CHECK(!level_event_should_fire(&g, &t, 120, 0x00000010u),
          "★回绕: 差值只有 0x110ms(272) < 60s -> 不出");
    CHECK(level_event_should_fire(&g, &t, 120, 0x00010000u),
          "★回绕: 差值(65536+256) > 60s -> 出");

    CHECK(!level_event_should_fire(NULL, &t, 100, 0u), "g=NULL -> false");
    CHECK(!level_event_should_fire(&g, NULL, 100, 0u), "t=NULL -> false");

    /* ================= [7] {level} 模板渲染(文档 3.5) ================= */
    printf("\n[7] {level} 模板渲染(替换为当前水位 cm 整数)\n");
    {
        char b[CFG_LEDTEXT_MAX + 1u];

        (void)memset(b, 'Z', sizeof(b));        /* 先填脏, 验证失败时会置空 */
        CHECK(level_render_template(b, (uint16_t)sizeof(b),
                                    "路面积水{level}CM 禁止通行", 123) > 0u,
              "含 {level} 的模板渲染成功");
        CHECK(strcmp(b, "路面积水12CM 禁止通行") == 0,
              "★ 123mm -> \"12\"(文档: 替换为 cm 整数) (实际 %s)", b);

        (void)level_render_template(b, (uint16_t)sizeof(b), "{level},{level}!", 85);
        /* ★2026-10-14 期望改了: 一位数要补一个空格(用户要求, LED/TTS 共用)。
         * 85mm -> cm=8 -> 一位数 -> "8 " ⇒ "{level},{level}!" 变成 "8 ,8 !" */
        CHECK(strcmp(b, "8 ,8 !") == 0,
              "★ 模板里多次 {level} 全部替换, 且一位数【补了空格】 (实际 %s)", b);

        (void)level_render_template(b, (uint16_t)sizeof(b), "无占位符", 999);
        CHECK(strcmp(b, "无占位符") == 0, "没有 {level} 时原样输出");

        (void)level_render_template(b, (uint16_t)sizeof(b), "高度{level}", 0);
        /* ★0 也算一位数 ⇒ 补空格("0 ") —— 用户 2026-10-14 明确"0 也补" */
        CHECK(strcmp(b, "高度0 ") == 0, "★ 0mm -> \"0 \"(0 也补空格)");

        /* ★两位数【不补】—— 与一位数形成对照, 防止将来改成"总是补" */
        (void)level_render_template(b, (uint16_t)sizeof(b), "高度{level}", 150);
        CHECK(strcmp(b, "高度15") == 0, "★ 150mm -> \"15\"(两位数不补空格)");

        (void)level_render_template(b, (uint16_t)sizeof(b), "{x}原样", 100);
        CHECK(strcmp(b, "{x}原样") == 0, "★ 不认识的花括号原样保留(不吞不改)");

        CHECK(level_render_template(b, 4u, "abcdefgh", 0) == 0u, "缓冲不够 -> 0");
        CHECK(b[0] == '\0', "★ 失败时 out 被置空(绝不发半截文本给 LED)");
        CHECK(level_render_template(NULL, 16u, "x", 0) == 0u, "out=NULL -> 0");
        CHECK(level_render_template(b, (uint16_t)sizeof(b), NULL, 0) == 0u, "tmpl=NULL -> 0");
    }

    /* ================= [8] 该不该下发(档位未变不重发 / 文本变了才下发) ================= */
    printf("\n[8] 下发判据(文档 4 章: 档位未变不重发, 文本实际变化才下发)\n");
    {
        level_dispatch_state_t d;

        level_dispatch_init(&d);
        CHECK(level_dispatch_should_send(&d, 1u, "正常10CM", ""), "从没发过 -> 要发");

        /* 下发成功, 记下来 */
        level_dispatch_mark(&d, 1u, "正常10CM", "");
        CHECK(!level_dispatch_should_send(&d, 1u, "正常10CM", ""),
              "★ 档位没变 + 文本没变 -> 不再下发(防重发)");
        CHECK(level_dispatch_should_send(&d, 2u, "正常10CM", ""),
              "★ 档位变了 -> 要发");
        CHECK(level_dispatch_should_send(&d, 1u, "正常15CM", ""),
              "★ 档位没变但文本变了(水位在档位内变化) -> 要发");

        /* 下发【失败】的语义: 不 mark -> 下一页仍然要发(文档: 失败则清除缓存下次重试) */
        level_dispatch_mark(&d, 3u, "告警20CM", "");
        CHECK(!level_dispatch_should_send(&d, 3u, "告警20CM", ""), "成功后同一份不再发");
        CHECK(level_dispatch_should_send(&d, 3u, "告警21CM", ""),
              "★ 文本变了(11mm-> 20CM vs 21CM) -> 要发");

        /* 边界: led_text 传 NULL 等价于空串 */
        level_dispatch_init(&d);
        level_dispatch_mark(&d, 0u, NULL, "");
        CHECK(!level_dispatch_should_send(&d, 0u, NULL, ""), "NULL 文本记过之后不再发");

        /* 超长文本不会越界(截断保存), 且不崩 */
        level_dispatch_mark(&d, 0u,
            "0123456789012345678901234567890123456789012345678901234567890123456789", "");
        CHECK(level_dispatch_should_send(&d, 0u, "短", "") == true, "超长文本存入后不崩");

        CHECK(!level_dispatch_should_send(NULL, 1u, "x", ""), "d=NULL -> false");
        level_dispatch_init(NULL);              /* 不崩 */
        level_dispatch_mark(NULL, 0u, "x", "");     /* 不崩 */
        CHECK(true, "NULL 参数不崩");
    }

    /* ====== [18] distance 换算纯函数 (⚠️ 固件已无调用点, 仅保边界覆盖) ====== */
    printf("\n[18] level_convert_mm: 纯函数边界(固件不调用, 换算已移交设备侧)\n");
    {
        /* level 模式: 传感器报的就是水深, 原样返回(不碰 installHeight/calib) */
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_LEVEL, 300u, 0, 1234u) == 1234,
              "level 模式 -> 原样透传");
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_LEVEL, 300u, 50, 1234u) == 1234,
              "★ level 模式即使配了 installHeight/calib 也不参与换算");

        /* 认不出的模式值(脏配置/老固件)按 level 处理 —— 不猜 */
        CHECK(level_convert_mm(99u, 300u, 0, 1234u) == 1234,
              "★ 未知 measureMode -> 按 level 透传");

        /* distance 模式: 水深 = installHeight(cm)*10 - 距离(mm) + calib(mm) */
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 300u, 0, 1500u) == 1500,
              "★ 300cm 装高, 测距 1500mm -> 水深 1500mm");
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 300u, 30, 1500u) == 1530,
              "★ 校准 +30mm -> 1530mm");
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 300u, -30, 1500u) == 1470,
              "★ 校准 -30mm -> 1470mm");

        /* 水面退到探头以下 / 探头装低了: 结果为负, 【不钳位】(配置 3.3、协议 §7.2) */
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 100u, 0, 1500u) == -500,
              "★ 装高 100cm 但测距 1500mm -> -500mm(允许负值, 不钳 0)");
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 0u, 0, 10u) == -10,
              "★ 装高 0 -> 直接取负");

        /* 极值不溢出: installHeight 上限 2000cm, 测距 0 / 65535 */
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 2000u, 5000, 0u) == 25000,
              "★ 上限 2000cm*10 + 5000mm = 25000mm, int32 装得下");
        CHECK(level_convert_mm((uint8_t)CFG_MEASURE_DISTANCE, 0u, -5000, 65535u) == -70535,
              "★ 下限 0 - 65535 - 5000 = -70535mm, 不溢出");

        /* {level} 取整改成向下取整 —— distance 模式会给出负值, 截断会少报 1cm */
        {
            char buf[32];

            (void)level_render_template(buf, sizeof(buf), "{level}CM", 1500);
            CHECK(strcmp(buf, "150CM") == 0, "{level} 正值照旧(实测 \"%s\")", buf);

            (void)level_render_template(buf, sizeof(buf), "{level}CM", 1234);
            CHECK(strcmp(buf, "123CM") == 0, "1234mm -> 123cm(向下)");

            (void)level_render_template(buf, sizeof(buf), "{level}CM", -5);
            CHECK(strcmp(buf, "-1CM") == 0,
                  "★ -5mm -> -1cm(向下取整; 截断会得 0, 比实际高一档) 实测 \"%s\"", buf);

            (void)level_render_template(buf, sizeof(buf), "{level}CM", -10);
            CHECK(strcmp(buf, "-1CM") == 0, "-10mm -> -1cm");

            (void)level_render_template(buf, sizeof(buf), "{level}CM", -11);
            CHECK(strcmp(buf, "-2CM") == 0, "-11mm -> -2cm");

            (void)level_render_template(buf, sizeof(buf), "{level}CM", 0);
            CHECK(strcmp(buf, "0 CM") == 0, "0mm -> 0cm(一位数仍补空格)");
        }
    }

    /* 档位动作组帧 + 目标选择(自治"最后一跳", 2026-09-30 第 2 条) */
    test_gear_payload();
    test_gear_targets();

    printf("\n==== 通过 %d, 失败 %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
