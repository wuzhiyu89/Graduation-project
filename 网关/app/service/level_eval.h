/**
 * @file  level_eval.h
 * @brief 水位评估【纯函数】: 阈值判状态 + 有效水位
 *
 * 毕设版: 只有液位传感器, 没有一体机/声光联动设备。
 *   本模块只保留给 status/telemetry 报文打状态标签所需的纯函数:
 *   阈值 -> 状态(Normal/Warning/Alarm/Resolved) -> 档位映射, 以及有效水位(取max)。
 *   防抖 / 事件去抖 / 模板渲染 / 档位下发载荷 / fail-safe / 目标选择等
 *   联动专属逻辑已全部移除。
 *
 * 【单位约定: 一律用 mm(整数)】配置阈值是 cm, 比较时 cm*10, 不用浮点。
 *
 * 【状态边界(文档 3.4)】
 *   Alarm    level >= alarmCm                 -> 档位 3
 *   Warning  warningCm < level <  alarmCm     -> 档位 2
 *   Normal   resolveCm < level <= warningCm   -> 档位 1
 *   Resolved level <= resolveCm               -> 档位 0
 */
#ifndef __LEVEL_EVAL_H__
#define __LEVEL_EVAL_H__

#include <stdint.h>
#include <stdbool.h>
#include "config_store.h"       /* 只用 cfg_thresholds_t 这个类型, 不调它的函数 */

#ifdef __cplusplus
extern "C" {
#endif

/* 文档 3.4 的四个状态 + 一个"还没测到过任何水位"的初值 */
typedef enum
{
    LEVEL_ST_UNKNOWN  = 0,      /* 上电至今没有过有效水位 */
    LEVEL_ST_RESOLVED = 1,      /* <= resolveCm  -> 档位 0 */
    LEVEL_ST_NORMAL   = 2,      /* (resolve, warning] -> 档位 1 */
    LEVEL_ST_WARNING  = 3,      /* (warning, alarm)   -> 档位 2 */
    LEVEL_ST_ALARM    = 4       /* >= alarmCm          -> 档位 3 */
} level_state_t;

/** 文档 3.4 的状态 -> 档位映射(固定, 不可配) */
uint8_t level_state_to_gear(level_state_t st);

/**
 * @brief 把传感器上报值换算成【水深(mm)】—— 纯函数, ⚠️ 当前固件【不调用】⚠️
 *
 * @param measure_mode      cfg_measure_mode_t; 非 distance 一律按 level 处理(兜底不猜)
 * @param install_height_cm 探头安装高度(cm), 仅 distance 模式用
 * @param calib_offset_mm   校准偏移(mm), 加在换算结果上
 * @param raw_mm            传感器上报的原始值(mm)
 * @return 水深(mm)。**distance 模式可为负, 不钳位**
 *
 * @warning ★2026-10-30 起网关【不再做换算】★
 *   液位传感器固件自己实现了 measureMode / installHeightCm / calibOffsetMm
 *   (0x01 帧 [6] / [7-8] / [9-10]), 并在设备内部换算, 0x02 帧报上来的已经
 *   是液位值。网关若再算一次 = 重复扣减安装高度, distance 模式下水位系统性偏低。
 *   所以 `data_cache_up_push()` 已改成原值透传, 本函数【没有任何调用点】。
 *   保留它只为: ① 单测仍覆盖换算边界(负值/校准/未配置); ② 万一将来要接
 *   不做换算的第三方传感器, 逻辑不用重写。**新增代码不要再调它。**
 *
 * @note 【口径】level 模式原样返回 —— 传感器报的就是水深, 网关不做二次加工。
 */
int32_t level_convert_mm(uint8_t measure_mode, uint16_t install_height_cm,
                         int16_t calib_offset_mm, uint16_t raw_mm);

/**
 * @brief 水位(mm) -> 候选状态(文档 3.4 的表)
 * @note  t == NULL 时返回 LEVEL_ST_UNKNOWN(不猜)
 */
level_state_t level_state_of(int32_t level_mm, const cfg_thresholds_t *t);

/**
 * @brief 有效水位 = 所有"在线且 enabled"的传感器里取最大值(文档 3.3 末)
 * @param levels_mm 每台的水位(mm)
 * @param select    每台是否参与(1 = 在线且 enabled) —— 【把"在线/enabled"的判断
 *                  留给调用方】, 因为在线状态在 LoRa 层、enabled 在配置里,
 *                  本模块不该去碰它们
 * @param n         台数
 * @param out_mm    输出: 有效水位(mm)
 * @return true = 至少有一台参与(此时有效水位才有意义); false = 一台都没有
 * @note  文档 5 章"传感器全部失联"的 fail-safe 由调用方按 rules.sensorOfflinePolicy
 *        处理, 本函数只回答"有没有有效水位"。
 */
bool level_effective(const int32_t *levels_mm, const uint8_t *select,
                     uint8_t n, int32_t *out_mm);

#ifdef __cplusplus
}
#endif

#endif /* __LEVEL_EVAL_H__ */
