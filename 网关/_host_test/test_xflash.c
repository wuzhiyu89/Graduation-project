/**
 * @file  test_xflash.c
 * @brief 外挂 Flash 的分区表 / 超级块 / 出厂固件区 —— 主机单测
 *        （被测：app/drivers/xflash/xflash_store.c；后端：_host_test/fake_xflash.c）
 *
 * 为什么这套测试值得先写：
 *   板子还没到，但"分区表算错一格"、"两份超级块都写同一份"、"出厂镜像读越界"
 *   这类问题**必须在写业务代码之前钉死** —— 它们一旦进了现场，表现是
 *   "偶尔补报少了半天数据"或"恢复出厂后设备变砖"，几乎无法远程定位。
 *
 * 重点验七件：
 *   1) 分区表：连续、64 KB 对齐、首尾正好铺满 8 MB，没有重叠也没有空洞；
 *   2) 首次使用：空白芯片能自动格式化出 META，之后 ready；
 *   3) 超级块双份：seq 单调递增，破坏任一份仍能读出较新的一份；
 *   4) 两份都坏：明确返回 false（由上层走全环扫描，不能"假装有值"）；
 *   5) NOR 语义：不擦就写得到 AND 垃圾 —— 证明"先擦后写"不是可选项；
 *   6) 出厂固件区：空 → 不可用；写正文+头 → 可用且读回一致；越界/坏头必须拒绝；
 *   7) 掉电截断：写超级块写到一半失败，**另一份仍然可读**，不丢历史。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "xflash_store.h"
#include "utils.h"

/* fake_xflash.c 提供的测试钩子 */
uint8_t *fx_mem(void);
void     fx_blank(void);
void     fx_set_present(bool present);
void     fx_set_write_budget(long n);
uint32_t fx_erase_count(void);

static int g_fail;
static int g_pass;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) { g_pass++; printf("  ok   : "); }            \
        else      { g_fail++; printf("  FAIL : "); }            \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    } while (0)

/* ==================== 1. 分区表 ==================== */

static void test_partition_table(void)
{
    uint32_t i;
    uint32_t expect = 0u;
    bool     contig = true;
    bool     align  = true;
    bool     sizeok = true;

    printf("\n[1] 分区表几何\n");

    for (i = 0u; i < (uint32_t)XF_REGION_COUNT; i++)
    {
        const xf_region_info_t *r = xf_region_info((xf_region_t)i);

        if (r == NULL) { contig = false; break; }
        if (r->base != expect) { contig = false; }
        if ((r->base % XFLASH_BLOCK_SIZE) != 0u) { align = false; }
        if ((r->size == 0u) || ((r->size % XFLASH_BLOCK_SIZE) != 0u)) { sizeok = false; }
        expect = r->base + r->size;
    }

    CHECK(contig, "各区首尾相接、无空洞无重叠");
    CHECK(align,  "每个区起点都是 64 KB 对齐");
    CHECK(sizeok, "每个区大小都是 64 KB 的整数倍");
    CHECK(expect == 0x800000u, "整表正好铺满 8 MB(末尾=0x%06X)", (unsigned)expect);

    /* 关键区的大小/位置就是用户 2026-11-04 定的那版 */
    CHECK(xf_region_info(XF_REGION_EVENT)->size == 0x200000u, "EVENT = 2 MB");
    CHECK(xf_region_info(XF_REGION_OTA)->size == 0x080000u,   "OTA   = 512 KB");
    CHECK(xf_region_info(XF_REGION_FW_FACTORY)->size == 0x100000u, "FW_FACTORY = 1 MB");
    CHECK(xf_region_info(XF_REGION_TELEM)->size == 0x300000u, "TELEM = 3 MB");
    CHECK(xf_region_info(XF_REGION_FW_FACTORY)->base == 0x6A0000u,
          "FW_FACTORY 起点 = 0x6A0000(与文档一致)");
}

/* ==================== 2/3/4. 超级块 ==================== */

