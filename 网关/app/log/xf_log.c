/**
 * \file  xf_log.c
 * \brief 关键日志落盘（LOG 环）实现
 *
 * 布局与语义见 xf_log.h 的文件头。这里只记实现上几个必须遵守的约束：
 *   1. fputc() 上下文里**只攒行 + 入 RAM 队列**，绝不碰 Flash。
 *   2. xf_ring_push() 内部会 log_info()，所以写盘期间必须置 s_writing 挡住递归。
 *   3. dump() 本身也在打印日志，同样要挡住。
 *   4. 截断按 UTF-8 字符边界，绝不把汉字切一半（半截 UTF-8 在终端上会连累后文）。
 */
#include "xf_log.h"

/* ★必须在 #if EXT_FLASH_ENABLE 之前 include★
 * EXT_FLASH_ENABLE 是在 xflash.h 里 #define 的（默认 1）。如果先写 #if 再 include，
 * 未定义宏按 0 处理 → 整份实现被编成空壳，而且【不会报任何错】：app_boot/app_loop
 * 调的都是空函数，链接照样过，表现为"日志一条都不落盘"且查不出原因。
 * Keil 工程里恰好全局定义了 EXT_FLASH_ENABLE=1，所以那边一直是好的 ——
 * 这个坑是主机测试（没有全局 define）抓出来的。 */
#include "xflash.h"

#if EXT_FLASH_ENABLE

#include "xflash_store.h"
#include "bsp_tim.h"
#include "log.h"

#include <string.h>
#include <stdio.h>      /* snprintf: 时间格式化。板子上 log.h 间接带进来, 主机 stub 不带 */

/* ------------------------------------------------------------------
 * 编译期断言：记录必须正好塞满一个槽的负载区
 * 64(槽) - 12(记录头) = 52 字节
 * ------------------------------------------------------------------ */
typedef char xf_log_rec_size_must_match_slot[
    (sizeof(xf_log_rec_t) == XF_REC_MAX_PAYLOAD) ? 1 : -1];

/* 行缓冲比正文多留几字节：截断判断时要读 s_line[n] 这一个"未写入"的字节 */
#define XF_LOG_LINE_CAP     (XF_LOG_TEXT_MAX + 8u)

/* 待写队列深度。
 * ★2026-11-11 从 8 提到 32★ 原因：开机阶段主循环还没跑，xf_log_poll() 一次都
 * 没被调用过，而这恰好是 [BOOT]/[XFLASH]/[G4]/[LORA] 打日志最密集的时候 ——
 * 队列 8 条瞬间就满，实测丢掉了 226 条（几乎整个启动过程）。启动日志是现场
 * 取证里最有价值的一段，不能丢。32 x 52 B = 1664 B RAM，换这个值。 */
#define XF_LOG_QDEPTH       32u

/* 每秒最多落盘条数（只在稳态生效）。
 * ★2026-11-11 从 4 提到 12★ [STAT] 已经用 pause 过滤掉了，剩下的全是真日志。
 * 实测 4 条/秒时，4G 建连阶段（大量 [G4_TX] AT 回显）会丢掉 187 条。
 * 12 条/秒 x 0.7 ms = 8.4 ms/秒 ≈ 0.8% CPU，可接受；
 * 16384 条满速也要 22 分钟才写满，稳态远远达不到。 */
#define XF_LOG_MAX_PER_SEC  12u

/* 每条 poll 最多写几条（Flash 页编程 ~0.7ms，换扇区还要擦 45ms） */
#define XF_LOG_POLL_BUDGET  2u

static xf_log_rec_t s_q[XF_LOG_QDEPTH];
static volatile uint8_t s_q_head;       /* 下一个写入位置 */
static volatile uint8_t s_q_tail;       /* 下一个读取位置 */
static volatile uint8_t s_q_cnt;

static char     s_line[XF_LOG_LINE_CAP];
static uint16_t s_line_len;
static uint8_t  s_line_trunc;           /* 本行已被丢弃过字符 */

