/**
 * @file  fake_xflash.c
 * @brief 主机侧的外挂 Flash 模拟后端（8 MB 内存盘，**按 NOR 真实语义**）
 *
 * 为什么不在主机上直接编 xflash.c：
 *   那份代码是寄存器级的（SPI1/GPIOA），主机上根本跑不了；而 xflash_store.c
 *   （分区表 / 超级块 / 出厂固件区）是纯逻辑，**最该被单测覆盖的正是它**——
 *   它决定"掉电后能不能恢复"、"恢复出厂会不会读错镜像"。所以：
 *     · 主机测试只编 xflash_store.c + 本文件；
 *     · 本文件实现 xflash.h 的全部接口，但底下是内存盘。
 *
 * 关键：**写是 `new = old & data`，不是 memcpy**。
 *   NOR 只能把 1 写成 0，不擦就写会得到"旧 AND 新"的垃圾。用真语义模拟，
 *   "忘了擦"、"擦错区"、"跨页回卷"这类真板上最难查的 bug 会直接断言失败。
 */
#include "xflash.h"

#include <stdlib.h>
#include <string.h>

#define FX_CAP_DEFAULT  XFLASH_CAPACITY_8MB

static uint8_t *g_mem;
static uint32_t g_cap;
static bool     g_present = true;
static uint32_t g_err;
static uint32_t g_erase_cnt;

/* 故障注入：还能成功写多少字节；<0 = 不限制。0 时下一次写立刻失败（模拟掉电截断）。 */
static long     g_write_budget = -1;

/* ==================== 测试专用 ==================== */

/** 取内存盘指针（测试用来手工破坏/检查某个字节） */
uint8_t *fx_mem(void)
{
    if (g_mem == NULL)
    {
        g_cap = FX_CAP_DEFAULT;
        g_mem = (uint8_t *)malloc(g_cap);
        if (g_mem != NULL) { (void)memset(g_mem, 0xFF, g_cap); }
    }
    return g_mem;
}

/** 恢复成"刚出厂的空白芯片"（全 0xFF） */
void fx_blank(void)
{
    if (fx_mem() != NULL) { (void)memset(g_mem, 0xFF, g_cap); }
    g_err         = 0u;
    g_erase_cnt   = 0u;
    g_write_budget = -1;
    g_present     = true;
}

/** 模拟"没焊芯片" */
void fx_set_present(bool present)
{
    g_present = present;
}

/** 允许后续成功写入 n 字节；n=0 表示下一次写立即失败 */
void fx_set_write_budget(long n)
{
    g_write_budget = n;
}

uint32_t fx_erase_count(void)
{
    return g_erase_cnt;
}

/* ==================== xflash.h 的实现（内存后端） ==================== */

bool xflash_init(void)
{
    return (fx_mem() != NULL) && g_present;
}

bool xflash_present(void)
{
    return g_present && (g_mem != NULL);
}

uint32_t xflash_capacity(void)
{
    return xflash_present() ? g_cap : 0u;
}

const char *xflash_name(void)
{
    return "FAKE-8MB";
}

bool xflash_id(uint8_t out[3])
{
    if (out == NULL) { return false; }
    out[0] = 0xEFu;                 /* Winbond */
    out[1] = 0x40u;
    out[2] = 0x17u;                 /* 8 MB */
    return xflash_present();
}

bool xflash_read(uint32_t off, void *buf, uint32_t len)
{
    if ((!xflash_present()) || (buf == NULL) || (len == 0u)) { return false; }
    if ((off >= g_cap) || (len > (g_cap - off))) { return false; }

    (void)memcpy(buf, &g_mem[off], len);
    return true;
}

bool xflash_write(uint32_t off, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t       i;

    if ((!xflash_present()) || (buf == NULL) || (len == 0u)) { return false; }
    if ((off >= g_cap) || (len > (g_cap - off))) { return false; }

    for (i = 0u; i < len; i++)
    {
        if (g_write_budget == 0)
        {
            /* 掉电截断：已经写下去的部分【保留】，这正是真板的行为 */
            g_err++;
            return false;
        }
        if (g_write_budget > 0) { g_write_budget--; }

        g_mem[off + i] &= p[i];     /* ★ NOR 语义：只能 1 -> 0 ★ */
    }
    return true;
}

static bool fx_erase(uint32_t off, uint32_t size)
{
    if ((!xflash_present()) || ((off % size) != 0u) || (off >= g_cap)) { return false; }
    if (size > (g_cap - off)) { return false; }

    (void)memset(&g_mem[off], 0xFF, size);
    g_erase_cnt++;
    return true;
}

bool xflash_erase_4k(uint32_t off)
{
    return fx_erase(off, XFLASH_SECTOR_SIZE);
}

bool xflash_erase_64k(uint32_t off)
{
    return fx_erase(off, XFLASH_BLOCK_SIZE);
}

bool xflash_read_status(uint8_t *out)
{
    if ((out == NULL) || (!xflash_present())) { return false; }
    *out = 0x00u;
    return true;
}

bool xflash_wait_ready(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return xflash_present();
}

uint32_t xflash_error_count(void)
{
    return g_err;
}
