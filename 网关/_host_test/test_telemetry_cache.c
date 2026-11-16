/**
 * \file  test_telemetry_cache.c
 * \brief 离线期间的"逐条液位上报"队列的主机单测（外挂 Flash 后端）
 *        (见 app/service/telemetry_cache.h)
 *
 * 为什么值得单独测:
 *   这是**现场最难复现**的一块 —— 要断 4G、等液位报几十条、再重连,
 *   才能看出"顺序对不对/满了丢哪条/计数准不准"。而这几件事全是纯逻辑,
 *   主机上一次跑完。
 *
 * ★2026-11-11: 后端从"RAM 环 128 条"换成"外挂 Flash 单写"★
 *   所以这里用 fake_xflash 的内存盘（真 NOR 语义：写是 AND、擦才回 1），
 *   而不是以前的 fake_xflash_absent —— 那条路径已经不存在了。
 *
 * 重点验六件:
 *   1) 顺序: peek 永远给【最旧】那条, drop 之后下一条接上(FIFO, 不是 LIFO);
 *   2) 原样: ts / uid / 电量 / RSSI / level+raw 双口径（含负水位）都不许走样;
 *   3) 落盘: push 之后 head/tail 一落 META, 重启才能接着补报;
 *   4) 满了: 淘汰【最旧】而不是丢新的, dropped 如实累加;
 *   5) 边界: 空表 peek == NULL / drop 安全 / push(NULL) 拒绝且不改状态;
 *   6) 容量: 与 TELEM 分区算出来的槽数一致（不写死 128）。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "telemetry_cache.h"
#include "xflash_store.h"

/* host_stub.c 提供的时基控制（没有头文件，按工程惯例 extern 声明） */
void host_tick_set(uint32_t ms);

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

/** 造一条记录: 用 level 当作"第几条"的标记, 便于验顺序 */
static void mk_rec(tc_record_t *r, uint16_t level)
{
    (void)memset(r, 0, sizeof(*r));
    r->ts        = 1700000000u + (uint32_t)level;   /* 每条 ts 不同, 便于验"原时间戳" */
    r->level_mm  = level;
    r->raw_mm    = level;
    r->msg_seq   = (uint16_t)(level % 1000u);
    r->uid[0]    = 0x21u;
    r->uid[12]   = (uint8_t)(level & 0xFFu);
    r->bat_pct   = 88u;
    r->rssi_dbm  = -70;
    r->state     = 2u;
    r->gear      = 1u;
}

/* ---------------- 0) 夹具：干净的内存盘 + 初始化 ---------------- */
static void setup(void)
{
    host_tick_set(1000u);
    fx_blank();
    (void)xf_store_init();
    (void)xf_ring_format_all();
    telemetry_cache_reset();
}

/* ---------------- 1) 记录大小 / 容量 ---------------- */
static void test_size(void)
{
    const xf_region_info_t *ri = xf_region_info(XF_REGION_TELEM);

    printf("\n--- 记录大小与容量 ---\n");
    setup();                    /* capacity 要先有可用的 Flash 才有值 */

    /* 记录必须装进 64 B 槽（12 B 头 + 52 B 负载）；超了环会直接写失败 */
    CHECK(sizeof(tc_record_t) <= (XF_SLOT_SIZE - XF_REC_HDR_SIZE),
          "sizeof(tc_record_t) = %u <= 单槽负载上限 %u",
          (unsigned)sizeof(tc_record_t), (unsigned)(XF_SLOT_SIZE - XF_REC_HDR_SIZE));

    CHECK((ri != NULL) && (telemetry_cache_capacity() == (uint16_t)(ri->size / XF_SLOT_SIZE)),
          "★ capacity 由 TELEM 分区算出 = %u 条(不是写死的 128)",
          (unsigned)telemetry_cache_capacity());
    CHECK(telemetry_cache_capacity() > 10000u,
          "★ 3 MB 能存 %u 条(4 台x300s 约 %u 天)",
          (unsigned)telemetry_cache_capacity(),
          (unsigned)(telemetry_cache_capacity() / (4u * 288u)));
}

