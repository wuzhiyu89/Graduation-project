/**
 * @file  app_state.h
 * @brief 编排层共享的运行时状态：时钟、最近一次水位评估、周期换算
 *
 * 这些量被"上报 / 下行 / 主循环 / 控制台"多处读取，但只由一个地方写入：
 *   · app_refresh_epoch()  由主循环/下行在需要时刷新（自带 10 分钟节流）
 *   · app_note_level()     只由水位上报路径在拿到有效水位时调用
 *
 * 放在单独模块，是为了让 app_uplink / app_console / app_downlink 不必
 * include app_main.c（那会重新变成"什么都依赖 main"）。
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 当前 Unix 秒（模组时间外推；问不到时退回开机秒数） */
uint32_t app_now_unix(void);

/** 必要时向 4G 模组对时（内部 10 分钟节流，非阻塞窗口很短） */
void app_refresh_epoch(void);

/** 让下一次 app_refresh_epoch() 强制真正去问模组（sys.time_sync 用） */
void app_invalidate_epoch(void);

/** 当前是否已拿到过模组时间 */
bool app_epoch_valid(void);

/** 配置里的"秒"换算成毫秒；0 或 >3600 时用 fallback_ms */
uint32_t app_cfg_period_ms(uint16_t sec, uint32_t fallback_ms);

/**
 * 记下最近一次有效水位算出的状态与档位（给 status/telemetry 打标签用）。
 * state 是 level_state_t 数值(1..4)，gear 是 0..3；由水位上报路径调用。
 */
void app_note_level(uint8_t state, uint8_t gear);

/** 当前实时档位（还没有过有效评估时返回 0） */
uint8_t app_live_gear(void);

/** 当前实时状态码（level_state_t；没有有效评估时返回 0） */
uint8_t app_live_state(void);

/** 当前实时状态名（"Alarm"/"Warning"/"Normal"/"Resolved"；无评估时 "Normal"） */
const char *app_live_state_name(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */
