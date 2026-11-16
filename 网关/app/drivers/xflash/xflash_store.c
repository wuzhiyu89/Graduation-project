/**
 * @file  xflash_store.c
 * @brief 分区表 + 超级块(META 双份) + 出厂固件区
 *
 * 这里只做"空间管理"，不碰业务数据格式（遥测/事件的记录结构仍由
 * telemetry_cache / event_cache 决定）。分层理由见 xflash_store.h 顶部。
 */
#include  "xflash_store.h"

#include <string.h>
#include "utils.h"
#include "log.h"
#include "bsp_tim.h"        /* get_tick_ms —— 数据环的 META 落盘节流用 */

/* ==================== 分区表（8 MB，64 KB 对齐） ==================== */
/* ⚠ 改这里必须同时改 docs/外挂SPI-Flash空间规划.md，否则文档与固件说的不是一回事 */
static const xf_region_info_t s_regions[XF_REGION_COUNT] =
{
    /* name            base          size      */
    { "META",         0x000000u,    0x010000u },   /*   64 KB, 1 块 */
    { "CFG",          0x010000u,    0x010000u },   /*   64 KB, 1 块 */
    { "EVENT",        0x020000u,    0x200000u },   /*    2 MB, 32 块 */
    { "TELEM",        0x220000u,    0x300000u },   /*    3 MB, 48 块 */
    { "LOG",          0x520000u,    0x100000u },   /*    1 MB, 16 块 */
    { "OTA",          0x620000u,    0x080000u },   /*  512 KB, 8 块 */
    { "FW_FACTORY",   0x6A0000u,    0x100000u },   /*    1 MB, 16 块 ★出厂固件 */
    { "FACTORY",      0x7A0000u,    0x010000u },   /*   64 KB, 1 块 */
    { "SPARE",        0x7B0000u,    0x050000u }    /*  320 KB, 5 块 */
};

#define XF_TOTAL_SIZE   (8u * 1024u * 1024u)

static bool g_ready;

/* 超级块的 RAM 缓存 —— 16 槽轮转后"每次写都先扫一遍"会很慢(64 KB SPI 读 ≈ 90 ms),
 * 而落盘是每 30 s 一次的高频动作。所以开机扫一次, 之后以这里为准。 */
static xf_meta_t s_meta;
static uint8_t   s_meta_slot;       /* s_meta 所在的槽号 0..XF_META_SLOTS-1 */
static bool      s_meta_ok;         /* s_meta 是否有效 */

/* 数据环相关（实现放在文件末尾，前面 init 要用） */
static void xf_ring_load(void);

/* ==================== 内部工具 ==================== */

static uint32_t xf_meta_crc(const xf_meta_t *m)
{
    return crc32_ieee((const uint8_t *)m, (uint32_t)(sizeof(xf_meta_t) - sizeof(uint32_t)));
}

static bool xf_meta_valid(const xf_meta_t *m)
{
    if (m->magic != XF_META_MAGIC) { return false; }
    if (m->version != XF_META_VERSION) { return false; }
    if (m->size != (uint16_t)sizeof(xf_meta_t)) { return false; }
    return (m->crc == xf_meta_crc(m));
}

/** 读第 idx 个槽（0..XF_META_SLOTS-1）；返回该槽是否有效 */
static bool xf_meta_read_copy(uint8_t idx, xf_meta_t *out)
{
    uint32_t off = s_regions[XF_REGION_META].base + ((uint32_t)idx * XF_META_SLOT_SIZE);

    if (!xflash_read(off, out, (uint32_t)sizeof(*out))) { return false; }
    return xf_meta_valid(out);
}

/**
 * @brief 扫全部 16 个槽，取 seq 最大且 CRC 通过的那一份
 * @param out_slot 非 NULL 时回传它所在的槽号
 * @note  只在开机、或 RAM 缓存失效（某次写失败）时调用。64 KB SPI 读 ≈ 90 ms。
 */
static bool xf_meta_scan(xf_meta_t *out, uint8_t *out_slot)
{
    xf_meta_t cur;
    xf_meta_t best;
    uint8_t   i;
    uint8_t   best_slot = 0u;
    bool      any = false;

    (void)memset(&best, 0, sizeof(best));

    for (i = 0u; i < (uint8_t)XF_META_SLOTS; i++)
    {
        (void)memset(&cur, 0, sizeof(cur));
        if (!xf_meta_read_copy(i, &cur)) { continue; }
        if ((!any) || (cur.seq > best.seq))
        {
            best      = cur;
            best_slot = i;
            any       = true;
        }
    }

    if (!any) { return false; }

    if (out != NULL)      { *out = best; }
    if (out_slot != NULL) { *out_slot = best_slot; }
    return true;
}