static void test_meta(void)
{
    xf_meta_t m;
    uint8_t  *mem;
    uint32_t  meta0;

    printf("\n[2] 首次使用：空白芯片自动格式化\n");

    fx_blank();
    CHECK(xf_store_init(), "xf_store_init() 在空白片上返回 true(自动建 META)");
    CHECK(xf_store_ready(), "之后 xf_store_ready() = true");
    CHECK(xf_meta_read(&m), "能立刻读出 META");
    CHECK(m.seq == 1u, "初始 seq = 1(实际 %u)", (unsigned)m.seq);
    CHECK(m.ring_head[XF_RING_EVENT] == 0u, "初始 EVENT head = 0");
    CHECK(m.fw_factory_valid == 0u, "初始 fw_factory_valid = 0");

    printf("\n[3] 超级块 16 槽轮转：seq 递增 + 坏几份仍可读\n");

    meta0 = xf_region_info(XF_REGION_META)->base;
    mem   = fx_mem();

    CHECK(xf_meta_write(&m), "第 2 次写 META 成功");
    CHECK(xf_meta_read(&m) && (m.seq == 2u), "seq 递增到 2(实际 %u)", (unsigned)m.seq);

    CHECK(xf_meta_write(&m), "第 3 次写 META 成功");
    CHECK(xf_meta_read(&m) && (m.seq == 3u), "seq 递增到 3(实际 %u)", (unsigned)m.seq);

    /* ★2026-11-11: 双份 -> 16 槽轮转★
     * 前三份分别落在槽 0/1/2，而且必须是【不同扇区】—— 否则"轮转省磨损"没意义。 */
    CHECK((mem[meta0] != 0xFFu) && (mem[meta0 + XF_META_SLOT_SIZE] != 0xFFu) &&
          (mem[meta0 + (2u * XF_META_SLOT_SIZE)] != 0xFFu),
          "前 3 次写落在槽 0/1/2（轮转，不是反复擦同一处）");
    CHECK((mem[meta0 + (3u * XF_META_SLOT_SIZE)] == 0xFFu),
          "第 4 个槽还没被用过");

    /* 破坏当前那一份（槽 2）—— 必须还能从别的槽读出来 */
    (void)memset(&mem[meta0 + (2u * XF_META_SLOT_SIZE)], 0x00, 8u);
    xf_meta_invalidate();                   /* 丢掉 RAM 缓存，强制重新扫盘 */
    CHECK(xf_meta_read(&m), "破坏 copy2 后仍能读出 META(走其它 15 份)");
    CHECK(m.seq == 2u, "读到的是次新的 seq=2(实际 %u)", (unsigned)m.seq);

    printf("\n[4] 16 份全坏：必须明确失败，不能编一个值\n");

    {
        uint8_t k;

        for (k = 0u; k < (uint8_t)XF_META_SLOTS; k++)
        {
            (void)memset(&mem[meta0 + ((uint32_t)k * XF_META_SLOT_SIZE)], 0x00, 8u);
        }
    }
    xf_meta_invalidate();
    CHECK(!xf_meta_read(&m), "全坏 -> xf_meta_read() = false");

    CHECK(xf_store_format(), "xf_store_format() 可重建 META");
    CHECK(xf_meta_read(&m) && (m.seq == 1u), "重建后 seq 回到 1(实际 %u)", (unsigned)m.seq);
}

/* ==================== 5. NOR 语义 ==================== */

static void test_nor_semantics(void)
{
    xf_region_info_t const *r = xf_region_info(XF_REGION_SPARE);
    uint8_t  buf[4];
    uint32_t before;

    printf("\n[5] NOR 语义：不擦就写 = AND 垃圾（证明必须先擦）\n");

    /* 不重铺整片：SPARE 区在 [2] 的 fx_blank() 之后仍是全 0xFF，够用了。
     * 这里再 fx_blank() 会把 META 一起抹掉，而 g_ready 已缓存为 true，
     * 后续套件就读不到超级块了 —— 那是夹具的问题，不是被测代码的问题。 */
    buf[0] = 0xF0u; buf[1] = 0x0Fu; buf[2] = 0xFFu; buf[3] = 0x00u;

    CHECK(xflash_write(r->base, buf, sizeof(buf)), "空白区写 4 字节成功");
    buf[0] = 0x0Fu; buf[1] = 0xF0u; buf[2] = 0x00u; buf[3] = 0xFFu;
    CHECK(xflash_write(r->base, buf, sizeof(buf)), "不擦再写一次(真板上允许，但结果是 AND)");

    (void)memset(buf, 0, sizeof(buf));
    CHECK(xflash_read(r->base, buf, sizeof(buf)), "读回");
    CHECK((buf[0] == 0x00u) && (buf[1] == 0x00u) && (buf[2] == 0x00u) && (buf[3] == 0x00u),
          "读回 = 旧 AND 新 = 00 00 00 00(实际 %02X %02X %02X %02X)",
          buf[0], buf[1], buf[2], buf[3]);

    before = fx_erase_count();
    CHECK(xflash_erase_4k(r->base), "擦 4 KB");
    CHECK(fx_erase_count() == (before + 1u), "擦除计数 +1");
    CHECK(xflash_read(r->base, buf, sizeof(buf)) && (buf[0] == 0xFFu), "擦后回到 0xFF");

    printf("\n[5b] 参数保护：非对齐/越界必须拒绝\n");
    CHECK(!xflash_erase_4k(r->base + 1u), "4 KB 擦除非对齐 -> 拒绝");
    CHECK(!xflash_erase_64k(r->base + 0x1000u), "64 KB 擦除非对齐 -> 拒绝");
    CHECK(!xflash_read(0x800000u, buf, 1u), "读起点越界 -> 拒绝");
    CHECK(!xflash_write(0x800000u - 2u, buf, 4u), "写跨末尾 -> 拒绝");
}

