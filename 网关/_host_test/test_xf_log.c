/**
 * @file  test_xf_log.c
 * @brief 日志落盘（LOG 环）—— 主机单测
 *        （被测：app/log/xf_log.c；后端：app/drivers/xflash/xflash_store.c + fake_xflash.c）
 *
 * 为什么这套测试必须写：
 *   日志落盘是整个固件里【唯一在 fputc 里被调用】的业务代码。它一旦出错，
 *   表现不是"某个功能不灵"，而是**打印一句日志就把系统搞挂/搞慢**：
 *     · fputc 里直接写 Flash → 每条日志阻塞几毫秒，扇区擦除 45ms，串口看着"卡死"；
 *     · xf_ring_push 失败时内部会 log_info → 又进 fputc → 无限递归栈溢出；
 *     · 截断切在汉字中间 → 终端上一串乱码，事后看日志反而更难查问题。
 *   这三件事在板子上都极难现场定位，只能在主机上钉死。
 *
 * 重点验八件：
 *   1) 一行一记录：\n 提交、\r\n 只算一行、空行不记；
 *   2) 超长截断到 40 字节且置 flags.bit0；
 *   3) 截断只切在 UTF-8 字符边界（绝不出现半个汉字）；
 *   4) 写入时取注入的时钟；未注入时 ts=0 但 tick_ms 仍有值；
 *   5) 节流：同一秒最多 4 条，多出来的计入 dropped；
 *   6) 待写队列：不 poll 时最多积压 8 条，溢出丢弃；
 *   7) poll 每次最多写 2 条（不能一次把队列抽干，否则主循环被拖住）；
 *   8) dump / clear：dump 不改变条数（递归保护的一部分），clear 归零。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "xf_log.h"
#include "xflash_store.h"

/* fake_xflash.c 提供的测试钩子 */
void     fx_blank(void);
void     fx_set_present(bool present);

/* host_stub.c 的虚拟时基 */
void     host_tick_set(uint32_t ms);
void     host_tick_advance(uint32_t ms);
uint32_t host_tick_get(void);

static int g_fail;
static int g_pass;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) { g_pass++; printf("  ok   : "); }            \
        else      { g_fail++; printf("  FAIL : "); }            \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    } while (0)

/* 固定时钟，用来验证 ts 是否真的被写进记录 */
static uint32_t s_fake_clock_on;
static uint32_t s_fake_clock_val;

static uint32_t fake_clock(void)
{
    return s_fake_clock_on ? s_fake_clock_val : 0u;
}

/* ---------------- 小工具：把一整行喂进去 ---------------- */

static void feed(const char *s)
{
    while (*s != '\0')
    {
        xf_log_feed_char(*s);
        s++;
    }
}

/** 把队列里剩下的都刷进 Flash（poll 每次只写 2 条） */
static void drain(void)
{
    uint32_t guard = 0u;

    while ((xf_log_pending() > 0u) && (guard < 64u))
    {
        xf_log_poll();
        guard++;
    }
}

/**
 * @brief 读回第 idx 条（0 = 最旧）的文本，返回文本长度
 * @note  xf_ring_peek_at 的 out_len 是【记录总长】(= sizeof(xf_log_rec_t) = 52)，
 *        正文长度要看 rec.len 字段 —— 别把两者搞混。
 */
static uint16_t read_back(uint32_t idx, char *out, uint32_t cap)
{
    xf_log_rec_t rec;
    uint16_t     len = 0u;
    uint8_t      l;

    out[0] = '\0';
    if (!xf_ring_peek_at(XF_RING_LOG, idx, &rec, (uint16_t)sizeof(rec), &len))
    {
        return 0u;
    }
    l = rec.len;
    if (l > XF_LOG_TEXT_MAX) { l = XF_LOG_TEXT_MAX; }
    if ((uint32_t)l >= cap) { l = (uint8_t)(cap - 1u); }
    (void)memcpy(out, rec.text, l);
    out[l] = '\0';
    return (uint16_t)l;
}

/** 读回第 idx 条的完整记录（用来检查 flags/ts/tick） */
static bool read_rec(uint32_t idx, xf_log_rec_t *out)
{
    uint16_t len = 0u;

    return xf_ring_peek_at(XF_RING_LOG, idx, out, (uint16_t)sizeof(*out), &len);
}