const xf_region_info_t *xf_region_info(xf_region_t r)
{
    if ((uint32_t)r >= (uint32_t)XF_REGION_COUNT) { return NULL; }
    return &s_regions[r];
}

/* ==================== 超级块 ==================== */

bool xf_meta_read(xf_meta_t *out)
{
    if ((out == NULL) || (!xflash_present())) { return false; }

    /* 命中 RAM 缓存：绝大多数调用走这里，不碰 SPI */
    if (s_meta_ok)
    {
        *out = s_meta;
        return true;
    }

    if (!xf_meta_scan(&s_meta, &s_meta_slot)) { return false; }

    s_meta_ok = true;
    *out      = s_meta;
    return true;
}

void xf_meta_invalidate(void)
{
    s_meta_ok = false;
}

bool xf_meta_write(xf_meta_t *m)
{
    xf_meta_t base;
    xf_meta_t chk;
    uint32_t  off;
    uint32_t  seq;
    uint8_t   target;

    if ((m == NULL) || (!xflash_present())) { return false; }

    /* 基准：RAM 缓存优先；没有就扫一遍（可能刚上电/上次写失败） */
    if (s_meta_ok)
    {
        base = s_meta;
    }
    else if (!xf_meta_scan(&base, &s_meta_slot))
    {
        (void)memset(&base, 0, sizeof(base));
        base.seq    = 0u;
        s_meta_slot = (uint8_t)(XF_META_SLOTS - 1u);   /* 下一个正好是槽 0 */
    }

    /* ★轮转：永远写"上一个有效槽"的下一个槽★
     * 这样 16 个扇区轮流被擦，单槽擦除次数只有"双份交替"的 1/8；
     * 任何时刻至少还有 15 份完好，掉电发生在擦/写中间也不会全部失效。 */
    target = (uint8_t)(((uint32_t)s_meta_slot + 1u) % XF_META_SLOTS);

    m->magic   = XF_META_MAGIC;
    m->version = XF_META_VERSION;
    m->size    = (uint16_t)sizeof(xf_meta_t);

    seq = base.seq + 1u;                /* 调用方传的 seq 不作数，防止版本回退 */
    if (seq == 0u) { seq = 1u; }
    m->seq = seq;
    m->crc = xf_meta_crc(m);

    off = s_regions[XF_REGION_META].base + ((uint32_t)target * XF_META_SLOT_SIZE);

    if (!xflash_erase_4k(off))
    {
        s_meta_ok   = false;            /* 下一次重新扫；同时换一个槽再试 */
        s_meta_slot = target;
        return false;
    }
    if (!xflash_write(off, m, (uint32_t)sizeof(*m)))
    {
        s_meta_ok   = false;
        s_meta_slot = target;
        return false;
    }

    /* 回读校验：写完不验等于没写（掉电/坏块都在这时候暴露） */
    (void)memset(&chk, 0, sizeof(chk));
    if (!xflash_read(off, &chk, (uint32_t)sizeof(chk)))
    {
        s_meta_ok   = false;
        s_meta_slot = target;
        return false;
    }
    if (!xf_meta_valid(&chk) || (chk.seq != m->seq))
    {
        s_meta_ok   = false;
        s_meta_slot = target;
        return false;
    }

    s_meta      = *m;
    s_meta_slot = target;
    s_meta_ok   = true;
    return true;
}

bool xf_store_format(void)
{
    xf_meta_t m;
    xf_meta_t old;
    uint32_t  base;
    uint32_t  seq = 1u;

    if (!xflash_present()) { return false; }

    /* ★不复位到 seq=1★：盘上若还有 seq 更大的有效份，读的时候会把它选中，
     *   格式化等于白做。所以取"现有最大 + 1"，也就不用把 16 个槽全擦一遍
     *   （16 × 45 ms 的擦除会把开机拖到秒级）。 */
    if (xf_meta_scan(&old, NULL))
    {
        seq = old.seq + 1u;
        if (seq == 0u) { seq = 1u; }
    }

    base = s_regions[XF_REGION_META].base;

    if (!xflash_erase_4k(base)) { s_meta_ok = false; return false; }

    (void)memset(&m, 0, sizeof(m));
    m.magic   = XF_META_MAGIC;
    m.version = XF_META_VERSION;
    m.size    = (uint16_t)sizeof(xf_meta_t);
    m.seq     = seq;
    m.crc     = xf_meta_crc(&m);

    if (!xflash_write(base, &m, (uint32_t)sizeof(m))) { s_meta_ok = false; return false; }

    s_meta      = m;
    s_meta_slot = 0u;
    s_meta_ok   = true;

    log_info("[XFLASH] META 已格式化(seq=%u)\r\n", (unsigned)seq);
    return true;
}

bool xf_store_init(void)
{
    xf_meta_t m;

    if (g_ready) { return true; }

    if (!xflash_init())
    {
        return false;               /* 老板/没焊芯片 —— 正常路径，不报错 */
    }

    if (!xf_meta_read(&m))
    {
        log_info("[XFLASH] META 无效(首次使用或损坏), 执行格式化...\r\n");
        if (!xf_store_format()) { return false; }
    }
    else
    {
        log_info("[XFLASH] META 有效: seq=%u, EVENT head=%u tail=%u / TELEM head=%u tail=%u\r\n",
                 (unsigned)m.seq,
                 (unsigned)m.ring_head[XF_RING_EVENT], (unsigned)m.ring_tail[XF_RING_EVENT],
                 (unsigned)m.ring_head[XF_RING_TELEM], (unsigned)m.ring_tail[XF_RING_TELEM]);
    }

    /* 出厂固件区是否有可用镜像（只读检查，不会写） */
    {
        xf_fw_header_t h;

        if (xf_fw_factory_header(&h))
        {
            log_info("[XFLASH] 出厂固件: %s, %u KB, crc32=0x%08X\r\n",
                     (h.ver_str[0] != '\0') ? h.ver_str : "(无版本串)",
                     (unsigned)(h.size / 1024u), (unsigned)h.crc32);
        }
        else
        {
            log_info("[XFLASH] 出厂固件区为空(产线尚未写入)\r\n");
        }
    }

    /* 数据环: 从 META 恢复 head/tail（EVENT/TELEM/LOG 的断点续写位置） */
    xf_ring_load();

    g_ready = true;
    return true;
}

bool xf_store_ready(void)
{
    return g_ready;
}

void xf_store_dump(void)
{
    uint8_t         i;
    uint8_t         id[3] = { 0u, 0u, 0u };
    xf_meta_t       m;
    xf_fw_header_t  h;

    log_info("[XFLASH] EXT_FLASH_ENABLE=%d, present=%d, ready=%d\r\n",
             (int)EXT_FLASH_ENABLE, (int)xflash_present(), (int)g_ready);

    if (!xflash_present())
    {
        log_info("[XFLASH] 芯片不可用(未启用/未焊/ID 不认), 分区表仅供参考:\r\n");
    }
    else
    {
        (void)xflash_id(id);
        log_info("[XFLASH] 芯片=%s JEDEC=%02X %02X %02X 容量=%u KB 错误计数=%u\r\n",
                 xflash_name(), id[0], id[1], id[2],
                 (unsigned)(xflash_capacity() / 1024u),
                 (unsigned)xflash_error_count());
    }

    log_info("[XFLASH] 分区表(8 MB):\r\n");
    for (i = 0u; i < (uint8_t)XF_REGION_COUNT; i++)
    {
        log_info("[XFLASH]   %-11s base=0x%06X size=%6u KB (%u 块)\r\n",
                 s_regions[i].name,
                 (unsigned)s_regions[i].base,
                 (unsigned)(s_regions[i].size / 1024u),
                 (unsigned)(s_regions[i].size / XFLASH_BLOCK_SIZE));
    }

    if (xf_meta_read(&m))
    {
        /* ★2026-11-11: META 是 16 槽轮转, 打印"现在用的是哪一槽" ——
         * 现场排查"是不是总擦同一个扇区"时, 这个数字就是答案。 */
        log_info("[XFLASH] META seq=%u 槽=%u/%u\r\n",
                 (unsigned)m.seq, (unsigned)(s_meta_slot + 1u), (unsigned)XF_META_SLOTS);
        log_info("[XFLASH]   EVENT %u/%u 条  head=%u tail=%u dropped=%u\r\n",
                 (unsigned)xf_ring_count(XF_RING_EVENT),
                 (unsigned)(s_regions[XF_REGION_EVENT].size / XF_SLOT_SIZE),
                 (unsigned)m.ring_head[XF_RING_EVENT], (unsigned)m.ring_tail[XF_RING_EVENT],
                 (unsigned)m.ring_dropped[XF_RING_EVENT]);
        log_info("[XFLASH]   TELEM %u/%u 条  head=%u tail=%u dropped=%u\r\n",
                 (unsigned)xf_ring_count(XF_RING_TELEM),
                 (unsigned)(s_regions[XF_REGION_TELEM].size / XF_SLOT_SIZE),
                 (unsigned)m.ring_head[XF_RING_TELEM], (unsigned)m.ring_tail[XF_RING_TELEM],
                 (unsigned)m.ring_dropped[XF_RING_TELEM]);
        log_info("[XFLASH]   LOG   head=%u tail=%u dropped=%u\r\n",
                 (unsigned)m.ring_head[XF_RING_LOG], (unsigned)m.ring_tail[XF_RING_LOG],
                 (unsigned)m.ring_dropped[XF_RING_LOG]);
    }
    else
    {
        log_info("[XFLASH] META 不可读\r\n");
    }

    if (xf_fw_factory_header(&h))
    {
        log_info("[XFLASH] 出厂固件: ver=%s size=%u KB crc32=0x%08X flags=0x%04X\r\n",
                 h.ver_str, (unsigned)(h.size / 1024u), (unsigned)h.crc32, (unsigned)h.flags);
    }
    else
    {
        log_info("[XFLASH] 出厂固件: 无\r\n");
    }
}