/* ==================== 6. 出厂固件区 ==================== */

static void test_fw_factory(void)
{
    xf_fw_header_t h;
    xf_region_info_t const *r = xf_region_info(XF_REGION_FW_FACTORY);
    uint8_t  img[512];
    uint8_t  back[512];
    uint32_t i;

    printf("\n[6] 出厂固件区\n");

    /* 同上：不 fx_blank()，否则会把 META 抹掉。出厂固件区初始为 0xFF（见 [2]）。 */
    CHECK(xf_store_ready(), "store 已就绪(前面套件初始化过)");
    CHECK(!xf_fw_factory_header(&h), "空白片：没有出厂固件头");
    CHECK(!xf_fw_factory_present(), "空白片：xf_fw_factory_present() = false");

    for (i = 0u; i < (uint32_t)sizeof(img); i++)
    {
        img[i] = (uint8_t)(i * 31u + 7u);
    }

    CHECK(xf_fw_factory_erase(), "擦除出厂固件区(16 个 64 KB 块)");
    CHECK(xf_fw_factory_write(0u, img, (uint32_t)sizeof(img)), "写入镜像正文 512 B");

    (void)memset(&h, 0, sizeof(h));
    h.size    = (uint32_t)sizeof(img);
    h.crc32   = 0x12345678u;
    h.version = 0x00010002u;
    (void)strcpy(h.ver_str, "V1.0.2");
    h.flags   = 0x0001u;
    CHECK(xf_fw_factory_set_header(&h), "写头(最后一步，写成功才算可用)");

    CHECK(xf_fw_factory_present(), "★ 现在 present = true ★");
    CHECK(xf_fw_factory_header(&h) && (h.size == (uint32_t)sizeof(img)) &&
          (h.crc32 == 0x12345678u) && (strcmp(h.ver_str, "V1.0.2") == 0),
          "头字段完整往返(size=%u ver=%s)", (unsigned)h.size, h.ver_str);

    (void)memset(back, 0, sizeof(back));
    CHECK(xf_fw_factory_read(0u, back, (uint32_t)sizeof(back)), "读回镜像正文");
    CHECK(memcmp(img, back, sizeof(img)) == 0, "★ 镜像 512 B 逐字节一致(头没覆盖正文)★");

    printf("\n[6b] 越界与坏头必须拒绝\n");
    CHECK(!xf_fw_factory_read((uint32_t)sizeof(img), back, 1u), "读越过镜像长度 -> 拒绝");
    CHECK(!xf_fw_factory_read(0u, back, (uint32_t)sizeof(img) + 1u), "读长度超过镜像 -> 拒绝");
    CHECK(!xf_fw_factory_write(0u, img, r->size), "写长度超过区容量 -> 拒绝");

    /* 手工把头里的 hdr_crc 改坏：必须判为不可用，而不是"读完再崩" */
    {
        xf_fw_header_t bad;
        uint8_t       *mem = fx_mem();

        (void)memcpy(&bad, &mem[r->base], sizeof(bad));
        bad.hdr_crc ^= 0xFFu;
        (void)xflash_erase_4k(r->base);
        CHECK(xflash_write(r->base, &bad, (uint32_t)sizeof(bad)), "写入被篡改的头");
        CHECK(!xf_fw_factory_header(&bad), "★ hdr_crc 不匹配 -> 拒绝(不会拿坏镜像去刷机)★");

        /* size 超出区容量 */
        bad.hdr_crc     = 0u;
        bad.size        = r->size;      /* 超了(还要再减 4 KB 头扇区) */
        bad.magic       = XF_FW_MAGIC;
        bad.header_size = (uint16_t)sizeof(bad);
        (void)xflash_erase_4k(r->base);
        (void)xflash_write(r->base, &bad, (uint32_t)sizeof(bad));
        CHECK(!xf_fw_factory_header(&bad), "size 超出区容量 -> 拒绝");
    }
}

