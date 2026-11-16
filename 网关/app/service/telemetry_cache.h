/**
 * @file  telemetry_cache.h
 * @brief 离线期间的【逐条液位上报】队列 —— 重连后按"当时的时间戳"补报
 *
 * ============================================================================
 * 【它存什么】一条 = 某台液位传感器【一次 0x02 上报】，不是 30 秒聚合遥测。
 *   用户 2026-10-18 明确: "离线后补报的应该是每次传感器上报的水位,
 *   而不是我们 30s 的遥测"。
 *   ⇒ 所以每条记录是"一台、一次、一个水位值", 补报时一条记录发一条
 *     telemetry.report(sensors[] 里就那一个元素)。
 *
 * 【为什么要它】4G 掉线时遥测发不出去, 而水位还在照常上报(LoRa 与 4G 独立)。
 *   以前这些数据【直接丢掉了】。现在: 发不出去就进队列, 重连后逐条补发。
 *
 * ============================================================================
 * ★2026-11-11: 存储从"RAM 环 + 满时溢出 Flash"改成【外挂 Flash 单写】★
 *   用户拍板方案 A(Flash 就是队列), 四条口径:
 *     Q1 在线正常时不写 Flash —— 只有"发不出去"的记录才入队(见 app_on_level_report);
 *     Q2 补报严格按 ts 递增 —— 单写天然满足(写入顺序 = 采集顺序);
 *     Q3 不考虑老板子 —— 不再保留 RAM 后端, Flash 不可用时直接计入 dropped;
 *     Q4 事件与遥测各一个环 —— 不合并。
 *
 *   【改之前是什么样】
 *     RAM 有 128 条热窗口(g_tc[], 4 KB), 满了才把【最旧】一条溢出到 Flash。
 *     于是 Flash 装的是头段、RAM 装的是尾段, 而补报是从 RAM 队头开始走 ——
 *     实测 200 条的补报顺序是 73..200 再 1..72, 最后一条落到平台的是两小时前的值。
 *     而且"刚断网、RAM 还没满"恰恰是最需要保护的时刻, 那时一条都没进 Flash。
 *
 *   【改之后】
 *     push 直接写 TELEM 环, 一条记录 64 B 槽, 3 MB ≈ 49,152 条
 *     (4 台 × 300 s ⇒ 约 204 天)。掉电最多丢最后一拍(见 xf_ring_poll 的说明),
 *     且 tail 落后导致的重复补报有确定性 msgId 兜底, 平台按 msgId 去重即可。
 *
 * 【时间戳口径】(用户 2026-10-18 要求)
 *   rec.ts = 网关【收到】这一帧上报时的 Unix 秒 ⇒ 补报时原样使用,
 *   **不是**补报那一刻的时间。信封里的 ts 与 sensors[].ts 都用它。
 *   (前提: 绑定设备必须云端下发命令 ⇒ 4G 至少通过一次 ⇒ epoch 有效 —— 用户确认。)
 * ============================================================================
 */
#ifndef __TELEMETRY_CACHE_H__
#define __TELEMETRY_CACHE_H__

#include <stdint.h>
#include <stdbool.h>

/** UID 字节数(与 LORA_UID_BYTES / CFG_UID_BYTES 一致, 由 lora_business.c 的静态断言盯着) */
#define TELEMETRY_UID_BYTES   13U

/**
 * 一条"液位上报"记录。
 * 布局刻意把 4 字节字段放前面, 保持 4 字节对齐; 合计 32 B
 * (★2026-10-28(A1): level_mm 从 uint16 改成 int32, 并新增 raw_mm★)。
 * ⚠️ 结构体一改, 环里旧记录的尺寸就对不上 —— xf_ring 的 64 B 槽能装到 52 B,
 *    所以"加字段"还有余量; 真要超了必须改分区/槽大小, 别硬塞。
 */
typedef struct
{
    uint32_t ts;                              /* 收到该上报时的 Unix 秒(补报不改) */
    int32_t  level_mm;                        /* 该次上报的【水深】(mm) —— distance 模式可为负 */
    uint16_t raw_mm;                          /* 传感器【原始上报值】(mm), 补报的 rawMm 要用 */
    uint16_t msg_seq;                         /* 补报时还原 msgId 的 3 位序号 */
    uint8_t  uid[TELEMETRY_UID_BYTES];        /* 哪台设备 */
    uint8_t  bat_pct;                         /* 电量百分比 */
    int8_t   rssi_dbm;                        /* 该帧的 LoRa RSSI */
    uint8_t  state;                           /* 快照时刻的 level_state_t 码值 */
    uint8_t  gear;                            /* 快照时刻的档位 0~3 */
} tc_record_t;

/**
 * @brief 清空队列（恢复出厂 / 换绑）
 * @note  只把环的 head/tail 归零并落 META，不擦数据区（NOR 归零即等于空）。
 *        外挂 Flash 不可用时是空操作。
 */
void     telemetry_cache_reset(void);

/**
 * @brief 追加一条记录
 * @param rec 不可为 NULL
 * @return true = 已写入 Flash；false = 参数非法 / Flash 不可用 / 写失败
 * @note  写失败会计入 telemetry_cache_dropped()（可观察，不静默丢）。
 *        环满时由 xf_ring_push 淘汰最旧一条并计入同一个 dropped。
 */
bool     telemetry_cache_push(const tc_record_t *rec);

/** 当前条数（0 表示没有待补报的） */
uint16_t telemetry_cache_count(void);

/** 容量（= TELEM 环的槽数；外挂 Flash 不可用时为 0） */
uint16_t telemetry_cache_capacity(void);

/** 因"环满淘汰"或"写失败"丢掉的累计条数（现场判断离线多久会丢历史） */
uint32_t telemetry_cache_dropped(void);

/**
 * @brief 看一眼最旧的一条（不出队）
 * @return 指向模块内部静态缓冲的指针；空 -> NULL
 * @note  ★指针只在"下一次调用本模块之前"有效★ —— 数据是从 Flash 现读进静态
 *        缓冲的（Flash 里没法直接给指针）。调用方要留就自己拷一份。
 */
const tc_record_t *telemetry_cache_peek(void);

/** 丢掉最旧的一条（补报成功、或确认渲染不出来时调）；空时安全无操作 */
void     telemetry_cache_drop(void);

#endif /* __TELEMETRY_CACHE_H__ */
