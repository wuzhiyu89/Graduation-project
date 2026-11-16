/**
 * \file  test_event_cache.c
 * \brief 断线事件队列的主机单测（外挂 Flash 后端 + 待提交槽合并）
 *        (见 app/service/event_cache.h)
 *
 * 为什么单独一个套件:
 *   2026-11-11 把存储从"RAM 环 256 条 + 溢出 Flash"改成"Flash 单写"，
 *   合并方式也随之从"就地覆写"改成"待提交槽"。这两件事的坑都在边界上：
 *     · 合并不当 -> 平台看到几十条一模一样的 offline（G2 现场问题）；
 *     · 提交时机不对 -> 补报顺序倒挂（正是改造前的老毛病）；
 *     · 分页/分组切错 -> 动作事件少报几台设备，看起来完全正常。
 *   这些在板子上要断网几小时才复现，主机上一次跑完。
 *
 * 重点验六件:
 *   1) 顺序: 出队严格按写入顺序（= ts 递增）；
 *   2) 合并: 同一设备同一种持续状态只留最新一条，且不破坏前后顺序；
 *   3) 到点: 1 s 内没有同 key 再来，待提交槽必须自己落盘（不然掉电就丢）；
 *   4) 分页: 一页不把动作组切两半；组比页大时整组返回（宁可超限也不卡住）；
 *   5) 分组: results[] 收齐 (uid, 执行码)，装不下返回 0 而不是截断；
 *   6) 边界: 空队列 / reset / 非法 kind。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "event_cache.h"
#include "xflash_store.h"

/* host_stub.c 提供的时基控制（没有头文件，按工程惯例 extern 声明） */
void host_tick_set(uint32_t ms);
void host_tick_advance(uint32_t ms);

/* fake_xflash.c 提供的测试钩子 */
void fx_blank(void);

static int g_fail;
static int g_pass;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) { g_pass++; printf("  ok   : "); }             \
        else      { g_fail++; printf("  FAIL : "); }             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    } while (0)

static void setup(void)
{
    host_tick_set(1000u);
    fx_blank();
    (void)xf_store_init();
    (void)xf_ring_format_all();
    event_cache_reset();
}

static void mk_uid(uint8_t uid[13], uint8_t tail)
{
    (void)memset(uid, 0, 13u);
    uid[0]  = 0x21u;
    uid[1]  = 0x00u;
    uid[12] = tail;
}

/** 造一条设备事件（低电量 / 离线这类"持续状态"） */
static void mk_dev(ec_record_t *r, uint8_t ev, uint8_t dev_tail, uint32_t ts)
{
    (void)memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)EC_KIND_DEVICE;
    mk_uid(r->uid, dev_tail);
    r->a    = ev;
    r->b    = (uint8_t)EC_DEV_SENSOR;
    r->ts   = ts;
}

/** 造一条状态迁移事件（不可合并） */
static void mk_status(ec_record_t *r, uint8_t to, uint8_t from, uint32_t ts)
{
    (void)memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)EC_KIND_STATUS_CHANGE;
    r->a    = to;
    r->b    = from;
    r->ts   = ts;
}

/** 造一条动作记录（按 seq 分组） */
static void mk_action(ec_record_t *r, uint8_t seq, uint8_t dev_tail, int32_t code, uint32_t ts)
{
    (void)memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)EC_KIND_ACTION;
    mk_uid(r->uid, dev_tail);
    r->a    = 2u;                        /* 档位 */
    r->b    = (uint8_t)EC_TRIG_AUTO;
    r->seq  = seq;
    r->ts   = ts;
    ec_action_set_code(r, code);
}

/* ---------------- 1) 基本顺序 ---------------- */
static void test_order(void)
{
    ec_record_t r;
    ec_record_t out;
    uint16_t    i;

    printf("\n--- 顺序: 严格按写入顺序(ts 递增) ---\n");
    setup();

    for (i = 0u; i < 20u; i++)
    {
        mk_status(&r, (uint8_t)EC_ST_ALARM, (uint8_t)EC_ST_NORMAL, 1000u + i);
        (void)event_cache_push(&r);
    }
    CHECK(event_cache_count() == 20u, "push 20 条 -> count 20");

    for (i = 0u; i < 20u; i++)
    {
        CHECK(event_cache_peek_at(i, &out) && (out.ts == (1000u + i)),
              "第 %u 条 ts = %u(实际 %u)", (unsigned)i, 1000u + i, (unsigned)out.ts);
    }

    CHECK(event_cache_peek(&out) && (out.ts == 1000u), "peek 给最旧那条");
    event_cache_drop();
    CHECK(event_cache_peek(&out) && (out.ts == 1001u), "drop 后队头前进");

    /* 全部出队 + drop，队尾也要能干净返回 false */
    while (event_cache_pop(&out)) { }
    CHECK(event_cache_count() == 0u, "pop 干净后 count 0");
    CHECK(!event_cache_peek(&out), "空队列 peek -> false");
}

