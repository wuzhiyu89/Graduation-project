/**
 * @file  xflash_store.h
 * @brief 外挂 Flash 的分区表 + 超级块(META) + 出厂固件区
 *
 * ============================================================================
 * 【8 MB 分区表 —— 与 docs/外挂SPI-Flash空间规划.md 必须逐字节一致】
 *
 *   区            起始        大小      块数(64KB)   放什么
 *   META        0x000000     64 KB        1        超级块(双份) + 坏块位图
 *   CFG         0x010000     64 KB        1        配置快照 4KB x 16
 *   EVENT       0x020000      2 MB       32        事件补报(掉电不丢)
 *   TELEM       0x220000      3 MB       48        液位遥测逐条
 *   LOG         0x520000      1 MB       16        关键日志
 *   OTA         0x620000    512 KB        8        OTA 整包暂存(可续传)
 *   FW_FACTORY  0x6A0000      1 MB       16        ★出厂固件镜像(恢复出厂用)★
 *   FACTORY     0x7A0000     64 KB        1        出厂参数备份
 *   SPARE       0x7B0000    320 KB        5        预留
 *   ---------------------------------------------------------------
 *   合计                                    128       = 8 MB
 *
 * 【为什么 EVENT 从 1 MB 提到 2 MB、OTA 从 1.5 MB 降到 512 KB】
 *   · 事件是"平台看到的过程记录"，断网时比遥测更容易被漏看，且单条 28 B、
 *     2 MB 能存 ~7.4 万条，足够长断网 + 长期掉电；
 *   · OTA 只需要放下一份完整固件：内部 Slot 只有 208 KB，512 KB 已是 2.5 倍余量，
 *     再大纯属浪费（真要涨到 512 KB 以上，先改这里再改分区）。
 *
 * 【为什么单独开 FW_FACTORY(1 MB)】
 *   出厂固件是"最后一道保险"：云端下发"恢复出厂设置"时，即使当前固件跑飞、
 *   内部 A/B 双槽都坏，也能从这颗外挂 Flash 里把出厂镜像读出来重新写回内部槽。
 *   · 写：产线工装/`#flash fwif` 一次性写入(镜像 + 64 B 头，头带 CRC32/MD5)；
 *   · 读：只读，运行期绝不修改 —— 它是"不可变基线"，不能被 OTA 流程碰；
 *   · 1 MB = 内部 App 区(208 KB)的 5 倍，足够放"整包 bin"甚至"bin+压缩包"。
 *
 * 【超级块为什么要双份】
 *   写 META 时掉电，单份就废了。两份交替写 + seq 递增 + CRC32：
 *   总能读出"较新且校验通过"的那一份；两份都坏才走全环扫描。
 * ============================================================================
 */
#ifndef __XFLASH_STORE_H__
#define __XFLASH_STORE_H__

#include <stdint.h>
#include <stdbool.h>
#include "xflash.h"

/* ==================== 分区 ==================== */
typedef enum
{
    XF_REGION_META = 0,
    XF_REGION_CFG,
    XF_REGION_EVENT,
    XF_REGION_TELEM,
    XF_REGION_LOG,
    XF_REGION_OTA,
    XF_REGION_FW_FACTORY,
    XF_REGION_FACTORY,
    XF_REGION_SPARE,
    XF_REGION_COUNT
} xf_region_t;

typedef struct
{
    const char *name;
    uint32_t    base;
    uint32_t    size;
} xf_region_info_t;

/* ==================== 超级块 ==================== */
#define XF_META_MAGIC       0x584D4554u     /* "XMET" */
#define XF_META_VERSION     1u
#define XF_META_SLOT_SIZE   4096u           /* 每个槽占 4 KB（NOR 最小擦除单位） */

/* ★2026-11-11: 从 2 份扩到 16 槽轮转★
 *
 * 【为什么必须扩】方案 A 之后 META 不再是"偶尔写一次"—— 待补报队列的 head/tail
 *   每变一次就要落一次盘（断网期间一直在变）。只有 2 个槽交替写时，每次写都要
 *   擦同一个扇区：实测 4 台 × 60 s 的量就达到 120 次/小时/槽，
 *   连续断网约 830 小时（35 天）就能把 W25Q64 一个扇区的 10 万次寿命磨完；
 *   现场信号差、断网攒两三年是很正常的，所以必须摊开。
 *   16 槽后同样的写入量下，单槽累计只有原来的 1/16。
 *
 * 【为什么是 16】META 区正好 64 KB = 16 × 4 KB，一个字节都不用多划；
 *   再多就得动分区表，收益也已不明显（写入节流同时放宽到 30 s）。
 *
 * 【代价】开机要扫 16 个槽找 seq 最大且 CRC 通过的那一份（64 KB SPI 读，≈ 90 ms），
 *   但只在开机扫一次 —— 之后全走 RAM 缓存（s_meta），不再读盘。
 */
#define XF_META_SLOTS       16u

/** 环的个数：事件 / 遥测 / 日志（与 xf_ring_id_t 一一对应） */
#define XF_RING_COUNT       3u
typedef enum
{
    XF_RING_EVENT = 0,
    XF_RING_TELEM,
    XF_RING_LOG
} xf_ring_id_t;

typedef struct
{
    uint32_t magic;                         /* XF_META_MAGIC */
    uint16_t version;                       /* XF_META_VERSION */
    uint16_t size;                          /* sizeof(xf_meta_t)，防止结构体改坏旧数据 */
    uint32_t seq;                           /* 递增，取大的那份 */
    uint32_t ring_head[XF_RING_COUNT];      /* 写指针（字节偏移，相对环起点） */
    uint32_t ring_tail[XF_RING_COUNT];      /* 最老有效记录偏移 */
    uint32_t ring_dropped[XF_RING_COUNT];   /* 因满覆盖而丢掉的条数 */
    uint32_t fw_factory_valid;              /* 1 = FW_FACTORY 里有可用的出厂镜像 */
    uint32_t reserved[2];
    uint32_t crc;                           /* CRC32，覆盖前面所有字节 */
} xf_meta_t;

/* ==================== 出厂固件头 ==================== */
#define XF_FW_MAGIC         0x31574657u     /* "FWF1" */
#define XF_FW_VER_STR_MAX   24u
/** 头单独占一个 4 KB 扇区，镜像正文从 base + 4096 开始。
 *  这样"只改头/只擦头"不会带走镜像的前 4 KB —— NOR 的擦除粒度是扇区，
 *  头与正文放同一扇区的话，更新头就必须重写整段。 */
#define XF_FW_HDR_SLOT      4096u

/**
 * 出厂固件头（64 B，占 FW_FACTORY 起始处一个 4 KB 扇区）。
 * 镜像正文从 `base + XF_FW_HDR_SLOT` 开始。产线工装写入；运行期只读。
 */
typedef struct
{
    uint32_t magic;                         /* XF_FW_MAGIC */
    uint32_t size;                          /* 镜像字节数（<= 区大小 - 4 KB 头扇区） */
    uint32_t crc32;                         /* 镜像正文的 CRC32 */
    uint32_t version;                       /* 数值版本号（便于比较新旧） */
    char     ver_str[XF_FW_VER_STR_MAX];    /* "V1.2.3" 之类，仅展示 */
    uint8_t  md5[16];                       /* 镜像 MD5（与 OTA 校验口径一致） */
    uint16_t header_size;                   /* sizeof(xf_fw_header_t) = 64 */
    uint16_t flags;                         /* bit0 = 镜像有效 */
    uint32_t hdr_crc;                       /* 本结构体(前面所有字段)的 CRC32 */
} xf_fw_header_t;

/* ==================== 接口 ==================== */

/**
 * @brief 初始化外挂 Flash + 读/建超级块（幂等）
 * @return true = 可用（芯片在且 META 有效或已成功格式化）
 * @note  芯片不在时返回 false，后续所有接口 no-op；调用方【不要】把它当成致命错误。
 */
bool xf_store_init(void);

/** 是否可用（= 芯片在 && META 有效） */
bool xf_store_ready(void);

/** 取分区信息；越界返回 NULL */
const xf_region_info_t *xf_region_info(xf_region_t r);

/**
 * @brief 读超级块（16 槽里取 seq 最大且 CRC 通过的那一份）
 * @note  ★命中 RAM 缓存时不读盘★ —— 落盘是每 30 s 一次的常规动作，每次都扫
 *        16 个扇区（≈90 ms）会把主循环拖死。要强制重读用 xf_meta_invalidate()。
 */
bool xf_meta_read(xf_meta_t *out);

/**
 * @brief 丢掉 RAM 里的超级块缓存，下次 xf_meta_read() 重新扫盘
 * @note  给"外部改过 META"的场景用：产线工装、调试工具、以及主机单测里模拟
 *        "别处把扇区写坏了"的用例。运行期正常路径不需要调。
 */
void xf_meta_invalidate(void);

/**
 * @brief 写超级块（写到 seq 较旧的那一份，擦 4 KB 后写入并回读校验）
 * @note  调用方传入的 seq 会被忽略：内部按"当前最大 seq + 1"写，避免回退。
 */
bool xf_meta_write(xf_meta_t *m);

/** 格式化 META（擦掉两份超级块并写入初始值）—— 只在首次使用/确认损坏时调 */
bool xf_store_format(void);

/** 打印分区表 + 超级块（诊断用；实现放在本文件，串口命令直接调） */
void xf_store_dump(void);

/* ==================== 数据环形区（EVENT / TELEM / LOG） ====================
 *
 * 【用途 —— ★2026-11-11 起是"队列本体"，不再是 RAM 环的溢出层★】
 *   event_cache / telemetry_cache 现在【直接】把待补报记录写进这两个环，
 *   RAM 里不再留副本（方案 A：Flash 就是队列）。理由与账见
 *   docs/缓存分层规划.md：
 *     · 掉电最多丢 1 条（以前丢 RAM 里那一整批 128/256 条）；
 *     · 补报顺序天然等于采集顺序（以前 Flash 存头段、RAM 存尾段，会倒挂）；
 *     · 没有"两份指针要对账"这回事，少一类最难查的错。
 *
 * 【记录格式】12 字节头 + 负载(4 字节对齐)：
 *     magic(u16) | len(u16) | seq(u32) | crc32(u32) | payload[len]
 *   crc 覆盖 payload；magic 不对 / crc 不对 = 该位置没有有效记录（掉电残留）。
 *
 * 【掉电语义】head/tail 存在 META 超级块里，但**不是每条都写**（每条都写会
 *   把 Flash 擦爆）。策略：`xf_ring_poll()` 在"空闲 30 s 后"落一次 META。
 *   · 记录本身**在写入那一刻就已经进 Flash 了**，META 只是"指针"；
 *   · 掉电后开机，`xf_ring_load()` 会从 META 的 head 位置**向前扫描连续的
 *     有效记录**，把"写了但没来得及落指针"的那几条补回来 ⇒ 通常一条不丢；
 *   · 只有"head 正好停在扇区边界"时不扫（下一扇区可能是上一轮的旧数据，
 *     扫进去会把旧记录当新的）—— 此时最多丢最后一拍（≈30 s，0.4 条）。
 *   · tail 落后于实际值时，最坏会**重复补报**几条；因为补报的 msgId 是由
 *     (uid, ts, msg_seq) 确定性重建的，平台按 msgId 去重即可，不会产生脏数据。
 *   每次换到新扇区会自动擦除该扇区（NOR 只能 1->0，必须先擦）。
 *
 * 【失败语义】任何一步失败返回 false；调用方（缓存层）自行退化成"纯 RAM 环形"。
 */
#define XF_REC_MAGIC        0x4645u      /* 'E','F' 小端; 记录头魔数 */
#define XF_REC_HDR_SIZE     12u
/* ★2026-11-10: 固定 64 字节槽（12 B 头 + ≤52 B 负载）。
 *   为什么固定槽: 变长记录会让"记录跨 4 KB 扇区 / 换行留空隙"这两件事互相纠缠，
 *   主机单测里连续抓到两个数据损坏缺陷（跨扇区尾巴被擦、tail 走过空隙读旧数据）。
 *   本工程两条环的负载只有 28 B(事件) / 32 B(遥测)，64 B 槽正好；
 *   64 整除 4096，槽永不跨扇区，tail 前进就是 +64，没有空隙问题。 */