/* ==================== 出厂固件区 ==================== */

static uint32_t xf_fw_crc(const xf_fw_header_t *h)
{
    return crc32_ieee((const uint8_t *)h, (uint32_t)(sizeof(xf_fw_header_t) - sizeof(uint32_t)));
}

bool xf_fw_factory_header(xf_fw_header_t *out)
{
    uint32_t base = s_regions[XF_REGION_FW_FACTORY].base;

    if ((out == NULL) || (!xflash_present())) { return false; }
    (void)memset(out, 0, sizeof(*out));

    if (!xflash_read(base, out, (uint32_t)sizeof(*out))) { return false; }

    if (out->magic != XF_FW_MAGIC) { return false; }
    if (out->header_size != (uint16_t)sizeof(xf_fw_header_t)) { return false; }
    if (out->crc32 == 0xFFFFFFFFu) { return false; }        /* 空片 */
    if (out->hdr_crc != xf_fw_crc(out)) { return false; }
    if (out->size == 0u) { return false; }
    if ((out->size + XF_FW_HDR_SLOT) > s_regions[XF_REGION_FW_FACTORY].size)
    {
        return false;
    }
    return true;
}

bool xf_fw_factory_present(void)
{
    xf_fw_header_t h;

    if (!xf_fw_factory_header(&h)) { return false; }
    return ((h.flags & 0x0001u) != 0u);
}

bool xf_fw_factory_read(uint32_t off, void *buf, uint32_t len)
{
    xf_fw_header_t h;
    uint32_t       base = s_regions[XF_REGION_FW_FACTORY].base + XF_FW_HDR_SLOT;

    if ((buf == NULL) || (len == 0u)) { return false; }
    if (!xf_fw_factory_header(&h)) { return false; }
    if ((off > h.size) || (len > (h.size - off))) { return false; }

    return xflash_read(base + off, buf, len);
}

bool xf_fw_factory_erase(void)
{
    uint32_t base  = s_regions[XF_REGION_FW_FACTORY].base;
    uint32_t nblk  = s_regions[XF_REGION_FW_FACTORY].size / XFLASH_BLOCK_SIZE;
    uint32_t i;

    if (!xflash_present()) { return false; }

    for (i = 0u; i < nblk; i++)
    {
        if (!xflash_erase_64k(base + (i * XFLASH_BLOCK_SIZE))) { return false; }
    }
    return true;
}

bool xf_fw_factory_write(uint32_t off, const void *buf, uint32_t len)
{
    uint32_t base = s_regions[XF_REGION_FW_FACTORY].base + XF_FW_HDR_SLOT;
    uint32_t maxs = s_regions[XF_REGION_FW_FACTORY].size - XF_FW_HDR_SLOT;

    if ((!xflash_present()) || (buf == NULL) || (len == 0u)) { return false; }
    if ((off > maxs) || (len > (maxs - off))) { return false; }

    return xflash_write(base + off, buf, len);
}

bool xf_fw_factory_set_header(const xf_fw_header_t *h)
{
    xf_fw_header_t tmp;
    uint32_t       base = s_regions[XF_REGION_FW_FACTORY].base;

    if ((h == NULL) || (!xflash_present())) { return false; }

    tmp = *h;
    tmp.magic       = XF_FW_MAGIC;
    tmp.header_size = (uint16_t)sizeof(xf_fw_header_t);
    tmp.crc32       = h->crc32;
    tmp.hdr_crc     = xf_fw_crc(&tmp);

    if (!xflash_erase_4k(base)) { return false; }
    return xflash_write(base, &tmp, (uint32_t)sizeof(tmp));
}

/* 未启用时也要提供符号，避免链接期报错（本文件的函数体已全部先判 present） */
void xf_store_noop_anchor(void)
{
    (void)g_ready;
    (void)XF_TOTAL_SIZE;
}

/* ========================================================================
 * 数据环形区（EVENT / TELEM / LOG）—— 2026-11-10
 * ========================================================================
 * 【和超级块的分工】META 里存 head/tail（掉电恢复要靠它），但**不是每条都写**：
 *   每写一条就擦一次 4 KB META 扇区，几天就能把 10 万次擦写寿命磨完。
 *   所以只在"空闲 1 s 后"由 xf_ring_poll() 落一次；硬掉电最多丢最后约 1 s
 *   的几条环记录（RAM 里那 256/128 条本来也会丢，属于同一量级）。
 *
 * 【掉电残留怎么处理】记录自带 magic + crc：
 *   · 扫描/读取时 magic 不对 → 该位置没有有效记录，环按空处理（自愈）；
 *   · crc 不对 → 这条是被掉电截断的半条，跳过它（tail 前移到下一条）。
 * ===================================================================== */