/* ==================== 7. 掉电截断 ==================== */

static void test_power_loss(void)
{
    xf_meta_t m;
    xf_meta_t got;
    uint8_t   *mem;
    uint32_t   meta0;
    uint32_t   seq0;

    printf("\n[7] 掉电截断：写超级块写到一半失败，另一份必须还能读\n");

    /* 相对判断，不假设当前 seq 是几（前面的套件已经推进过它） */
    CHECK(xf_meta_read(&m), "先读到一个有效的 META");
    seq0 = m.seq;

    CHECK(xf_meta_write(&m) && (m.seq == (seq0 + 1u)),
          "正常写一次 -> seq = %u(实际 %u)", (unsigned)(seq0 + 1u), (unsigned)m.seq);

    /* 下一次写：只允许写 6 字节就"掉电"（magic 写下了，后半截没了） */
    fx_set_write_budget(6);
    CHECK(!xf_meta_write(&m), "★ 写到一半掉电 -> xf_meta_write() 明确返回 false ★");
    fx_set_write_budget(-1);

    (void)memset(&got, 0, sizeof(got));
    CHECK(xf_meta_read(&got), "★ 但另一份完整，仍然读得出 META ★");
    CHECK(got.seq == (seq0 + 1u), "读到的是掉电前那一版 seq=%u(实际 %u)",
          (unsigned)(seq0 + 1u), (unsigned)got.seq);

    meta0 = xf_region_info(XF_REGION_META)->base;
    mem   = fx_mem();
    CHECK(mem != NULL, "内存盘可用(meta0=0x%06X)", (unsigned)meta0);

    /* 恢复后还能继续正常写（不能因为一次失败就永久废掉） */
    CHECK(xf_meta_write(&got), "掉电后下一次写恢复正常");
    CHECK(xf_meta_read(&got) && (got.seq >= (seq0 + 2u)), "seq 继续往前(%u)", (unsigned)got.seq);
}

/* ==================== 8. 芯片不在 ==================== */

static void test_no_chip(void)
{
    xf_meta_t m;
    uint8_t   b[4];

    printf("\n[8] 没焊芯片：全部 no-op，绝不影响主流程\n");

    fx_blank();
    fx_set_present(false);

    CHECK(!xflash_present(), "xflash_present() = false");
    CHECK(xflash_capacity() == 0u, "容量 = 0");
    CHECK(!xf_store_init(), "xf_store_init() = false(调用方按'增强不可用'处理)");
    CHECK(!xf_store_ready(), "ready = false");
    CHECK(!xf_meta_read(&m), "读 META -> false");
    CHECK(!xf_fw_factory_present(), "出厂固件 -> false");
    CHECK(!xflash_read(0u, b, sizeof(b)), "读 -> false");
    CHECK(!xflash_write(0u, b, sizeof(b)), "写 -> false");
    CHECK(!xflash_erase_4k(0u), "擦 -> false");

    fx_set_present(true);
}

/* ==================== 8. 数据环（事件/遥测溢出层） ==================== */