/* ---------------- 2) 基本顺序(最旧先出) ---------------- */
static void test_fifo_order(void)
{
    tc_record_t r;
    const tc_record_t *p;

    printf("\n--- 顺序: 最旧先出 ---\n");
    setup();
    CHECK(telemetry_cache_count() == 0u, "reset 后 count == 0");

    mk_rec(&r, 111u); (void)telemetry_cache_push(&r);
    mk_rec(&r, 222u); (void)telemetry_cache_push(&r);
    mk_rec(&r, 333u); (void)telemetry_cache_push(&r);
    CHECK(telemetry_cache_count() == 3u, "push 3 条 -> count 3");

    p = telemetry_cache_peek();
    CHECK((p != NULL) && (p->level_mm == 111u), "★ peek 给【最旧】(111) 而不是最新");
    CHECK((p != NULL) && (p->ts == 1700000111u), "★ 原时间戳原样保留(补报要用的就是它)");
    CHECK((p != NULL) && (p->uid[12] == (uint8_t)(111u & 0xFFu)), "uid 字节原样保留");
    CHECK((p != NULL) && (p->bat_pct == 88u) && (p->rssi_dbm == -70),
          "电量/RSSI 原样保留");

    telemetry_cache_drop();
    CHECK(telemetry_cache_count() == 2u, "drop 后 count 2");
    p = telemetry_cache_peek();
    CHECK((p != NULL) && (p->level_mm == 222u), "★ drop 之后队头变成 222");

    telemetry_cache_drop();
    telemetry_cache_drop();
    CHECK(telemetry_cache_count() == 0u, "全部取空");
    CHECK(telemetry_cache_peek() == NULL, "空表 peek == NULL");
    telemetry_cache_drop();                       /* 空表 drop 不能崩 */
    CHECK(telemetry_cache_count() == 0u, "空表 drop 安全(仍 0)");
}

/* ---------------- 3) 落盘：head/tail 进 META ---------------- */
static void test_persist(void)
{
    tc_record_t r;
    xf_meta_t   m;

    printf("\n--- 落盘: 指针进 META(重启才能接着补报) ---\n");

    setup();
    mk_rec(&r, 1u); (void)telemetry_cache_push(&r);
    mk_rec(&r, 2u); (void)telemetry_cache_push(&r);
    CHECK(telemetry_cache_count() == 2u, "推 2 条");

    /* 主循环里的 xf_ring_poll 是"空闲 30 s 才落"，这里直接 flush 等价于它到点 */
    CHECK(xf_ring_flush(), "xf_ring_flush() 成功");
    CHECK(xf_meta_read(&m), "读回 META");
    CHECK(m.ring_head[XF_RING_TELEM] == (2u * XF_SLOT_SIZE),
          "★ head = 2 个槽(%u B) —— 指针真的落盘了", (unsigned)m.ring_head[XF_RING_TELEM]);
    CHECK(m.ring_tail[XF_RING_TELEM] == 0u, "tail 仍在 0(还没补报)");

    telemetry_cache_drop();
    CHECK(xf_ring_flush(), "补报一条后再 flush");
    CHECK(xf_meta_read(&m) && (m.ring_tail[XF_RING_TELEM] == XF_SLOT_SIZE),
          "tail 跟着前移一个槽(%u B)", (unsigned)m.ring_tail[XF_RING_TELEM]);
}

/* ---------------- 4) 满了淘汰最旧 ---------------- */
static void test_overflow(void)
{
    tc_record_t r;
    const tc_record_t *p;
    uint32_t cap = (uint32_t)telemetry_cache_capacity();
    uint32_t i;

    printf("\n--- 满了淘汰最旧(不丢新的) ---\n");
    setup();
    CHECK(cap > 1000u, "容量 %u 条", (unsigned)cap);

    /* 先把环推到满：从 1 开始编号，便于验"被淘汰的是最旧那几条" */
    for (i = 1u; i <= cap; i++)
    {
        mk_rec(&r, (uint16_t)(i & 0xFFFFu));
        (void)telemetry_cache_push(&r);
    }

    /* 环形区会刻意留一整个 4 KB 扇区不写（否则 head 追上 tail 时会擦掉活数据），
     * 所以容纳条数是 cap - 64，多出来的那几十条按"淘汰最旧"处理。 */
    CHECK(telemetry_cache_dropped() <= 64u,
          "写满 cap 条, dropped = %u (只丢了'让出的那一个扇区'的条数)",
          (unsigned)telemetry_cache_dropped());

    {
        uint32_t d0 = telemetry_cache_dropped();
        uint32_t c0 = telemetry_cache_count();

        for (i = 0u; i < 10u; i++)
        {
            mk_rec(&r, 0u);
            r.level_mm = 9000 + (int32_t)i;         /* 新数据用大值, 一眼能认 */
            (void)telemetry_cache_push(&r);
        }
        CHECK(telemetry_cache_dropped() == (d0 + 10u),
              "★ 再进 10 条 -> dropped 精确 +10(%u -> %u)",
              (unsigned)d0, (unsigned)telemetry_cache_dropped());
        CHECK(telemetry_cache_count() == c0, "★ 条数保持不变(= 容量上限)");

        p = telemetry_cache_peek();
        CHECK((p != NULL) && (p->level_mm != 9000) && (p->level_mm != 9009),
              "★ 队头不是刚写的新数据(说明淘汰的是最旧)");
    }
}