typedef struct
{
    uint16_t magic;         /* XF_REC_MAGIC */
    uint16_t len;           /* payload 字节数 */
    uint32_t seq;           /* 递增序号（仅用于调试排序，掉电后可能回绕） */
    uint32_t crc;           /* crc32_ieee(payload) */
} xf_rec_hdr_t;             /* 12 B，字段自然对齐，无填充 */

typedef struct
{
    uint32_t head;          /* 写位置（相对环起点） */
    uint32_t tail;          /* 最旧记录位置 */
    uint32_t used;          /* 已占用字节（含头与对齐填充） */
    uint32_t dropped;       /* 因满淘汰的累计条数 */
    uint32_t seq;           /* 下一条记录的序号 */
    uint32_t last_sector;   /* 最近一次擦/写的扇区号；0xFFFFFFFF = 下一个扇区要先擦 */
} xf_ring_t;

static xf_ring_t s_ring[XF_RING_COUNT];
static bool      s_ring_ready;
static bool      s_ring_dirty;      /* head/tail 有变化，待落 META */
static uint32_t  s_ring_dirty_ms;   /* 最后一次变化的后台 tick */

/* 定义在后面（peek/peek_at 共用；xf_ring_load 的恢复扫描也要用） */
static bool xf_ring_read_hdr(xf_ring_id_t id, uint32_t off, xf_rec_hdr_t *h);

#define XF_RING_FLUSH_IDLE_MS   30000u   /* head/tail 空闲多久后落 META */
#define XF_RING_RECOVER_MAX     64u      /* 开机最多补回多少条（防御性上限） */

static uint32_t xf_ring_region(xf_ring_id_t id)
{
    /* XF_RING_EVENT/TELEM/LOG 与 XF_REGION_EVENT/TELEM/LOG 顺序一致 */
    return (uint32_t)XF_REGION_EVENT + (uint32_t)id;
}

/**
 * @brief 开机恢复：从 META 的 head 向前数"已经写进去、但指针没来得及落盘"的记录
 * @return 补回的条数
 *
 * 【为什么需要它】为了不把 META 擦爆，head 是空闲 30 s 才落一次盘。这期间记录
 *   已经写进 Flash 了，只是指针还停在旧位置。开机若直接信 META，这些记录会被
 *   当成"空闲空间"在下一轮覆盖掉 —— 白写。
 *   记录自带 magic + len + crc，正好可以"顺着往下数"，数到第一条无效为止。
 *
 * 【为什么只在 head 不在扇区边界时扫】
 *   写指针进入新扇区前会先擦整个 4 KB（NOR 只能 1->0），所以"当前扇区里 head
 *   之后的槽"一定是 0xFF（空）。但 head 正好停在扇区边界时，下一个扇区可能还是
 *   上一轮的旧数据 —— 扫进去会把早已补报过的旧记录当成新记录，那比重发更糟。
 *   此时不扫，代价是丢掉最后一拍（≈30 s，按 0.8 条/分算不到 1 条）。
 */
static uint32_t xf_ring_recover_head(xf_ring_id_t id, uint32_t head, uint32_t size)
{
    uint32_t     base = s_regions[xf_ring_region(id)].base;
    uint32_t     off  = head;
    uint32_t     n    = 0u;
    uint8_t      buf[XF_REC_MAX_PAYLOAD];
    xf_rec_hdr_t h;

    while ((n < XF_RING_RECOVER_MAX) && ((off % XFLASH_SECTOR_SIZE) != 0u))
    {
        if (!xf_ring_read_hdr(id, off, &h)) { break; }
        if (h.len > (uint16_t)sizeof(buf))  { break; }
        if (!xflash_read(base + off + XF_REC_HDR_SIZE, buf, (uint32_t)h.len)) { break; }
        if (crc32_ieee(buf, (uint32_t)h.len) != h.crc) { break; }   /* 半条（掉电截断） */

        off = (off + XF_SLOT_SIZE) % size;
        n++;
    }

    return n;
}