static volatile uint8_t s_writing;      /* ★递归保护★ 1 = 正在碰 Flash / 正在打印 */
static bool     s_enabled;
static bool     s_paused;               /* 临时屏蔽捕获(周期性快照日志) */
static bool     s_direct;               /* 启动阶段: 直接写盘, 不经队列 */
static uint32_t (*s_clock)(void);

static uint32_t s_dropped;
static uint32_t s_sec_tick;             /* 当前统计秒的起点 tick */
static uint8_t  s_sec_cnt;

/* ==================== 内部：时间格式化 ==================== */

/**
 * @brief Unix 秒 -> "MM-DD HH:MM:SS"（东八区）
 * @note  用 Howard Hinnant 的 civil_from_days 算法，不依赖库/云端模块，
 *        免得 log 层反向依赖 cloud_proto。日志只需要"看得懂"，不需要闰秒精度。
 */
static void xf_log_fmt_ts(uint32_t ts, char *out, uint32_t cap)
{
    uint32_t days = ts / 86400u;
    uint32_t secs = ts % 86400u;
    uint32_t z, era, doe, yoe, doy, mp, d, m;
    int32_t  m32;

    if ((out == NULL) || (cap < 18u)) { return; }

    secs += 8u * 3600u;                        /* UTC -> 北京 */
    if (secs >= 86400u)
    {
        secs -= 86400u;
        days++;
    }

    z   = days + 719468u;
    era = z / 146097u;
    doe = z - (era * 146097u);
    yoe = (doe - (doe / 1460u) + (doe / 36524u) - (doe / 146096u)) / 365u;
    doy = doe - ((365u * yoe) + (yoe / 4u) - (yoe / 100u));
    mp  = ((5u * doy) + 2u) / 153u;

    /* 1/2 月属于上一年的末尾，这里不需要年份，只把月份摆正 */
    m32 = (int32_t)mp + ((mp < 10u) ? 3 : -9);
    m   = (uint32_t)m32;
    d   = doy - (((153u * mp) + 2u) / 5u) + 1u;

    (void)snprintf(out, cap, "%02u-%02u %02u:%02u:%02u",
                   (unsigned)m, (unsigned)d,
                   (unsigned)(secs / 3600u),
                   (unsigned)((secs % 3600u) / 60u),
                   (unsigned)(secs % 60u));
}

/* ==================== 内部：提交一行 ==================== */

static void xf_log_commit_line(void)
{
    xf_log_rec_t rec;
    uint32_t     now;
    uint16_t     n = s_line_len;

    if ((n == 0u) && (s_line_trunc == 0u)) { return; }    /* 纯空行不记 */

    /* ---- 节流：每秒最多 XF_LOG_MAX_PER_SEC 条（只在稳态生效）----
     * 启动阶段（s_direct）不节流：启动日志密度最高、价值也最高，
     * 用"每秒 4 条"去卡它等于把取证材料砍掉八成。 */
    now = get_tick_ms();
    if (!s_direct)
    {
        if ((uint32_t)(now - s_sec_tick) >= 1000u)
        {
            s_sec_tick = now;
            s_sec_cnt  = 0u;
        }
        if (s_sec_cnt >= XF_LOG_MAX_PER_SEC)
        {
            s_dropped++;
            s_line_len   = 0u;
            s_line_trunc = 0u;
            return;
        }
        s_sec_cnt++;
    }

    /* ---- 组装记录 ---- */
    (void)memset(&rec, 0, sizeof(rec));

    if (n > XF_LOG_TEXT_MAX)
    {
        n            = XF_LOG_TEXT_MAX;
        s_line_trunc = 1u;
    }
    /* ★UTF-8 安全截断★ 若截到续字节(10xxxxxx)上, 往前退到字符起始。
     * 只在"确实截短了"时才有越界风险 —— n == s_line_len 时整行本来就是完整字符。 */
    while ((n > 0u) && (n < s_line_len) && ((((uint8_t)s_line[n]) & 0xC0u) == 0x80u))
    {
        n--;
    }

    if (n > 0u) { (void)memcpy(rec.text, s_line, n); }
    rec.len     = (uint8_t)n;
    rec.flags    = (s_line_trunc != 0u) ? 1u : 0u;
    rec.tick_ms = now;
    rec.ts      = (s_clock != NULL) ? s_clock() : 0u;

    s_line_len   = 0u;
    s_line_trunc = 0u;

    /* ---- 启动阶段：直接写盘 ----
     * 主循环这时还没跑，xf_log_poll() 一次都不会被调用，队列再大也装不下整个
     * 启动过程（实测 100+ 行）。启动阶段没有任何实时性要求，多花 100~200 ms
     * 把每条都写下去是划算的 —— 这段日志恰恰是现场取证最需要的。
     * s_writing 挡住 xf_ring_push 内部 log_info 的回喂。 */
    if (s_direct)
    {
        s_writing = 1u;
        if (!xf_ring_push(XF_RING_LOG, &rec, (uint16_t)sizeof(rec))) { s_dropped++; }
        s_writing = 0u;
        return;
    }

    /* ---- 稳态：入队，由主循环 xf_log_poll() 写盘 ---- */
    if (s_q_cnt >= XF_LOG_QDEPTH)
    {
        s_dropped++;
        return;
    }
    s_q[s_q_head] = rec;
    s_q_head      = (uint8_t)((s_q_head + 1u) % XF_LOG_QDEPTH);
    s_q_cnt++;
}

