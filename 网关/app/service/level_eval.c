/**
 * @file  level_eval.c
 * @brief 水位评估纯函数实现(见 level_eval.h)
 *
 * 毕设版: 只有阈值判状态 + 有效水位。联动专属逻辑已全部移除。
 */
#include "level_eval.h"

/* 阈值是 cm, 水位是 mm -> 比较前统一乘 10。
 * alarmCmd 等是 uint16(最大 65535), *10 后 655350, int32 装得下, 不会溢出。 */
static int32_t lv_cm2mm(uint16_t cm)
{
    return (int32_t)cm * 10;
}

uint8_t level_state_to_gear(level_state_t st)
{
    switch (st)
    {
        case LEVEL_ST_RESOLVED: return 0u;
        case LEVEL_ST_NORMAL:   return 1u;
        case LEVEL_ST_WARNING:  return 2u;
        case LEVEL_ST_ALARM:    return 3u;
        default:                return 0u;   /* UNKNOWN 也按 0 处理(还没测到水位) */
    }
}

int32_t level_convert_mm(uint8_t measure_mode, uint16_t install_height_cm,
                         int16_t calib_offset_mm, uint16_t raw_mm)
{
    /* ⚠️ 2026-10-30: 本函数【已无调用点】—— 换算在液位设备侧完成。
     *    见 level_eval.h 的 @warning; 保留只为单测覆盖 + 将来接第三方传感器。 */

    /* level 模式(也是"没配置/认不出"的兜底): 传感器报的就是水深, 原样返回 */
    if (measure_mode != (uint8_t)CFG_MEASURE_DISTANCE)
    {
        return (int32_t)raw_mm;
    }

    /* distance 模式: 水深 = 安装高度 - 探头到水面的距离 + 校准偏移
     *   单位: install_height_cm 是 cm -> ×10 换成 mm; raw_mm / calib 本来就是 mm。
     * 【可以为负, 不钳位】(配置 3.3 / 协议 §7.2, 1027 联调定稿):
     *   探头到水面的距离超过安装高度(比如探头装低了、或水面退到探头以下)时,
     *   水深就是负的, 平台要看到真实值, 不能拿 0 冒充。
     *   取值范围: 0 - 65535 - 5000 mm ~ 20000 - 0 + 5000 mm, int32 装得下。
     * 【没有除法】全程整数, 不引浮点(本工程无 FPU 使用约定)。 */
    return (int32_t)install_height_cm * 10 - (int32_t)raw_mm + (int32_t)calib_offset_mm;
}

level_state_t level_state_of(int32_t level_mm, const cfg_thresholds_t *t)
{
    if (t == NULL)
    {
        return LEVEL_ST_UNKNOWN;             /* 没阈值就不猜 */
    }

    /* 顺序照文档 3.4 的表: >= alarm 先判, 然后是 (warning, alarm), 再 (resolve, warning], 最后 <= resolve */
    if (level_mm >= lv_cm2mm(t->alarm_cm))
    {
        return LEVEL_ST_ALARM;
    }
    if (level_mm > lv_cm2mm(t->warning_cm))
    {
        return LEVEL_ST_WARNING;
    }
    if (level_mm > lv_cm2mm(t->resolve_cm))
    {
        return LEVEL_ST_NORMAL;
    }
    return LEVEL_ST_RESOLVED;
}

bool level_effective(const int32_t *levels_mm, const uint8_t *select,
                     uint8_t n, int32_t *out_mm)
{
    uint8_t i;
    bool    got = false;
    int32_t best = 0;

    if ((levels_mm == NULL) || (select == NULL) || (out_mm == NULL) || (n == 0u))
    {
        return false;
    }

    for (i = 0u; i < n; i++)
    {
        if (select[i] == 0u)
        {
            continue;                       /* 离线或停用的不参与(文档 3.3 末) */
        }
        if (!got || (levels_mm[i] > best))
        {
            best = levels_mm[i];
            got  = true;
        }
    }

    if (!got)
    {
        return false;                       /* 一台有效的都没有 */
    }
    *out_mm = best;
    return true;
}