/* ---------------- 5) 边界 ---------------- */
static void test_edges(void)
{
    tc_record_t r;

    printf("\n--- 边界 ---\n");
    setup();
    CHECK(telemetry_cache_push(NULL) == false, "push(NULL) -> false");
    CHECK(telemetry_cache_count() == 0u, "push(NULL) 不改状态");

    mk_rec(&r, 7u);
    CHECK(telemetry_cache_push(&r) == true, "正常 push -> true");

    telemetry_cache_reset();
    CHECK((telemetry_cache_count() == 0u) && (telemetry_cache_dropped() == 0u),
          "reset 清空 count 与 dropped");
    CHECK(telemetry_cache_capacity() > 0u, "capacity 恒 > 0");
}

/* ---------------- 6) level/raw 双口径 + 负水位 (A1) ---------------- */
static void test_level_raw(void)
{
    tc_record_t  in;
    const tc_record_t *p;

    printf("\n--- level / raw 双口径 (A1) ---\n");
    setup();

    /* distance 模式: level_mm 是换算后的水深(可为负), raw_mm 是传感器原始距离。
     * 补报必须两条都还原 —— 只存一个的话, 补报的 rawMm 会拿水深冒充原始值。 */
    mk_rec(&in, 2850u);
    in.level_mm = 2850;
    in.raw_mm   = 150u;
    CHECK(telemetry_cache_push(&in) == true, "push 一条 level=2850 raw=150");

    p = telemetry_cache_peek();
    CHECK((p != NULL) && (p->level_mm == 2850) && (p->raw_mm == 150u),
          "★ 补报还原两个值: level=%ld raw=%u",
          (p != NULL) ? (long)p->level_mm : -1L,
          (p != NULL) ? (unsigned)p->raw_mm : 0u);
    telemetry_cache_drop();

    /* 负水位(distance 模式水面退到探头以下)必须原样保留, 不能存成 0 或大正数 */
    mk_rec(&in, 0u);
    in.level_mm = -500;
    in.raw_mm   = 1500u;
    CHECK(telemetry_cache_push(&in) == true, "push 一条 level=-500 raw=1500");

    p = telemetry_cache_peek();
    CHECK((p != NULL) && (p->level_mm == -500) && (p->raw_mm == 1500u),
          "★ 负水位原样保留: level=%ld raw=%u",
          (p != NULL) ? (long)p->level_mm : 0L,
          (p != NULL) ? (unsigned)p->raw_mm : 0u);
    telemetry_cache_drop();

    /* level 模式: 两者相等(快照表里也是这么填的) */
    mk_rec(&in, 0u);
    in.level_mm = 1234;
    in.raw_mm   = 1234u;
    CHECK(telemetry_cache_push(&in) == true, "push 一条 level=raw=1234");
    p = telemetry_cache_peek();
    CHECK((p != NULL) && (p->level_mm == 1234) && (p->raw_mm == 1234u),
          "level 模式下两个值相同");

    telemetry_cache_reset();
}

int main(void)
{
    printf("==== telemetry_cache: 离线逐条上报队列(外挂 Flash 后端) ====\n");
    test_size();
    test_fifo_order();
    test_persist();
    test_overflow();
    test_edges();
    test_level_raw();
    printf("\n==== %d passed, %d FAILED ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