/** 每个用例开始前把环境重置干净 */
static void fresh(void)
{
    fx_blank();
    (void)xf_store_init();
    xf_ring_format_all();

    host_tick_set(1000u);
    s_fake_clock_on = 0u;
    s_fake_clock_val = 0u;

    xf_log_init();
    xf_log_set_clock(fake_clock);
    xf_log_set_enable(true);
    /* xf_log_init() 默认进"启动直写"模式；绝大多数用例要测的是稳态队列路径 */
    xf_log_set_direct(false);
}

/* ==================== 1. 一行一记录 ==================== */

static void test_one_line_one_record(void)
{
    char buf[64];

    printf("\n[1] 一行一记录 / 换行处理\n");
    fresh();

    feed("hello world\n");
    drain();
    CHECK(xf_log_count() == 1u, "喂 \"hello world\\n\" -> 1 条 (实际 %lu)",
          (unsigned long)xf_log_count());
    CHECK(read_back(0u, buf, sizeof(buf)) == 11u, "读回长度 11");
    CHECK(strcmp(buf, "hello world") == 0, "读回内容 = \"hello world\" (实际 \"%s\")", buf);

    /* \r\n 只算一行，且 \r 不进正文 */
    feed("second line\r\n");
    drain();
    CHECK(xf_log_count() == 2u, "\\r\\n 只算 1 条 (实际 %lu)", (unsigned long)xf_log_count());
    (void)read_back(1u, buf, sizeof(buf));
    CHECK(strcmp(buf, "second line") == 0, "\\r 未进正文 (实际 \"%s\")", buf);

    /* 纯空行不记 */
    feed("\n");
    feed("\r\n");
    drain();
    CHECK(xf_log_count() == 2u, "纯空行 / \\r\\n 空行都不记 (仍是 %lu 条)",
          (unsigned long)xf_log_count());

    /* 没有换行就不提交 —— 半行必须留在缓冲里等下一个字节 */
    feed("no newline yet");
    drain();
    CHECK(xf_log_count() == 2u, "没有 \\n 的半行不落盘 (仍是 %lu 条)",
          (unsigned long)xf_log_count());
    feed("\n");
    drain();
    CHECK(xf_log_count() == 3u, "补上 \\n 后才落盘 (现 %lu 条)",
          (unsigned long)xf_log_count());
}

/* ==================== 2. 超长截断 ==================== */

static void test_truncate(void)
{
    xf_log_rec_t rec;
    char         buf[64];
    char         longline[128];
    int          i;

    printf("\n[2] 超长截断\n");
    fresh();

    for (i = 0; i < 100; i++) { longline[i] = (char)('A' + (i % 26)); }
    longline[100] = '\0';

    feed(longline);
    feed("\n");
    drain();

    CHECK(xf_log_count() == 1u, "100 字节的行照样落盘 1 条");
    CHECK(read_rec(0u, &rec), "能读回记录");
    CHECK(rec.len == XF_LOG_TEXT_MAX, "len 被钳到 %u (实际 %u)",
          (unsigned)XF_LOG_TEXT_MAX, (unsigned)rec.len);
    CHECK((rec.flags & 0x01u) != 0u, "flags.bit0 = 1 表示被截断");

    (void)read_back(0u, buf, sizeof(buf));
    CHECK(strlen(buf) == XF_LOG_TEXT_MAX, "读回文本 %u 字节 (实际 %u)",
          (unsigned)XF_LOG_TEXT_MAX, (unsigned)strlen(buf));

    /* 正好 40 字节不该被标截断 */
    fresh();
    for (i = 0; i < 40; i++) { longline[i] = 'x'; }
    longline[40] = '\0';
    feed(longline);
    feed("\n");
    drain();
    CHECK(read_rec(0u, &rec), "40 字节整行的记录可读");
    CHECK((rec.flags & 0x01u) == 0u, "正好 40 字节不算截断");
    CHECK(rec.len == 40u, "len = 40 (实际 %u)", (unsigned)rec.len);
}

/* ==================== 3. UTF-8 边界 ==================== */