/* 从 META 装载三个环的 head/tail（boot 调一次） */
static void xf_ring_load(void)
{
    xf_meta_t m;
    uint8_t   i;

    (void)memset(s_ring, 0, sizeof(s_ring));

    if (!xf_meta_read(&m))
    {
        s_ring_ready = false;
        return;
    }

    for (i = 0u; i < (uint8_t)XF_RING_COUNT; i++)
    {
        uint32_t size = s_regions[xf_ring_region((xf_ring_id_t)i)].size;
        uint32_t h    = m.ring_head[i];
        uint32_t t    = m.ring_tail[i];
        uint32_t add;

        if ((h >= size) || (t >= size)) { h = 0u; t = 0u; }

        s_ring[i].head    = h;
        s_ring[i].tail    = t;
        s_ring[i].dropped = m.ring_dropped[i];
        s_ring[i].used    = (h >= t) ? (h - t) : (size - t + h);
        s_ring[i].seq     = 1u;

        /* 从中间续写：当前扇区里的旧记录必须保留，不能再擦；但若正好停在
         * 扇区边界，那个扇区可能是上一轮写满的旧数据 -> 强制先擦。 */
        if ((h % XFLASH_SECTOR_SIZE) == 0u)
        {
            s_ring[i].last_sector = 0xFFFFFFFFu;
        }
        else
        {
            s_ring[i].last_sector = h / XFLASH_SECTOR_SIZE;

            /* ★补回"写了但没落指针"的记录（见 xf_ring_recover_head 的说明）★
             * 只在环非空时做：used==0 表示上一轮已排空，head 处的"有效记录"
             * 一定是更早以前留下的垃圾，不能当成新的。 */
            if (s_ring[i].used != 0u)
            {
                add = xf_ring_recover_head((xf_ring_id_t)i, h, size);
                if (add != 0u)
                {
                    h = (h + (add * XF_SLOT_SIZE)) % size;
                    s_ring[i].head  = h;
                    s_ring[i].used += add * XF_SLOT_SIZE;

                    /* 扫完正好落在扇区边界 -> 下一个扇区待擦，规则同上面 */
                    s_ring[i].last_sector = ((h % XFLASH_SECTOR_SIZE) == 0u)
                                                ? 0xFFFFFFFFu
                                                : (h / XFLASH_SECTOR_SIZE);

                    log_info("[XFLASH] 环 %u 开局补回 %u 条未落指针的记录\r\n",
                             (unsigned)i, (unsigned)add);
                }
            }
        }
    }

    s_ring_ready = true;
    s_ring_dirty = false;

    log_info("[XFLASH] 数据环装载: EVENT %u/%u 条 (drop=%u) | TELEM %u/%u 条 (drop=%u)\r\n",
             (unsigned)(s_ring[XF_RING_EVENT].used / XF_SLOT_SIZE),
             (unsigned)(s_regions[xf_ring_region(XF_RING_EVENT)].size / XF_SLOT_SIZE),
             (unsigned)s_ring[XF_RING_EVENT].dropped,
             (unsigned)(s_ring[XF_RING_TELEM].used / XF_SLOT_SIZE),
             (unsigned)(s_regions[xf_ring_region(XF_RING_TELEM)].size / XF_SLOT_SIZE),
             (unsigned)s_ring[XF_RING_TELEM].dropped);
}

/* 把 RAM 里的 head/tail 落进 META */
bool xf_ring_flush(void)
{
    xf_meta_t m;
    uint8_t   i;

    if ((!s_ring_ready) || (!xflash_present())) { return false; }

    if (!xf_meta_read(&m))
    {
        (void)memset(&m, 0, sizeof(m));
    }

    for (i = 0u; i < (uint8_t)XF_RING_COUNT; i++)
    {
        m.ring_head[i]    = s_ring[i].head;
        m.ring_tail[i]    = s_ring[i].tail;
        m.ring_dropped[i] = s_ring[i].dropped;
    }

    if (!xf_meta_write(&m))
    {
        log_info("[XFLASH] !! 环指针落 META 失败\r\n");
        return false;
    }

    s_ring_dirty = false;
    return true;
}

void xf_ring_poll(void)
{
    if ((!s_ring_ready) || (!s_ring_dirty)) { return; }

    if ((uint32_t)(get_tick_ms() - s_ring_dirty_ms) < XF_RING_FLUSH_IDLE_MS) { return; }

    (void)xf_ring_flush();
}

/* 读 tail 处那条记录的头部；magic/长度不对返回 false */
static bool xf_ring_read_hdr(xf_ring_id_t id, uint32_t off, xf_rec_hdr_t *h)
{
    uint32_t base = s_regions[xf_ring_region(id)].base;

    if ((off % XF_SLOT_SIZE) != 0u) { return false; }
    if ((off + XF_SLOT_SIZE) > s_regions[xf_ring_region(id)].size) { return false; }

    if (!xflash_read(base + off, h, (uint32_t)sizeof(*h))) { return false; }
    if (h->magic != XF_REC_MAGIC) { return false; }
    if ((h->len == 0u) || (h->len > XF_REC_MAX_PAYLOAD)) { return false; }
    return true;
}