/* ---------------- 2) 合并：同一设备同一种持续状态 ---------------- */
static void test_merge(void)
{
    ec_record_t r;
    ec_record_t out;

    printf("\n--- 合并: 持续状态只留最新一条 ---\n");
    setup();

    mk_dev(&r, (uint8_t)EC_DEV_EV_OFFLINE, 1u, 2000u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_ADDED, "第 1 条 offline -> 新增");

    mk_dev(&r, (uint8_t)EC_DEV_EV_OFFLINE, 1u, 2100u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_MERGED, "★ 同一台再报 offline -> 合并");
    mk_dev(&r, (uint8_t)EC_DEV_EV_OFFLINE, 1u, 2200u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_MERGED, "★ 再来一条 -> 仍然合并");

    CHECK(event_cache_count() == 1u, "★ 3 次 offline 只占 1 条(实际 %u)",
          (unsigned)event_cache_count());
    CHECK(event_cache_peek(&out) && (out.ts == 2200u), "留下的是最新那条(ts=2200)");

    /* 换一台设备：必须新增，不能把上一台覆盖掉 */
    mk_dev(&r, (uint8_t)EC_DEV_EV_OFFLINE, 2u, 2300u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_ADDED, "另一台设备 -> 新增");
    CHECK(event_cache_count() == 2u, "两台各一条");

    /* 同一台恢复上线：online 不可合并，要成对保留 */
    mk_dev(&r, (uint8_t)EC_DEV_EV_ONLINE, 1u, 2400u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_ADDED, "online 不合并 -> 新增");
    CHECK(event_cache_count() == 3u, "offline(A) / offline(B) / online(A) 三条");

    /* 恢复之后再次掉线：这是【新的一次】掉线，必须追加 */
    mk_dev(&r, (uint8_t)EC_DEV_EV_OFFLINE, 1u, 2500u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_ADDED,
          "★ online 之后的 offline 是新的掉线 -> 追加(不合并)");
    CHECK(event_cache_count() == 4u, "三条变四条");

    /* 顺序必须是 2200(A offline) < 2300(B) < 2400(A online) < 2500(A offline) */
    {
        static const uint32_t want[4] = { 2200u, 2300u, 2400u, 2500u };
        uint16_t i;

        for (i = 0u; i < 4u; i++)
        {
            CHECK(event_cache_peek_at(i, &out) && (out.ts == want[i]),
                  "第 %u 条 ts = %u(实际 %u)", (unsigned)i, (unsigned)want[i],
                  (unsigned)out.ts);
        }
    }
}

/* ---------------- 3) 到点提交：没等到同 key 也要落盘 ---------------- */
static void test_pend_timeout(void)
{
    ec_record_t r;
    ec_record_t out;
    xf_meta_t   m;

    printf("\n--- 到点提交: 1 s 内没等到同 key 必须落盘 ---\n");
    setup();

    mk_dev(&r, (uint8_t)EC_DEV_EV_LOW_VOLTAGE, 3u, 3000u);
    CHECK(event_cache_push_latest(&r) == EC_PUSH_ADDED, "低电量 -> 进待提交槽");
    CHECK(event_cache_count() == 1u, "★ 待提交槽也算一条(count 里含它)");

    /* 未到点：poll 不该动它 */
    host_tick_advance(500u);
    event_cache_poll();
    CHECK(xf_ring_count(XF_RING_EVENT) == 0u, "500 ms 时还没落盘(继续攒合并机会)");

    /* 到点：必须落盘 */
    host_tick_advance(600u);
    event_cache_poll();
    CHECK(xf_ring_count(XF_RING_EVENT) == 1u, "★ 1 s 到点 -> 已写进环(掉电也不丢)");
    CHECK(event_cache_count() == 1u, "count 仍然是 1(不重复计)");

    CHECK(xf_ring_flush(), "flush META");
    CHECK(xf_meta_read(&m) && (m.ring_head[XF_RING_EVENT] == XF_SLOT_SIZE),
          "head 落盘 = 1 个槽");

    CHECK(event_cache_peek(&out) && (out.ts == 3000u) && (out.a == (uint8_t)EC_DEV_EV_LOW_VOLTAGE),
          "读回来的还是那条低电量");
}