/* ==================== 对外接口 ==================== */

void xf_log_init(void)
{
    s_q_head = 0u;
    s_q_tail = 0u;
    s_q_cnt  = 0u;

    s_line_len   = 0u;
    s_line_trunc = 0u;

    s_writing = 0u;
    s_dropped = 0u;
    s_sec_cnt = 0u;
    s_sec_tick = get_tick_ms();
    s_direct = true;                    /* 启动阶段默认直写，由 app_boot 末尾关掉 */

    /* 芯片没焊/没起来时直接关掉，省掉每秒一次的无用重试 */
    s_enabled = xflash_present();
}

void xf_log_set_clock(uint32_t (*fn)(void))
{
    s_clock = fn;
}

void xf_log_feed_char(char c)
{
    if ((!s_enabled) || s_paused || (s_writing != 0u)) { return; }

    if (c == '\r') { return; }                  /* \r\n 只认 \n，避免多记一行 */

    if (c == '\n')
    {
        xf_log_commit_line();
        return;
    }

    if (s_line_len < XF_LOG_LINE_CAP)
    {
        s_line[s_line_len] = c;
        s_line_len++;
    }
    else
    {
        s_line_trunc = 1u;                      /* 超长：本行后续字符直接吃掉 */
    }
}

void xf_log_poll(void)
{
    uint8_t budget = XF_LOG_POLL_BUDGET;

    if ((!s_enabled) || (s_q_cnt == 0u)) { return; }
    if (s_writing != 0u) { return; }            /* 重入：上一次还没退出来 */

    while ((budget > 0u) && (s_q_cnt > 0u))
    {
        xf_log_rec_t rec = s_q[s_q_tail];
        bool ok;

        /* ★挡住 xf_ring_push 内部 log_info 引起的递归★ */
        s_writing = 1u;
        ok = xf_ring_push(XF_RING_LOG, &rec, (uint16_t)sizeof(rec));
        s_writing = 0u;

        if (!ok)
        {
            s_dropped++;
            break;                              /* 写失败就别把队列抽干, 下拍再试 */
        }

        s_q_tail = (uint8_t)((s_q_tail + 1u) % XF_LOG_QDEPTH);
        s_q_cnt--;
        budget--;
    }
}

uint32_t xf_log_pending(void)
{
    return (uint32_t)s_q_cnt;
}

uint32_t xf_log_count(void)
{
    return xf_ring_count(XF_RING_LOG);
}

uint32_t xf_log_dropped(void)
{
    return s_dropped;
}

