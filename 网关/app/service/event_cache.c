/**
 * @file  event_cache.c
 * @brief 断线事件队列 —— 直接落在【外挂 Flash 的 EVENT 环】(★2026-11-11 方案 A)
 *
 * 语义见 event_cache.h。这里只做存储 + 合并，不碰 JSON、不碰网络。
 *
 * 【实现要点一：为什么不再有 RAM 环形】
 *   以前 g_ev[256] 是"热窗口"，满了才往 Flash 溢出，结果是补报顺序倒挂
 *   (Flash 装头段、RAM 装尾段，补报却从 RAM 队头开始)。现在单写 Flash，
 *   写入顺序 = 采集顺序 = 补报顺序，天然满足用户定的"严格按 ts 递增"。
 *
 * 【实现要点二：合并怎么做到不擦扇区】
 *   NOR 只能 1→0，想"就地改写一条旧记录"必须先擦整个 4 KB，那会连带擦掉
 *   同扇区里另外 63 条还能用的记录。所以合并【不覆盖旧的】，而是：
 *     · 把可合并的记录先攥在 RAM 里（s_pend，只占 28 B），
 *     · 同一台设备同一种持续状态再来一条 -> 直接替换 RAM 里这一条；
 *     · 直到"来了别的记录"或"1 s 到点"，才把它写进 Flash。
 *   这样每条持续状态在环里只占一个槽，与原来的"就地覆写"效果一致，
 *   而且**永远不会为了改写去擦扇区**。
 *
 *   【顺序会不会乱】不会。s_pend 永远是"最后一条"，它的 ts 每次刷新都会变新；
 *   提交时机也永远在"下一条记录之前"，所以落盘顺序的 ts 单调不减。
 *
 *   【掉电丢什么】最多丢 s_pend 这一条（28 B / 1 条），且它本身就是"重复了
 *   很多次的同一个状态"，丢了下次上报会再来一条。
 */
#include "event_cache.h"
#include "xflash_store.h"
#include "bsp_tim.h"        /* get_tick_ms —— 待提交槽的到点提交 */
#include <string.h>

/* 2026-10-18: UID 尺寸静态断言 —— 防"改长度忘改缓冲"(现场踩过, 见 lora_business.c 的说明) */
_Static_assert(sizeof(((ec_record_t *)0)->uid) == 13u,
               "ec_record_t.uid 必须 13 字节");
/* 单槽容量兜底: 12 B 头 + 52 B 负载；28 B 的记录还有余量给将来加字段 */
_Static_assert(sizeof(ec_record_t) <= (XF_SLOT_SIZE - XF_REC_HDR_SIZE),
               "ec_record_t 超过环的单槽负载上限(需要改 XF_SLOT_SIZE 或分区)");

/** 待提交槽最多攥多久（ms）。到点就写进 Flash，免得掉电丢掉"最新状态" */
#define EC_PEND_HOLD_MS   1000u

static ec_record_t s_pend;          /* 可合并记录的暂存（永远是"最后一条"） */
static uint32_t    s_pend_ms;       /* 它被最后一次刷新的时刻 */
static bool        s_pend_valid;
static uint32_t    s_fail;          /* 写失败累计（与环满淘汰合并对外报） */

/* ---------------- 内部：待提交槽的提交 ---------------- */

static void ec_commit_pending(void)
{
    if (!s_pend_valid)
    {
        return;
    }
    if (!xf_ring_push(XF_RING_EVENT, &s_pend, (uint16_t)sizeof(s_pend)))
    {
        /* Flash 不在 / 擦写失败：这条真丢了。不重试、不阻塞（同 telemetry_cache）。 */
        s_fail++;
    }
    s_pend_valid = false;
}

/* ---------------- 对外 ---------------- */

void event_cache_reset(void)
{
    s_pend_valid = false;
    s_pend_ms    = 0u;
    s_fail       = 0u;
    (void)xf_ring_clear(XF_RING_EVENT);
}

void event_cache_poll(void)
{
    if (!s_pend_valid)
    {
        return;
    }
    if ((uint32_t)(get_tick_ms() - s_pend_ms) < EC_PEND_HOLD_MS)
    {
        return;
    }
    ec_commit_pending();
}

bool event_cache_push(const ec_record_t *rec)
{
    if (rec == NULL)
    {
        return false;
    }
    /* 只认这三种, 免得存进去一堆渲染层没法处理的记录 */
    if ((rec->kind != (uint8_t)EC_KIND_STATUS_CHANGE) &&
        (rec->kind != (uint8_t)EC_KIND_ACTION) &&
        (rec->kind != (uint8_t)EC_KIND_DEVICE))
    {
        return false;
    }

    /* ★先把待提交的那条落盘★ —— 否则它会被后来的记录挤到后面去，顺序就乱了 */
    ec_commit_pending();

    if (!xf_ring_push(XF_RING_EVENT, rec, (uint16_t)sizeof(*rec)))
    {
        s_fail++;
        return false;
    }
    return true;
}