/* ---------------- 4) 分页：不切动作组 ---------------- */
static void test_paging(void)
{
    ec_record_t r;
    uint16_t    n;

    printf("\n--- 分页: 不把同一次动作的 results 切两半 ---\n");
    setup();

    /* 8 条普通事件 + 一次涉及 3 台设备的动作(seq=7) */
    {
        uint16_t i;

        for (i = 0u; i < 8u; i++)
        {
            mk_status(&r, (uint8_t)EC_ST_WARNING, (uint8_t)EC_ST_NORMAL, 4000u + i);
            (void)event_cache_push(&r);
        }
        for (i = 0u; i < 3u; i++)
        {
            mk_action(&r, 7u, (uint8_t)(i + 1u), (int32_t)i, 4100u + i);
            (void)event_cache_push(&r);
        }
    }

    /* 页大小 9: 页尾正好落在动作组的第 1 条 —— 必须整组补完(返回 11) */
    n = event_cache_page_count(9u);
    CHECK(n == 11u, "★ 页大小 9 -> 返回 11(把 seq=7 这一组补完), 实际 %u", (unsigned)n);

    /* 页大小 5: 页尾在普通事件上，不补 */
    n = event_cache_page_count(5u);
    CHECK(n == 5u, "页大小 5 -> 按 5 条返回(不涉及动作组), 实际 %u", (unsigned)n);

    /* 页大小 >= 总数: 全给 */
    n = event_cache_page_count(64u);
    CHECK(n == 11u, "页大小 64 -> 全给 11 条");

    /* 页大小 0: 不取 */
    CHECK(event_cache_page_count(0u) == 0u, "页大小 0 -> 0");
}

/* ---------------- 5) 动作分组：results[] 收齐 ---------------- */
static void test_group(void)
{
    ec_record_t      r;
    ec_action_pair_t pairs[8];
    uint8_t          n;

    printf("\n--- 动作分组: results[] 必须收齐 ---\n");
    setup();

    mk_status(&r, (uint8_t)EC_ST_ALARM, (uint8_t)EC_ST_NORMAL, 5000u);
    (void)event_cache_push(&r);                 /* 前面垫一条普通事件 */

    mk_action(&r, 3u, 1u, 0, 5100u); (void)event_cache_push(&r);
    mk_action(&r, 3u, 2u, 1005, 5101u); (void)event_cache_push(&r);
    mk_action(&r, 3u, 3u, 0, 5102u); (void)event_cache_push(&r);

    /* 从队头(不是动作记录)取 -> 0 */
    CHECK(event_cache_peek_action_group(pairs, 8u) == 0u, "队头不是动作记录 -> 0 对");

    /* 从第 1 条(第一条动作)取 -> 3 对 */
    n = event_cache_group_at(1u, pairs, 8u);
    CHECK(n == 3u, "★ 收齐 3 台(实际 %u)", (unsigned)n);
    CHECK((n == 3u) && (pairs[1].code == 1005), "第 2 台的执行码 = 1005");
    CHECK((n == 3u) && (pairs[2].uid[12] == 3u), "第 3 台的 uid 尾巴 = 3");

    /* 装不下(max_pairs=2) -> 返回 0，绝不截断成 2 台 */
    CHECK(event_cache_group_at(1u, pairs, 2u) == 0u,
          "★ 容量装不下整组 -> 0(宁可不报, 也不发半条)");

    /* seq == 0 的动作记录不成组 */
    mk_action(&r, 0u, 9u, 0, 5200u);
    (void)event_cache_push(&r);
    CHECK(event_cache_group_at(4u, pairs, 8u) == 0u, "seq=0 不成组");
}

/* ---------------- 6) 边界 ---------------- */
static void test_edges(void)
{
    ec_record_t r;

    printf("\n--- 边界 ---\n");
    setup();

    CHECK(event_cache_push(NULL) == false, "push(NULL) -> false");
    CHECK(event_cache_push_latest(NULL) == EC_PUSH_FAIL, "push_latest(NULL) -> FAIL");

    (void)memset(&r, 0, sizeof(r));
    r.kind = 99u;                                /* 不认识的 kind */
    CHECK(event_cache_push(&r) == false, "未知 kind -> false");
    CHECK(event_cache_count() == 0u, "拒收后 count 仍 0");

    mk_status(&r, (uint8_t)EC_ST_NORMAL, (uint8_t)EC_ST_WARNING, 6000u);
    CHECK(event_cache_push(&r) == true, "正常 push -> true");
    CHECK(event_cache_capacity() > 1000u, "容量 = %u 条(由 EVENT 分区决定)",
          (unsigned)event_cache_capacity());

    event_cache_reset();
    CHECK((event_cache_count() == 0u) && (event_cache_dropped() == 0u),
          "reset 清空 count 与 dropped");
}

int main(void)
{
    printf("==== event_cache: 断线事件队列(外挂 Flash 后端) ====\n");
    test_order();
    test_merge();
    test_pend_timeout();
    test_paging();
    test_group();
    test_edges();
    printf("\n==== %d passed, %d FAILED ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