/* 内部: 弹出最旧一条（不返回数据），失败返回 false
 * 固定槽: 一次前进 XF_SLOT_SIZE, 不存在"空隙/变长"问题。 */
static bool xf_ring_drop_tail(xf_ring_id_t id)
{
    xf_ring_t *r = &s_ring[id];

    if (r->used == 0u) { return false; }

    r->tail += XF_SLOT_SIZE;
    if (r->tail >= s_regions[xf_ring_region(id)].size) { r->tail = 0u; }
    r->used = (r->used >= XF_SLOT_SIZE) ? (r->used - XF_SLOT_SIZE) : 0u;

    if (r->used == 0u)
    {
        r->head = 0u;
        r->tail = 0u;
        r->last_sector = 0xFFFFFFFFu;
    }

    s_ring_dirty = true;
    s_ring_dirty_ms = get_tick_ms();
    return true;
}

bool xf_ring_push(xf_ring_id_t id, const void *rec, uint16_t len)
{
    xf_ring_t   *r;
    uint32_t     base;
    uint32_t     size;
    uint32_t     sec;
    xf_rec_hdr_t h;

    if ((!s_ring_ready) || (!xflash_present())) { return false; }
    if (((uint32_t)id >= XF_RING_COUNT) || (rec == NULL)) { return false; }
    if ((len == 0u) || (len > XF_REC_MAX_PAYLOAD))
    {
        log_info("[XFLASH] !! 环记录 %u B 超过单槽上限 %u B\r\n",
                 (unsigned)len, (unsigned)XF_REC_MAX_PAYLOAD);
        return false;
    }

    r    = &s_ring[id];
    base = s_regions[xf_ring_region(id)].base;
    size = s_regions[xf_ring_region(id)].size;

    /* 满了: 淘汰最旧（与 RAM 环"满了覆盖最旧"一致） */
    if (r->used != 0u)
    {
        for (;;)
        {
            uint32_t free_arc = (r->tail + size - r->head) % size;

            if (r->tail == r->head) { free_arc = 0u; }
            /* ★必须让出【整个 4 KB 扇区】再写★ 否则 head 进入新扇区时会擦掉
             * 仍在 tail 一侧的有效槽(环正好写满时的经典事故: 擦掉活数据)。
             * 代价只是少存 64 条(4 KB), 换"擦除永不碰活数据"。 */
            if (free_arc >= (XF_SLOT_SIZE + XFLASH_SECTOR_SIZE)) { break; }

            if (!xf_ring_drop_tail(id)) { return false; }
            r->dropped++;
        }
    }

    /* 换行 */
    if ((r->head + XF_SLOT_SIZE) > size)
    {
        r->head = 0u;
        r->last_sector = 0xFFFFFFFFu;
    }

    /* 进入新扇区: 先擦（NOR 只能 1->0）。槽 64 B 整除 4 KB, 不会跨扇区。 */
    sec = r->head / XFLASH_SECTOR_SIZE;
    if (sec != r->last_sector)
    {
        if (!xflash_erase_4k(base + (sec * XFLASH_SECTOR_SIZE)))
        {
            log_info("[XFLASH] !! 环擦除失败(id=%u off=0x%06X)\r\n",
                     (unsigned)id, (unsigned)(base + (sec * XFLASH_SECTOR_SIZE)));
            return false;
        }
        r->last_sector = sec;
    }

    h.magic = XF_REC_MAGIC;
    h.len   = len;
    h.seq   = r->seq++;
    h.crc   = crc32_ieee((const uint8_t *)rec, (uint32_t)len);

    if (!xflash_write(base + r->head, &h, (uint32_t)sizeof(h))) { return false; }
    if (!xflash_write(base + r->head + XF_REC_HDR_SIZE, rec, (uint32_t)len)) { return false; }

    r->head += XF_SLOT_SIZE;
    if (r->head >= size) { r->head = 0u; r->last_sector = 0xFFFFFFFFu; }
    r->used += XF_SLOT_SIZE;

    s_ring_dirty = true;
    s_ring_dirty_ms = get_tick_ms();
    return true;
}

/** 内部: 读 off 处的记录（头 + 负载 + CRC 校验）。不动 tail */
static bool xf_ring_read_rec(xf_ring_id_t id, uint32_t off, void *buf,
                             uint16_t cap, uint16_t *out_len)
{
    uint32_t     base = s_regions[xf_ring_region(id)].base;
    xf_rec_hdr_t h;
    uint32_t     crc;

    if (!xf_ring_read_hdr(id, off, &h)) { return false; }
    if (h.len > cap) { return false; }

    if (!xflash_read(base + off + XF_REC_HDR_SIZE, buf, (uint32_t)h.len)) { return false; }

    crc = crc32_ieee((const uint8_t *)buf, (uint32_t)h.len);
    if (crc != h.crc) { return false; }

    if (out_len != NULL) { *out_len = h.len; }
    return true;
}