static void test_ring(void)
{
    uint8_t  rec[52];
    uint8_t  out[52];
    uint16_t len;
    uint32_t i;
    uint32_t dropped0;
    xf_meta_t m;

    printf("\n[8] 数据环: 进出行序 / 环满淘汰 / 掉电落盘 / crc 自愈\n");

    CHECK(xf_ring_format_all(), "格式化三个数据环");
    CHECK((xf_ring_used(XF_RING_EVENT) == 0u) && (xf_ring_used(XF_RING_TELEM) == 0u),
          "格式化后 used=0");

    /* ---- 1) 基本进出行序 ---- */
    for (i = 0u; i < 5u; i++)
    {
        (void)memset(rec, (int)(0xA0u + i), sizeof(rec));
        rec[0] = (uint8_t)i;
        CHECK(xf_ring_push(XF_RING_EVENT, rec, 32u), "push #%u", (unsigned)i);
    }
    for (i = 0u; i < 5u; i++)
    {
        len = 0u;
        CHECK(xf_ring_peek(XF_RING_EVENT, out, sizeof(out), &len) && (len == 32u) && (out[0] == i),
              "peek 第 %u 条(最旧先出)", (unsigned)i);
        CHECK(xf_ring_pop(XF_RING_EVENT, out, sizeof(out), &len), "pop 第 %u 条", (unsigned)i);
    }
    len = 0u;
    CHECK(!xf_ring_peek(XF_RING_EVENT, out, sizeof(out), &len), "空环 peek -> false");

    /* ---- 2) 掉电落盘: flush 后 META 里的 head/tail 与 RAM 一致 ---- */
    (void)memset(rec, 0x5A, sizeof(rec));
    CHECK(xf_ring_push(XF_RING_EVENT, rec, 52u), "再 push 一条");
    CHECK(xf_ring_flush(), "flush META");
    CHECK(xf_meta_read(&m), "读回 META");
    CHECK((m.ring_head[XF_RING_EVENT] == (uint32_t)XF_SLOT_SIZE) &&
          (m.ring_tail[XF_RING_EVENT] == 0u),
          "META head=%u tail=%u 与 RAM 一致",
          (unsigned)m.ring_head[XF_RING_EVENT], (unsigned)m.ring_tail[XF_RING_EVENT]);

    /* ---- 3) 环满: 淘汰最旧 + dropped 增加（不是丢当前这条） ---- */
    CHECK(xf_ring_format_all(), "再次格式化");
    dropped0 = xf_ring_dropped(XF_RING_EVENT);
    for (i = 0u; i < 60000u; i++)
    {
        (void)memset(rec, 0x11, sizeof(rec));
        rec[0] = (uint8_t)(i & 0xFFu);
        if (!xf_ring_push(XF_RING_EVENT, rec, 52u))
        {
            break;
        }
    }
    CHECK(xf_ring_dropped(XF_RING_EVENT) > dropped0,
          "环满 -> 淘汰最旧 dropped=%u(起始 %u)",
          (unsigned)xf_ring_dropped(XF_RING_EVENT), (unsigned)dropped0);
    len = 0u;
    CHECK(xf_ring_peek(XF_RING_EVENT, out, sizeof(out), &len) && (len == 52u),
          "环满之后仍能读出一条完整记录");

    /* ---- 4) crc 自愈: 人为改坏倒数第二个字节区(故意写坏最旧一条的负载) ---- */
    {
        uint8_t *mem = fx_mem();
        uint32_t base = 0x020000u;   /* XF_REGION_EVENT 基址 */
        uint32_t tail;

        /* 先把环指针落 META —— 否则读到的 tail 还是格式化时的旧值,
         * 改坏的是"空位置", 当然测不出 crc 自愈。 */
        CHECK(xf_ring_flush(), "落 META(取最新 tail)");
        CHECK(xf_meta_read(&m), "读 META 取 tail");
        tail = m.ring_tail[XF_RING_EVENT];
        if (mem != NULL)
        {
            mem[base + tail + XF_REC_HDR_SIZE + 3u] ^= 0xFFu;   /* 改坏负载 */
            len = 0u;
            CHECK(!xf_ring_peek(XF_RING_EVENT, out, sizeof(out), &len),
                  "负载被改坏 -> peek 检出 crc 不符并丢弃");
            CHECK(xf_ring_peek(XF_RING_EVENT, out, sizeof(out), &len),
                  "下一条仍然可读(自愈)");
        }
    }

    CHECK(xf_ring_format_all(), "收尾: 清空数据环");
}

int main(void)
{
    printf("==== test_xflash: 外挂 Flash 分区/超级块/出厂固件区 ====\n");

    test_partition_table();
    test_no_chip();          /* ★必须最先跑：验证"首次初始化就没芯片"的真实路径 ★ */
    test_meta();
    test_nor_semantics();
    test_power_loss();
    test_fw_factory();
    test_ring();

    printf("\n==== 通过 %d, 失败 %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