void xf_log_dump(uint32_t n)
{
    uint32_t total;
    uint32_t i;

    /* dump 全程会 log_info()，必须挡住回喂 */
    s_writing = 1u;

    total = xf_ring_count(XF_RING_LOG);

    if (total == 0u)
    {
        log_info("[LOG] 日志环为空(共 0 条)\r\n");
        s_writing = 0u;
        return;
    }

    if ((n == 0u) || (n > total)) { n = total; }
    if (n > XF_LOG_MAX_DUMP) { n = XF_LOG_MAX_DUMP; }

    log_info("[LOG] 共 %lu 条, 打印最新 %lu 条(新 -> 旧) | 丢弃 %lu 未落盘\r\n",
             (unsigned long)total, (unsigned long)n,
             (unsigned long)s_dropped);

    for (i = 0u; i < n; i++)
    {
        xf_log_rec_t rec;
        uint16_t     len = 0u;
        char         text[XF_LOG_TEXT_MAX + 1u];
        char         when[20];
        uint8_t      l;

        if (!xf_ring_peek_at(XF_RING_LOG, (total - 1u - i), &rec,
                             (uint16_t)sizeof(rec), &len))
        {
            continue;                            /* 该槽无有效记录(掉电残留), 跳过 */
        }

        l = rec.len;
        if (l > XF_LOG_TEXT_MAX) { l = XF_LOG_TEXT_MAX; }
        (void)memcpy(text, rec.text, l);
        text[l] = '\0';

        if (rec.ts != 0u)
        {
            xf_log_fmt_ts(rec.ts, when, (uint32_t)sizeof(when));
            log_info("[LOG] #%lu %s %s%s\r\n",
                     (unsigned long)(total - i), when, text,
                     ((rec.flags & 0x01u) != 0u) ? "..." : "");
        }
        else
        {
            log_info("[LOG] #%lu +%lums %s%s\r\n",
                     (unsigned long)(total - i), (unsigned long)rec.tick_ms, text,
                     ((rec.flags & 0x01u) != 0u) ? "..." : "");
        }
    }

    s_writing = 0u;
}

bool xf_log_clear(void)
{
    bool ok;

    s_writing = 1u;
    ok = xf_ring_clear(XF_RING_LOG);
    s_writing = 0u;

    s_q_head = 0u;
    s_q_tail = 0u;
    s_q_cnt  = 0u;

    s_line_len   = 0u;
    s_line_trunc = 0u;

    return ok;
}

void xf_log_set_enable(bool on)
{
    s_enabled = on;
    if (!on)
    {
        /* 关掉时把已经攒着的半行也丢掉, 免得下次开时接着写 */
        s_line_len   = 0u;
        s_line_trunc = 0u;
        s_q_head = 0u;
        s_q_tail = 0u;
        s_q_cnt  = 0u;
    }
}

bool xf_log_enabled(void)
{
    return s_enabled;
}

void xf_log_set_direct(bool on)
{
    s_direct = on;
}

void xf_log_pause_capture(void)
{
    s_paused = true;
    /* 这半行也不要了 —— 恢复后从干净的行首开始，避免两条日志粘一起 */
    s_line_len   = 0u;
    s_line_trunc = 0u;
}

void xf_log_resume_capture(void)
{
    s_paused = false;
}

#else /* !EXT_FLASH_ENABLE ---- 没有外挂 Flash 的板子: 全部空实现 ---- */

void     xf_log_init(void) {}
void     xf_log_set_clock(uint32_t (*fn)(void)) { (void)fn; }
void     xf_log_feed_char(char c) { (void)c; }
void     xf_log_poll(void) {}
uint32_t xf_log_pending(void) { return 0u; }
uint32_t xf_log_count(void) { return 0u; }
uint32_t xf_log_dropped(void) { return 0u; }
void     xf_log_dump(uint32_t n) { (void)n; }
bool     xf_log_clear(void) { return false; }
void     xf_log_set_enable(bool on) { (void)on; }
bool     xf_log_enabled(void) { return false; }
void     xf_log_set_direct(bool on) { (void)on; }
void     xf_log_pause_capture(void) {}
void     xf_log_resume_capture(void) {}

#endif /* EXT_FLASH_ENABLE */