static void test_utf8_boundary(void)
{
    xf_log_rec_t rec;
    char         buf[64];
    char         line[128];
    int          i;
    uint32_t     n;

    printf("\n[3] 截断只切在 UTF-8 字符边界\n");
    fresh();

    /* "中" = E4 B8 AD，3 字节。14 个 = 42 字节，钳到 40 时会落在第 14 个汉字的
     * 第 2 个字节上 —— 必须退回到 39，保住 13 个完整汉字。 */
    n = 0u;
    for (i = 0; i < 14; i++)
    {
        line[n++] = (char)0xE4;
        line[n++] = (char)0xB8;
        line[n++] = (char)0xAD;
    }
    line[n] = '\0';

    feed(line);
    feed("\n");
    drain();

    CHECK(read_rec(0u, &rec), "中文长行的记录可读");
    CHECK((rec.flags & 0x01u) != 0u, "被标为截断");
    CHECK(rec.len == 39u, "退到字符边界 -> 39 字节 = 13 个汉字 (实际 %u)",
          (unsigned)rec.len);
    CHECK((rec.len % 3u) == 0u, "长度是 3 的整数倍(没有半个汉字)");

    (void)read_back(0u, buf, sizeof(buf));
    CHECK(strlen(buf) == 39u, "读回 39 字节");

    /* 逐字节确认读回的正好是 13 个完整的 "中"(E4 B8 AD)，一个半截都不能有。
     * 注意："末尾不是续字节"这种判法是错的 —— UTF-8 多字节字符的中间/末字节
     * 本身就落在 10xxxxxx 区间，所以要看的是【整段是否由完整字符拼成】。 */
    {
        int      whole = 1;
        uint32_t k;

        for (k = 0u; k < 39u; k += 3u)
        {
            if (((uint8_t)buf[k] != 0xE4u) ||
                ((uint8_t)buf[k + 1u] != 0xB8u) ||
                ((uint8_t)buf[k + 2u] != 0xADu))
            {
                whole = 0;
                break;
            }
        }
        CHECK(whole == 1, "读回的是 13 个完整汉字(无半截字符)");
    }
}

/* ==================== 4. 时间戳 ==================== */

static void test_timestamp(void)
{
    xf_log_rec_t rec;

    printf("\n[4] 时间戳(tick 与注入时钟)\n");
    fresh();

    /* 没注入时钟: ts=0, tick 有值 */
    xf_log_set_clock(NULL);
    host_tick_set(54321u);
    feed("no clock\n");
    drain();
    CHECK(read_rec(0u, &rec), "无时钟记录可读");
    CHECK(rec.ts == 0u, "未注入时钟 -> ts = 0 (实际 %lu)", (unsigned long)rec.ts);
    CHECK(rec.tick_ms == 54321u, "tick_ms 仍记录 (实际 %lu)", (unsigned long)rec.tick_ms);

    /* 注入时钟: ts 原样写入 */
    fresh();
    s_fake_clock_on  = 1u;
    s_fake_clock_val = 1700000000u;
    xf_log_set_clock(fake_clock);
    feed("with clock\n");
    drain();
    CHECK(read_rec(0u, &rec), "有时钟记录可读");
    CHECK(rec.ts == 1700000000u, "ts = 注入值 (实际 %lu)", (unsigned long)rec.ts);
}

/* ==================== 5. 节流 ==================== */

static void test_throttle(void)
{
    int i;

    printf("\n[5] 节流: 同一秒最多 12 条\n");
    fresh();

    /* ★必须跨出"启动直写"窗口★：fresh() 已关掉 direct，这里直接限速 */
    host_tick_set(20000u);
    for (i = 0; i < 20; i++)
    {
        feed("flood line\n");
    }
    drain();

    CHECK(xf_log_count() == 12u, "同秒喂 20 行 -> 只落 12 条 (实际 %lu)",
          (unsigned long)xf_log_count());
    CHECK(xf_log_dropped() == 8u, "其余 8 条计入 dropped (实际 %lu)",
          (unsigned long)xf_log_dropped());

    /* 过一秒额度恢复 */
    host_tick_advance(1000u);
    for (i = 0; i < 20; i++) { feed("next second\n"); }
    drain();
    CHECK(xf_log_count() == 24u, "下一秒再落 12 条 (累计 %lu)", (unsigned long)xf_log_count());
}

