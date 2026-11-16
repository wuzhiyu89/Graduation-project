/**
 * @file  telemetry_cache.c
 * @brief 离线期间的逐条液位上报队列 —— 直接落在【外挂 Flash 的 TELEM 环】
 *
 * 语义见 telemetry_cache.h 的完整说明。这里只做三件事:
 *   ① 把记录放进环 / 从环里取最旧一条 / 出队;
 *   ② 把"环不可用或写失败"如实计成 dropped（可观察，不静默丢）;
 *   ③ 给上层一个"看一眼队头"的稳定接口（Flash 读进静态缓冲）。
 *
 * 不碰网络、不碰 JSON、不碰配置 —— 这样主机上能直接测边界。
 */
#include "telemetry_cache.h"
#include "xflash_store.h"
#include <stddef.h>      /* NULL —— <stdint.h>/<stdbool.h> 不带它 */

/* 2026-10-18: UID 尺寸静态断言 —— 防"改长度忘改缓冲"
 * (现场踩过: g_reply_dev 声明 3 字节却 memcpy 13 字节, 见 lora_business.c 的说明) */
_Static_assert(sizeof(((tc_record_t *)0)->uid) == TELEMETRY_UID_BYTES,
               "tc_record_t.uid 必须等于 TELEMETRY_UID_BYTES");

/* 单槽容量兜底: 记录必须装进 64 B 槽(12 B 头 + 52 B 负载)，否则环直接写失败 */
_Static_assert(sizeof(tc_record_t) <= (XF_SLOT_SIZE - XF_REC_HDR_SIZE),
               "tc_record_t 超过环的单槽负载上限(需要改 XF_SLOT_SIZE 或分区)");

static tc_record_t s_peek;        /* peek() 的返回缓冲（Flash 现读） */
static uint32_t    s_fail;        /* 写失败累计（与环满淘汰合并对外报） */

void telemetry_cache_reset(void)
{
    s_fail = 0u;
    (void)xf_ring_clear(XF_RING_TELEM);
}

uint16_t telemetry_cache_capacity(void)
{
    const xf_region_info_t *ri = xf_region_info(XF_REGION_TELEM);

    if ((ri == NULL) || (!xf_store_ready())) { return 0u; }
    return (uint16_t)(ri->size / XF_SLOT_SIZE);
}

uint16_t telemetry_cache_count(void)
{
    return (uint16_t)xf_ring_count(XF_RING_TELEM);
}

uint32_t telemetry_cache_dropped(void)
{
    return xf_ring_dropped(XF_RING_TELEM) + s_fail;
}

bool telemetry_cache_push(const tc_record_t *rec)
{
    if (rec == NULL)
    {
        return false;
    }

    if (!xf_ring_push(XF_RING_TELEM, rec, (uint16_t)sizeof(*rec)))
    {
        /* Flash 不在 / 擦写失败: 这条真丢了。
         * ★这里不重试、不阻塞★ —— 补报路径在主循环里, 卡住它比丢一条更糟;
         * 现场靠 dropped 计数与日志判断"这块板子的 Flash 是不是坏了"。 */
        s_fail++;
        return false;
    }
    return true;
}

const tc_record_t *telemetry_cache_peek(void)
{
    uint16_t len = 0u;

    if (!xf_ring_peek(XF_RING_TELEM, &s_peek, (uint16_t)sizeof(s_peek), &len))
    {
        return NULL;
    }
    if (len != (uint16_t)sizeof(s_peek))
    {
        /* 尺寸对不上 = 上一版固件留下的记录（结构体改过）。
         * 丢掉它，否则队头会永远堵在这条上（补报渲染出来也是错的）。 */
        (void)xf_ring_drop_n(XF_RING_TELEM, 1u);
        return NULL;
    }
    return &s_peek;
}

void telemetry_cache_drop(void)
{
    (void)xf_ring_drop_n(XF_RING_TELEM, 1u);
}
