/**
 * \file  test_level.c
 * \brief 边缘水位评估纯逻辑的主机单测(见 app/service/level_eval.h)
 *
 * 这一组测试的价值在于【边界等号】: 文档 3.4 的表里
 *   Warning 是 warningCm < level <  alarmCm  (两头都是开区间)
 *   Normal  是 resolveCm < level <= warningCm(右边是闭区间!)
 *   Resolved 是 level <= resolveCm
 * 少一个等号, 现象是"水位正好停在阈值上时档位不对" —— 现场极难复现, 只在主机上能一次测完。
 *
 * 毕设版: level_eval 只剩纯函数(状态标签/有效水位/换算), 档组帧、模板渲染、
 * 防抖、事件去抖、dispatch、fail-safe、目标选择等联动专属逻辑已全部移除,
 * 对应的测试用例也随之删除。
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

int main(void)
{
    cfg_thresholds_t t = make_t();
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

    /* ====== [18] distance 换算纯函数 (⚠️ 固件已无调用点, 仅保边界覆盖) ====== */
    printf("\n[18] level_convert_mm: 纯函数边界(固件不调用, 换算已移交设备侧)\n");
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

    printf("\n==== 通过 %d, 失败 %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