bool event_cache_peek(ec_record_t *out)
{
    uint16_t len = 0u;

    if (out == NULL)
    {
        return false;
    }

    if (xf_ring_count(XF_RING_EVENT) != 0u)
    {
        if (!xf_ring_peek(XF_RING_EVENT, out, (uint16_t)sizeof(*out), &len))
        {
            return false;
        }
        if (len != (uint16_t)sizeof(*out))
        {
            (void)xf_ring_drop_n(XF_RING_EVENT, 1u);    /* 尺寸变过的旧记录: 丢掉 */
            return false;
        }
        return true;
    }

    if (s_pend_valid)
    {
        *out = s_pend;
        return true;
    }
    return false;
}

void event_cache_drop(void)
{
    if (xf_ring_count(XF_RING_EVENT) != 0u)
    {
        (void)xf_ring_drop_n(XF_RING_EVENT, 1u);
        return;
    }
    s_pend_valid = false;           /* 环空了才轮到待提交那条 */
}

bool event_cache_pop(ec_record_t *out)
{
    if (!event_cache_peek(out))
    {
        return false;
    }
    event_cache_drop();
    return true;
}

uint16_t event_cache_count(void)
{
    uint32_t n = xf_ring_count(XF_RING_EVENT);

    if (s_pend_valid) { n++; }
    return (uint16_t)n;
}

uint32_t event_cache_dropped(void)
{
    return xf_ring_dropped(XF_RING_EVENT) + s_fail;
}

uint16_t event_cache_capacity(void)
{
    const xf_region_info_t *ri = xf_region_info(XF_REGION_EVENT);

    if ((ri == NULL) || (!xf_store_ready())) { return 0u; }
    return (uint16_t)(ri->size / XF_SLOT_SIZE);
}

bool event_cache_peek_at(uint16_t idx, ec_record_t *out)
{
    uint16_t len = 0u;
    uint32_t cnt;

    if (out == NULL)
    {
        return false;
    }

    cnt = xf_ring_count(XF_RING_EVENT);

    if (idx < (uint16_t)cnt)
    {
        if (!xf_ring_peek_at(XF_RING_EVENT, idx, out, (uint16_t)sizeof(*out), &len))
        {
            return false;
        }
        return (len == (uint16_t)sizeof(*out)) ? true : false;
    }

    if ((idx == (uint16_t)cnt) && s_pend_valid)
    {
        *out = s_pend;
        return true;
    }
    return false;
}

ec_push_result_t event_cache_push_latest(const ec_record_t *rec)
{
    bool mergeable;

    if (rec == NULL)
    {
        return EC_PUSH_FAIL;
    }

    /* 只有"持续状态"型的设备事件才合并（理由见 event_cache.h 的长注释：
     * bound/unbound 必须成对、online 必须成序，合并会让平台看不出"掉线又恢复"）。 */
    mergeable = (rec->kind == (uint8_t)EC_KIND_DEVICE) &&
                ((rec->a == (uint8_t)EC_DEV_EV_LOW_VOLTAGE) ||
                 (rec->a == (uint8_t)EC_DEV_EV_OFFLINE));

    if (!mergeable)
    {
        return event_cache_push(rec) ? EC_PUSH_ADDED : EC_PUSH_FAIL;
    }

    /* ★合并 = 替换 RAM 里的待提交槽★
     * 待提交槽按定义就是"环里最后一条"，所以这里天然满足 OFFLINE 那条约束
     * (只有它是最后一条时才合并 —— 前面已有 online 说明中间恢复过, 那是一次
     *  新的掉线, 必须追加)。而"追加"在这里的表现就是：先把旧的提交，再接管。 */
    if (s_pend_valid && (s_pend.kind == rec->kind) && (s_pend.a == rec->a) &&
        (memcmp(s_pend.uid, rec->uid, sizeof(rec->uid)) == 0))
    {
        s_pend     = *rec;
        s_pend_ms  = get_tick_ms();
        return EC_PUSH_MERGED;
    }

    if (s_pend_valid)
    {
        /* 待提交的是【另一台设备的另一种状态】，不是这一次的重复 ——
         * 必须先把它写进环，否则下面的 s_pend = *rec 会把它覆盖掉（丢一条）。
         * 提交后顺序仍然正确：它的 ts 不晚于新来的这条。 */
        ec_commit_pending();
    }

    s_pend       = *rec;
    s_pend_ms    = get_tick_ms();
    s_pend_valid = true;
    return EC_PUSH_ADDED;
}

int32_t ec_action_code_of(const ec_record_t *rec)
{
    /* 只有动作记录才有"执行码"语义; 其它 kind 调用它是逻辑错误, 返回 0 并让
     * 上层的 validate() 去拦(这里不 assert, 本工程不带 assert)。 */
    return (rec != NULL) ? rec->aux : 0;
}