/* ==================== 6. 待写队列 ==================== */

static void test_queue_overflow(void)
{
    int i;

    printf("\n[6] 待写队列: 不 poll 时最多积压 32 条\n");
    fresh();

    /* 每秒喂 4 条(正好不触发节流)，共 12 秒 = 48 条，但一条都不 poll。
     * 队列深 32，于是后 16 条被丢。 */
    for (i = 0; i < 12; i++)
    {
        host_tick_set(30000u + (uint32_t)(i * 1000));
        feed("q line\n");
        feed("q line\n");
        feed("q line\n");
        feed("q line\n");
    }

    CHECK(xf_log_pending() == 32u, "队列积压上限 32 (实际 %lu)",
          (unsigned long)xf_log_pending());
    CHECK(xf_log_dropped() == 16u, "溢出丢 16 条 (实际 %lu)",
          (unsigned long)xf_log_dropped());
    CHECK(xf_log_count() == 0u, "还没 poll -> Flash 里 0 条 (实际 %lu)",
          (unsigned long)xf_log_count());

    /* poll 一次只写 2 条，不能抽干 */
    xf_log_poll();
    CHECK(xf_log_pending() == 30u, "poll 一次写 2 条 -> 余 30 (实际 %lu)",
          (unsigned long)xf_log_pending());
    CHECK(xf_log_count() == 2u, "Flash 里 2 条 (实际 %lu)", (unsigned long)xf_log_count());

    drain();
    CHECK(xf_log_pending() == 0u, "drain 后队列空");
    CHECK(xf_log_count() == 32u, "Flash 里共 32 条 (实际 %lu)", (unsigned long)xf_log_count());
}

/* ==================== 6b. 启动直写 ==================== */

static void test_boot_window(void)
{
    int i;

    printf("\n[6b] 启动直写: 主循环还没跑起来也不能丢启动日志\n");
    fresh();
    xf_log_set_direct(true);        /* 模拟 xf_log_init() 之后的启动阶段 */

    for (i = 0; i < 30; i++) { feed("boot line\n"); }

    CHECK(xf_log_count() == 30u, "30 行全部直接落盘 (实际 %lu)",
          (unsigned long)xf_log_count());
    CHECK(xf_log_pending() == 0u, "没有一条走队列 (实际 %lu)",
          (unsigned long)xf_log_pending());
    CHECK(xf_log_dropped() == 0u, "dropped = 0 (实际 %lu)",
          (unsigned long)xf_log_dropped());

    /* 关掉直写后回到队列路径（进主循环时的行为） */
    xf_log_set_direct(false);
    feed("after boot\n");
    CHECK(xf_log_pending() == 1u, "关掉直写后走队列 (实际 %lu)",
          (unsigned long)xf_log_pending());
    drain();
    CHECK(xf_log_count() == 31u, "共 31 条 (实际 %lu)", (unsigned long)xf_log_count());
}

/* ==================== 7. dump / clear ==================== */

static void test_dump_and_clear(void)
{
    char buf[64];

    printf("\n[7] dump 不改变条数 / clear 归零\n");
    fresh();

    feed("alpha\n");
    feed("beta\n");
    feed("gamma\n");
    drain();

    CHECK(xf_log_count() == 3u, "已落 3 条");

    /* dump 会走 log_info —— 主机上 log 是桩，但仍须保证不会反向写入新记录 */
    xf_log_dump(0u);
    drain();
    CHECK(xf_log_count() == 3u, "dump 之后仍是 3 条(没把 dump 自己记进去) (实际 %lu)",
          (unsigned long)xf_log_count());

    (void)read_back(2u, buf, sizeof(buf));
    CHECK(strcmp(buf, "gamma") == 0, "最新一条是 gamma (实际 \"%s\")", buf);

    CHECK(xf_log_clear(), "clear 返回 true");
    CHECK(xf_log_count() == 0u, "clear 后 0 条 (实际 %lu)", (unsigned long)xf_log_count());
    CHECK(xf_log_pending() == 0u, "clear 也清空待写队列");

    /* clear 之后还能正常写 */
    feed("after clear\n");
    drain();
    CHECK(xf_log_count() == 1u, "clear 之后仍可写入 (实际 %lu)", (unsigned long)xf_log_count());
}