#define XF_SLOT_SIZE        64u
#define XF_REC_MAX_PAYLOAD  (XF_SLOT_SIZE - XF_REC_HDR_SIZE)

/** 追加一条记录；空间不足时自动淘汰最旧(dropped+1)。返回 false = 写失败/参数非法 */
bool     xf_ring_push(xf_ring_id_t id, const void *rec, uint16_t len);

/** 只读最旧一条：拷贝到 buf，*out_len 返回真实长度。false = 环空/读失败 */
bool     xf_ring_peek(xf_ring_id_t id, void *buf, uint16_t cap, uint16_t *out_len);

/**
 * @brief 只读第 idx 条（0 = 最旧），不出队
 * @return false = 越界 / 该位置没有有效记录
 * @note  槽是【定长 64 B】，所以 idx 换算成偏移是 O(1)（tail + idx×64，按环回绕），
 *        不需要从头扫描 —— 事件补报的"分页读第 i 条"就靠它。
 */
bool     xf_ring_peek_at(xf_ring_id_t id, uint32_t idx, void *buf,
                         uint16_t cap, uint16_t *out_len);

/** 读出最旧一条并把它从环里删掉（补报成功/加载进 RAM 时用） */
bool     xf_ring_pop(xf_ring_id_t id, void *buf, uint16_t cap, uint16_t *out_len);

/**
 * @brief 一次丢掉最旧 n 条（不出数据）
 * @note  补报成功一页(N 条)后集中 drop，避免 N 次走 peek 的读盘开销。
 *        实际丢弃数会被夹到当前条数，n=0 安全。
 */
bool     xf_ring_drop_n(xf_ring_id_t id, uint32_t n);

/** 环里的记录条数（= used / XF_SLOT_SIZE） */
uint32_t xf_ring_count(xf_ring_id_t id);

/** 环里已占用的字节数（诊断用） */
uint32_t xf_ring_used(xf_ring_id_t id);

/** 因环满被淘汰的累计条数（诊断用） */
uint32_t xf_ring_dropped(xf_ring_id_t id);

/**
 * @brief 清空一个环（恢复出厂 / 换绑 / #flash fmt）
 * @note  只把 head/tail 归零并落 META，**不擦数据区**（NOR 归零即等于空）；
 *        下一次写到该扇区时会按 last_sector 规则先擦。
 */
bool     xf_ring_clear(xf_ring_id_t id);

/** 主循环里周期调用：把 head/tail 在空闲 30 s 后落进 META */
void     xf_ring_poll(void);

/** 立刻把 head/tail 落进 META（关机/恢复出厂前调用）；成功返回 true */
bool     xf_ring_flush(void);

/** 清空三个数据环（恢复出厂用）。不动 META 的分区信息外的内容 */
bool     xf_ring_format_all(void);

/* ---------- 出厂固件区（只读为主） ---------- */

/** 读出厂固件头；不存在/校验失败返回 false */
bool xf_fw_factory_header(xf_fw_header_t *out);

/** 出厂镜像是否可用（头有效 && flags.bit0 && size 合理） */
bool xf_fw_factory_present(void);

/** 读镜像正文（off 相对镜像起点，不含头所在的 4 KB 扇区） */
bool xf_fw_factory_read(uint32_t off, void *buf, uint32_t len);

/** 擦除整个出厂固件区（产线写入前调用；运行期不要调） */
bool xf_fw_factory_erase(void);

/** 写镜像正文（off 相对镜像起点；调用前必须已 erase） */
bool xf_fw_factory_write(uint32_t off, const void *buf, uint32_t len);

/** 写头（最后一步，写头成功才算"这份镜像可用"） */
bool xf_fw_factory_set_header(const xf_fw_header_t *h);

#endif /* __XFLASH_STORE_H__ */