void ec_action_set_code(ec_record_t *rec, int32_t code)
{
    if (rec != NULL)
    {
        rec->aux = code;
    }
}

uint8_t event_cache_group_at(uint16_t idx, ec_action_pair_t *out, uint8_t max_pairs)
{
    ec_record_t head;
    ec_record_t cur;
    uint8_t     seq;
    uint8_t     n = 0u;

    if ((out == NULL) || (max_pairs == 0u))
    {
        return 0u;
    }
    if (!event_cache_peek_at(idx, &head))
    {
        return 0u;
    }
    if ((head.kind != (uint8_t)EC_KIND_ACTION) || (head.seq == 0u))
    {
        return 0u;              /* 这一条不是"成组"的动作记录 */
    }
    seq = head.seq;

    while (n < max_pairs)
    {
        if (!event_cache_peek_at((uint16_t)(idx + n), &cur))
        {
            break;
        }
        if ((cur.kind != (uint8_t)EC_KIND_ACTION) || (cur.seq != seq))
        {
            break;
        }
        (void)memcpy(out[n].uid, cur.uid, sizeof(out[n].uid));
        out[n].code = ec_action_code_of(&cur);
        n++;
    }

    /* ★ 本组比 out 能装的大: 返回 0, 而不是只报前几台 ★
     * 只报前几台会渲染出一条"少报了几台设备"的动作事件 —— 看起来完全正常。 */
    if (n == max_pairs)
    {
        if (event_cache_peek_at((uint16_t)(idx + n), &cur) &&
            (cur.kind == (uint8_t)EC_KIND_ACTION) && (cur.seq == seq))
        {
            return 0u;
        }
    }

    return n;
}

uint8_t event_cache_peek_action_group(ec_action_pair_t *out, uint8_t max_pairs)
{
    return event_cache_group_at(0u, out, max_pairs);        /* 只是"从队头找"的特例 */
}

uint16_t event_cache_page_count(uint16_t max_events)
{
    uint16_t    total;
    ec_record_t tail;
    uint16_t    n;

    total = event_cache_count();            /* 含待提交那条 */
    if ((total == 0u) || (max_events == 0u))
    {
        return 0u;
    }
    if (max_events >= total)
    {
        return total;       /* 一次就能全取走: 不存在"组被切断"的问题 */
    }

    /* 页里包含待提交那条时，它不可能是动作组的一部分（动作组只走 push）——
     * 直接按 max_events 返回即可。 */
    if (max_events > xf_ring_count(XF_RING_EVENT))
    {
        return max_events;
    }

    if (!event_cache_peek_at((uint16_t)(max_events - 1u), &tail))
    {
        return max_events;
    }

    /* 只有当"页尾那条"属于某个动作组时, 才需要把这一组补完 */
    if ((tail.kind == (uint8_t)EC_KIND_ACTION) && (tail.seq != 0u))
    {
        ec_record_t cur;

        n = max_events;
        while (n < total)
        {
            if (!event_cache_peek_at(n, &cur))
            {
                break;
            }
            if ((cur.kind != (uint8_t)EC_KIND_ACTION) || (cur.seq != tail.seq))
            {
                break;
            }
            n++;
        }
        /* 若整个组本身就比 max_events 大(从队头就是这个 seq), 上面会一路加到组尾,
         * 返回值自然 > max_events —— 这是刻意的(见头文件说明: 宁可超限也不能卡住)。 */
        return n;
    }

    return max_events;
}

bool event_cache_validate(const ec_record_t *rec)
{
    if (rec == NULL)
    {
        return false;
    }

    switch (rec->kind)
    {
        case (uint8_t)EC_KIND_STATUS_CHANGE:
            /* from/to 都必须是文档 3.4 的四个状态之一 */
            return (((rec->a >= (uint8_t)EC_ST_RESOLVED) && (rec->a <= (uint8_t)EC_ST_ALARM) &&
                     (rec->b >= (uint8_t)EC_ST_RESOLVED) && (rec->b <= (uint8_t)EC_ST_ALARM)))
                   ? true : false;

        case (uint8_t)EC_KIND_ACTION:
            /* a = 档位 0~3(0 合法); b = 触发来源 */
            return (((rec->a <= 3u) &&
                     (rec->b >= (uint8_t)EC_TRIG_AUTO) && (rec->b <= (uint8_t)EC_TRIG_FAILSAFE)))
                   ? true : false;

        case (uint8_t)EC_KIND_DEVICE:
            /* a = 设备事件码; b = 设备类型 */
            return (((rec->a >= (uint8_t)EC_DEV_EV_BOUND) &&
                     (rec->a <= (uint8_t)EC_DEV_EV_LOW_VOLTAGE) &&
                     ((rec->b == (uint8_t)EC_DEV_SENSOR) || (rec->b == (uint8_t)EC_DEV_DEVICE))))
                   ? true : false;

        default:
            return false;               /* kind 不认识 */
    }
}