/* ==================== 8. 关闭落盘 ==================== */

static void test_disable(void)
{
    printf("\n[8] 关闭落盘后零写入\n");
    fresh();

    feed("before off\n");
    drain();
    CHECK(xf_log_count() == 1u, "关闭前落了 1 条");

    xf_log_set_enable(false);
    CHECK(!xf_log_enabled(), "enabled = false");

    feed("while off\n");
    drain();
    CHECK(xf_log_count() == 1u, "关闭期间不落盘 (仍是 %lu 条)",
          (unsigned long)xf_log_count());

    xf_log_set_enable(true);
    feed("after on\n");
    drain();
    CHECK(xf_log_count() == 2u, "重新打开后继续 (实际 %lu)", (unsigned long)xf_log_count());
}

/* ==================== 9. 没有芯片 ==================== */

static void test_no_chip(void)
{
    uint32_t before;

    printf("\n[9] 芯片不在位 -> 全部 no-op(不能崩, 不能写)\n");

    /* ★注意 xf_store_init() 内部对已就绪的环有缓存, fx_blank() 之后再 init
     *   不一定把 s_ring 归零。所以这里不假设 count==0, 只要求"喂进去的东西
     *   不会让条数变化"。 */
    fx_blank();
    (void)xf_store_init();
    fx_set_present(false);

    host_tick_set(1000u);
    xf_log_init();

    CHECK(!xf_log_enabled(), "init 时芯片不在位 -> enabled = false");

    before = xf_log_count();

    feed("should be discarded\n");
    xf_log_poll();

    CHECK(xf_log_count() == before, "feed + poll 之后条数没变(%lu)",
          (unsigned long)before);
    CHECK(xf_log_pending() == 0u, "pending = 0 (喂进来的被直接丢弃)");

    /* dump 也不能崩 */
    xf_log_dump(0u);
    CHECK(xf_log_clear() == false, "clear 在无芯片时返回 false 而不是崩");

    fx_set_present(true);       /* 复原，免得影响后面的用例 */
}

/* ==================== 10. 暂停捕获 ==================== */

static void test_pause(void)
{
    char buf[64];

    printf("\n[10] 暂停捕获(每秒快照日志不落盘)\n");
    fresh();

    feed("real event\n");
    drain();
    CHECK(xf_log_count() == 1u, "正常日志落盘 1 条 (实际 %lu)",
          (unsigned long)xf_log_count());

    /* 模拟 app_status_report(): [STAT]/[STAT2] 每秒两条，必须被挡在外面 */
    xf_log_pause_capture();
    feed("[STAT] 4G=ONLINE snapshot\n");
    feed("[STAT2] counters snapshot\n");
    drain();
    CHECK(xf_log_count() == 1u, "暂停期间不落盘 (仍是 %lu 条)",
          (unsigned long)xf_log_count());

    xf_log_resume_capture();
    feed("after resume\n");
    drain();
    CHECK(xf_log_count() == 2u, "恢复后继续落盘 (实际 %lu)",
          (unsigned long)xf_log_count());

    /* 暂停时如果已经攒了半行，恢复后不能粘到下一行上 */
    xf_log_pause_capture();
    feed("half line");
    xf_log_resume_capture();
    feed("fresh line\n");
    drain();

    (void)read_back(2u, buf, sizeof(buf));
    CHECK(strcmp(buf, "fresh line") == 0, "半行被丢弃, 没粘到下一行 (实际 \"%s\")", buf);
}

int main(void)
{
    printf("==== test_xf_log: 日志落盘(LOG 环) ====\n");

    test_one_line_one_record();
    test_truncate();
    test_utf8_boundary();
    test_timestamp();
    test_throttle();
    test_queue_overflow();
    test_boot_window();
    test_dump_and_clear();
    test_disable();
    test_pause();
    test_no_chip();

    printf("\n==== 通过 %d, 失败 %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