bool xf_ring_peek(xf_ring_id_t id, void *buf, uint16_t cap, uint16_t *out_len)
{
    xf_ring_t   *r;
    xf_rec_hdr_t h;

    if ((!s_ring_ready) || (!xflash_present()) || (buf == NULL)) { return false; }
    if ((uint32_t)id >= XF_RING_COUNT) { return false; }

    r = &s_ring[id];
    if (r->used == 0u) { return false; }

    if (xf_ring_read_rec(id, r->tail, buf, cap, out_len)) { return true; }

    /* 读不出来 = 这条是掉电截断的半条（magic/len/CRC 任一不符），跳过它继续 */
    if (xf_ring_read_hdr(id, r->tail, &h))
    {
        log_info("[XFLASH] !! 环记录损坏(id=%u off=0x%06X) -> 丢弃\r\n",
                 (unsigned)id, (unsigned)r->tail);
    }
    (void)xf_ring_drop_tail(id);
    return false;
}

bool xf_ring_peek_at(xf_ring_id_t id, uint32_t idx, void *buf,
                     uint16_t cap, uint16_t *out_len)
{
    xf_ring_t *r;
    uint32_t   off;
    uint32_t   size;

    if ((!s_ring_ready) || (!xflash_present()) || (buf == NULL)) { return false; }
    if ((uint32_t)id >= XF_RING_COUNT) { return false; }

    r = &s_ring[id];
    if (idx >= (r->used / XF_SLOT_SIZE)) { return false; }

    size = s_regions[xf_ring_region(id)].size;
    off  = (r->tail + (idx * XF_SLOT_SIZE)) % size;

    return xf_ring_read_rec(id, off, buf, cap, out_len);
}

bool xf_ring_pop(xf_ring_id_t id, void *buf, uint16_t cap, uint16_t *out_len)
{
    if (!xf_ring_peek(id, buf, cap, out_len)) { return false; }
    return xf_ring_drop_tail(id);
}

bool xf_ring_drop_n(xf_ring_id_t id, uint32_t n)
{
    xf_ring_t *r;
    uint32_t   size;
    uint32_t   cnt;

    if ((!s_ring_ready) || ((uint32_t)id >= XF_RING_COUNT)) { return false; }

    r    = &s_ring[id];
    cnt  = r->used / XF_SLOT_SIZE;
    if (n > cnt) { n = cnt; }
    if (n == 0u) { return false; }

    size = s_regions[xf_ring_region(id)].size;

    /* 定长槽：一次算完，不用循环 —— 补报一页 N 条后集中出队就靠它 */
    r->tail = (r->tail + (n * XF_SLOT_SIZE)) % size;
    r->used -= n * XF_SLOT_SIZE;

    if (r->used == 0u)
    {
        r->head        = 0u;
        r->tail        = 0u;
        r->last_sector = 0xFFFFFFFFu;
    }

    s_ring_dirty    = true;
    s_ring_dirty_ms = get_tick_ms();
    return true;
}

uint32_t xf_ring_count(xf_ring_id_t id)
{
    if ((uint32_t)id >= XF_RING_COUNT) { return 0u; }
    return s_ring[id].used / XF_SLOT_SIZE;
}

uint32_t xf_ring_used(xf_ring_id_t id)
{
    return ((uint32_t)id < XF_RING_COUNT) ? s_ring[id].used : 0u;
}

uint32_t xf_ring_dropped(xf_ring_id_t id)
{
    return ((uint32_t)id < XF_RING_COUNT) ? s_ring[id].dropped : 0u;
}

bool xf_ring_clear(xf_ring_id_t id)
{
    if ((!s_ring_ready) || ((uint32_t)id >= XF_RING_COUNT)) { return false; }

    s_ring[id].head        = 0u;
    s_ring[id].tail        = 0u;
    s_ring[id].used        = 0u;
    s_ring[id].dropped     = 0u;
    s_ring[id].seq         = 1u;
    s_ring[id].last_sector = 0xFFFFFFFFu;

    s_ring_dirty    = true;
    s_ring_dirty_ms = get_tick_ms();
    return xf_ring_flush();
}

bool xf_ring_format_all(void)
{
    uint8_t i;

    if ((!s_ring_ready) || (!xflash_present())) { return false; }

    for (i = 0u; i < (uint8_t)XF_RING_COUNT; i++)
    {
        s_ring[i].head         = 0u;
        s_ring[i].tail         = 0u;
        s_ring[i].used         = 0u;
        s_ring[i].dropped      = 0u;
        s_ring[i].seq          = 1u;
        s_ring[i].last_sector  = 0xFFFFFFFFu;
    }

    s_ring_dirty = true;
    s_ring_dirty_ms = get_tick_ms();
    return xf_ring_flush();
}
