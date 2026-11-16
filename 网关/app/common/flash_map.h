/**
 * @file flash_map.h
 * @brief N32H482 片内 Flash 分区定义的【唯一真相源】(single source of truth)
 *
 * 之前分区地址散落在 5 个文件里(bootloader.h / bootloader.c / ota_port.h /
 * param_save.c 的注释与宏), 改一处漏一处。本文件把它们收拢到一处。
 *
 * ---------------------------------------------------------------------------
 * 芯片: N32H482  512KB Flash @ 0x08000000~0x0807FFFF, 页大小 8KB (0x2000 对齐)
 *       逻辑上共 64 页。
 * ---------------------------------------------------------------------------
 *
 * 三种布局, 由 FM_MAP_MODE 选择:
 *
 * 【模式 0】FM_MAP_SINGLE_LEGACY —— 原始布局
 *   板上没有 bootloader, app 直接烧在 0x08000000。本模式保留 v12 的全部地址。
 *   ⚠ 隐患: App 的链接区间是 0x08000000 + 0x80000(整个 Flash), 靠"代码只有 77KB"
 *     侥幸没撞到 0x08040000 的 OTA 缓存区/0x0807E000 的 param 页。
 *     而且本模式【没有】配置双备份区与事件日志区。
 *
 *     区域                 起始          大小       页数
 *     App 运行区           0x08000000    256 KB     32
 *     OTA 固件缓存区       0x08040000    240 KB     30
 *     OTA 标记页           0x0807C000      8 KB      1
 *     param_save 参数页    0x0807E000      8 KB      1
 *     ------------------------------------------------------
 *     合计                               512 KB     64
 *
 * ---------------------------------------------------------------------------
 *
 * 【模式 1】FM_MAP_SINGLE_RESERVED —— 历史模式, 当前不用(见模式 2)
 *   同样【不需要 bootloader】: App 依然从 0x08000000 启动, 与模式 0 完全一样。
 *   区别只是把 App 的链接区间从 512KB 收紧到 192KB, 于是后面的 64KB 就被
 *   真正"钉死"成配置区与事件日志区 —— 链接器再也不会把代码放进去。
 *
 *   这一步是安全的: 固件当前 ~77KB, 离 192KB 上限还有很大余量;
 *   而且顺带修掉了"代码长过界覆盖 param_save / OTA 缓存区"的隐患。
 *
 *     区域                 起始          大小       页数
 *     App 运行区           0x08000000    192 KB     24
 *     配置双备份 A         0x08030000      8 KB      1
 *     配置双备份 B         0x08032000      8 KB      1
 *     事件日志 (≥500 条, 未实现) 0x08034000     48 KB      6
 *     OTA 固件缓存区       0x08040000    240 KB     30
 *     OTA 标记页           0x0807C000      8 KB      1
 *     param_save 参数页    0x0807E000      8 KB      1
 *     ------------------------------------------------------
 *     合计                               512 KB     64
 *
 * ---------------------------------------------------------------------------
 *
 * 【模式 2】FM_MAP_DUAL_SLOT —— ★当前采用★ (双 Slot A/B, 2026-11-12 重分区)
 *   App 从 0x08004000 启动, 必须先烧 bootloader; OTA 暂存区(FM_STAGE)就是 Slot B。
 *
 *     区域                 起始          大小       页数
 *     BootLoader           0x08000000     16 KB      2
 *     App-A (运行区)       0x08004000    224 KB     28
 *     App-B (OTA 目标槽)   0x0803C000    224 KB     28
 *     META (标记+启动记录) 0x08074000      8 KB      1
 *     PARAM (device_param) 0x08076000      8 KB      1
 *     CFG-A                0x08078000      8 KB      1
 *     CFG-B                0x0807A000      8 KB      1
 *     预留 (RSV, 未分配)   0x0807C000     16 KB      2
 *     ------------------------------------------------------
 *     合计                               512 KB     64
 *
 * ---------------------------------------------------------------------------
 * 为什么是 224KB (16 + 224 + 224 + 8 + 8 + 8 + 8 + 16 = 512KB, 正好排满)?
 *   旧 208KB 布局里的事件日志区(48KB @0x08074000)从未实现、全工程零引用,
 *   且已被外挂 Flash 的 EVENT 环(2MB, 正在跑)取代 -> 整体删除。
 *   腾出的 48KB 分给两个 App 区各 16KB(208->224), 末 16KB 留作未分配预留,
 *   供将来"崩溃记录"或"更大配置快照"使用。
 *
 *   ⚠ 改 Slot 大小或任何区地址, 必须【重编并重烧 BL】: BL_APP_MAX_SIZE 与
 *     BL_META_ADDR 都是编译进 BL 的常量, 现场设备只能 OTA 到 App, 永远改不到 BL。
 *     开发阶段随便改; 量产后再改就是返厂。
 */
#ifndef __FLASH_MAP_H__
#define __FLASH_MAP_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * ★★★ 分区模式开关 ★★★
 *   0 = 原始布局(无配置区/事件区)
 *   1 = 单镜像 + 预留配置/事件区(默认; 无需 bootloader, App 仍在 0x08000000)
 *   2 = 双 Slot A/B(需 bootloader + App 链接到 0x08004000)
 * ====================================================================== */
#define FM_MAP_SINGLE_LEGACY     0
#define FM_MAP_SINGLE_RESERVED   1
#define FM_MAP_DUAL_SLOT         2

#ifndef FM_MAP_MODE
#define FM_MAP_MODE              FM_MAP_SINGLE_RESERVED
#endif

/* ==================== 芯片级常量 ==================== */
#define FM_FLASH_BASE           0x08000000U
#define FM_FLASH_SIZE           0x00080000U   /* 512 KB */
#define FM_PAGE_SIZE            0x00002000U   /* 8 KB */
#define FM_FLASH_END            (FM_FLASH_BASE + FM_FLASH_SIZE)
#define FM_PAGE_COUNT           (FM_FLASH_SIZE / FM_PAGE_SIZE)   /* 64 */

/* ==================== C99 静态断言 ==================== */
#define FM_STATIC_ASSERT(cond, tag)  typedef char fm_sa_##tag[(cond) ? 1 : -1]

/* ==================== 各模式的分区 ==================== */

#if (FM_MAP_MODE == FM_MAP_SINGLE_LEGACY)

/* --- 无独立 bootloader 区; App 从 0x08000000 开始 --- */
#define FM_BOOT_ADDR            0x08000000U
#define FM_BOOT_SIZE            0x00000000U

#define FM_SLOT_A_ADDR          0x08000000U
#define FM_SLOT_B_ADDR          0x00000000U   /* 本模式无 Slot B */
#define FM_SLOT_SIZE            0x00040000U

#define FM_STAGE_ADDR           0x08040000U
#define FM_STAGE_SIZE           0x0003C000U

#define FM_META_ADDR            0x0807C000U
#define FM_META_SIZE            0x00002000U

#define FM_PARAM_ADDR           0x0807E000U
#define FM_PARAM_SIZE           0x00002000U

#define FM_CFG_A_ADDR           0x00000000U
#define FM_CFG_B_ADDR           0x00000000U
#define FM_CFG_SIZE             0x00000000U
#define FM_CFG_TOTAL_SIZE       0x00000000U
#define FM_EVENT_ADDR           0x00000000U
#define FM_EVENT_SIZE           0x00000000U

#define FM_HAS_BOOTLOADER       0
#define FM_HAS_SLOT_B           0
#define FM_HAS_CFG_BANK         0
#define FM_HAS_EVENT_LOG        0

#elif (FM_MAP_MODE == FM_MAP_SINGLE_RESERVED)

/* --- 无独立 bootloader 区; App 仍从 0x08000000 开始, 但只占 192KB --- */
#define FM_BOOT_ADDR            0x08000000U
#define FM_BOOT_SIZE            0x00000000U

#define FM_SLOT_A_ADDR          0x08000000U
#define FM_SLOT_B_ADDR          0x00000000U   /* 本模式无 Slot B */
#define FM_SLOT_SIZE            0x00030000U   /* 192 KB = 24 页; 超了链接器会报错 */

/* --- 配置双备份(2 页, 紧跟在 App 之后) --- */
#define FM_CFG_A_ADDR           0x08030000U
#define FM_CFG_B_ADDR           0x08032000U
#define FM_CFG_SIZE             0x00002000U   /* 单区大小 = 1 页 */
#define FM_CFG_TOTAL_SIZE       0x00004000U   /* A+B 合计 */

/* --- 事件日志(48KB = 6 页) --- */
#define FM_EVENT_ADDR           0x08034000U
#define FM_EVENT_SIZE           0x0000C000U

#define FM_STAGE_ADDR           0x08040000U
#define FM_STAGE_SIZE           0x0003C000U

#define FM_META_ADDR            0x0807C000U
#define FM_META_SIZE            0x00002000U

#define FM_PARAM_ADDR           0x0807E000U
#define FM_PARAM_SIZE           0x00002000U

#define FM_HAS_BOOTLOADER       0
#define FM_HAS_SLOT_B           0
#define FM_HAS_CFG_BANK         1
#define FM_HAS_EVENT_LOG        1

#elif (FM_MAP_MODE == FM_MAP_DUAL_SLOT)

#define FM_BOOT_ADDR            0x08000000U
#define FM_BOOT_SIZE            0x00004000U   /* 16 KB = 2 页 */

#define FM_SLOT_A_ADDR          0x08004000U
#define FM_SLOT_B_ADDR          0x0803C000U   /* ★2026-11-12: 0x08038000 -> 0x0803C000 */
#define FM_SLOT_SIZE            0x00038000U   /* ★224 KB = 28 页 (原 208 KB) */

#define FM_META_ADDR            0x08074000U   /* ★原 0x0806C000 */
#define FM_META_SIZE            0x00002000U

#define FM_PARAM_ADDR           0x08076000U   /* ★原 0x0806E000 */
#define FM_PARAM_SIZE           0x00002000U

#define FM_CFG_A_ADDR           0x08078000U   /* ★原 0x08070000 */
#define FM_CFG_B_ADDR           0x0807A000U   /* ★原 0x08072000 */
#define FM_CFG_SIZE             0x00002000U   /* 单区 8 KB */
#define FM_CFG_TOTAL_SIZE       0x00004000U   /* A+B 合计 16 KB */

/* ★2026-11-12: 事件日志区(原 48 KB @0x08074000)整体删除, FM_EVENT_ADDR /
 *   FM_EVENT_SIZE 一并移除。理由: 从未实现过(全工程零引用), 且已被外挂 Flash 的
 *   EVENT 环(2 MB)取代。腾出的 48 KB = 两个 App 区各 16 KB + 末 16 KB 预留。
 *   注意: 外置 Flash 的 EVENT 环不受影响, 本处只删片内这个空分区。 */
#define FM_RSV_ADDR             0x0807C000U
#define FM_RSV_SIZE             0x00004000U   /* 16 KB = 2 页, 未分配预留 */

/* 双 Slot 模式下, OTA 的"暂存区"就是 Slot B(App-B) —— 自动跟随, 不用单独改 */
#define FM_STAGE_ADDR           FM_SLOT_B_ADDR
#define FM_STAGE_SIZE           FM_SLOT_SIZE

#define FM_HAS_BOOTLOADER       1
#define FM_HAS_SLOT_B           1
#define FM_HAS_CFG_BANK         1
#define FM_HAS_EVENT_LOG        0     /* ★改为 0 */

#else
#error "FM_MAP_MODE 取值非法(只能是 0/1/2)"
#endif

/* ======================================================================
 * 与模式无关的通用名字(业务代码只用下面这些)
 * ====================================================================== */

/* App 当前运行区(也是 bootloader 跳转目标) */
#define FM_APP_ADDR             FM_SLOT_A_ADDR
#define FM_APP_SIZE             FM_SLOT_SIZE

/* Slot 序号 <-> 地址 */
#define FM_SLOT_A               0U
#define FM_SLOT_B               1U
#define FM_SLOT_ADDR(slot)      (((slot) == FM_SLOT_B) ? FM_SLOT_B_ADDR : FM_SLOT_A_ADDR)

/* 配置区是否可用(编译期判据, 供 config_store 用) */
#define FM_CFG_AVAILABLE        (FM_HAS_CFG_BANK)

/* ======================================================================
 * 静态断言: 保证分区不重叠、页对齐、刚好排满
 * ====================================================================== */
FM_STATIC_ASSERT((FM_BOOT_ADDR == FM_FLASH_BASE),                          boot_base);
FM_STATIC_ASSERT((FM_PAGE_SIZE == 0x2000U),                               page_size);
FM_STATIC_ASSERT((FM_PAGE_COUNT == 64U),                                  page_count);

FM_STATIC_ASSERT(((FM_BOOT_SIZE  & (FM_PAGE_SIZE - 1U)) == 0U),           boot_align);
FM_STATIC_ASSERT(((FM_SLOT_SIZE  & (FM_PAGE_SIZE - 1U)) == 0U),           slot_align);
FM_STATIC_ASSERT(((FM_META_SIZE  & (FM_PAGE_SIZE - 1U)) == 0U),           meta_align);
FM_STATIC_ASSERT(((FM_PARAM_SIZE & (FM_PAGE_SIZE - 1U)) == 0U),           param_align);
FM_STATIC_ASSERT(((FM_CFG_SIZE   & (FM_PAGE_SIZE - 1U)) == 0U),           cfg_align);
/* 模式 2 已删除 EVENT 区并改用 RSV, 其对齐断言在下面的 DUAL_SLOT 分支里 */
#if (FM_MAP_MODE != FM_MAP_DUAL_SLOT)
FM_STATIC_ASSERT(((FM_EVENT_SIZE & (FM_PAGE_SIZE - 1U)) == 0U),           event_align);
#endif
FM_STATIC_ASSERT(((FM_STAGE_SIZE & (FM_PAGE_SIZE - 1U)) == 0U),           stage_align);

/* App 与暂存区不得重叠 */
FM_STATIC_ASSERT((FM_APP_ADDR + FM_APP_SIZE) <= FM_STAGE_ADDR,            app_no_overlap_stage);

#if (FM_MAP_MODE == FM_MAP_DUAL_SLOT)
FM_STATIC_ASSERT((FM_SLOT_A_ADDR == (FM_BOOT_ADDR  + FM_BOOT_SIZE)),      slota_follows_boot);
FM_STATIC_ASSERT((FM_SLOT_B_ADDR == (FM_SLOT_A_ADDR + FM_SLOT_SIZE)),     slotb_follows_slota);
FM_STATIC_ASSERT((FM_META_ADDR  == (FM_SLOT_B_ADDR + FM_SLOT_SIZE)),      meta_follows_slotb);
FM_STATIC_ASSERT((FM_PARAM_ADDR == (FM_META_ADDR  + FM_META_SIZE)),       param_follows_meta);
FM_STATIC_ASSERT((FM_CFG_A_ADDR == (FM_PARAM_ADDR + FM_PARAM_SIZE)),      cfg_follows_param);
FM_STATIC_ASSERT((FM_CFG_B_ADDR == (FM_CFG_A_ADDR + FM_CFG_SIZE)),        cfgb_follows_cfga);
FM_STATIC_ASSERT((FM_RSV_ADDR  == (FM_CFG_B_ADDR + FM_CFG_SIZE)),         rsv_follows_cfgb);
FM_STATIC_ASSERT(((FM_RSV_SIZE & (FM_PAGE_SIZE - 1U)) == 0U),             rsv_align);
FM_STATIC_ASSERT((FM_RSV_SIZE  == (FM_FLASH_END - FM_RSV_ADDR)),          rsv_fills_flash);

#elif (FM_MAP_MODE == FM_MAP_SINGLE_RESERVED)
/* App -> 配置A -> 配置B -> 事件 -> 暂存 -> 标记 -> 参数, 首尾相接 */
FM_STATIC_ASSERT((FM_CFG_A_ADDR  == (FM_APP_ADDR   + FM_APP_SIZE)),       cfga_follows_app);
FM_STATIC_ASSERT((FM_CFG_B_ADDR  == (FM_CFG_A_ADDR + FM_CFG_SIZE)),       cfgb_follows_cfga);
FM_STATIC_ASSERT((FM_EVENT_ADDR  == (FM_CFG_A_ADDR + FM_CFG_TOTAL_SIZE)), event_follows_cfg);
FM_STATIC_ASSERT((FM_STAGE_ADDR  == (FM_EVENT_ADDR + FM_EVENT_SIZE)),     stage_follows_event);
FM_STATIC_ASSERT((FM_META_ADDR   == (FM_STAGE_ADDR + FM_STAGE_SIZE)),     meta_follows_stage);
FM_STATIC_ASSERT((FM_PARAM_ADDR  == (FM_META_ADDR  + FM_META_SIZE)),      param_follows_meta);
FM_STATIC_ASSERT((FM_FLASH_END   == (FM_PARAM_ADDR + FM_PARAM_SIZE)),     map_fills_flash);

#else  /* LEGACY */
FM_STATIC_ASSERT((FM_STAGE_ADDR == (FM_APP_ADDR   + FM_APP_SIZE)),        stage_follows_app);
FM_STATIC_ASSERT((FM_META_ADDR  == (FM_STAGE_ADDR + FM_STAGE_SIZE)),      meta_follows_stage);
FM_STATIC_ASSERT((FM_PARAM_ADDR == (FM_META_ADDR  + FM_META_SIZE)),       param_follows_meta);
FM_STATIC_ASSERT((FM_FLASH_END  == (FM_PARAM_ADDR + FM_PARAM_SIZE)),      map_fills_flash);
#endif

/* ======================================================================
 * 运行时辅助
 * ====================================================================== */

/**
 * @brief 地址 -> 所属 Slot 序号
 */
static inline uint8_t fm_slot_of_addr(uint32_t addr)
{
    return (addr >= FM_SLOT_B_ADDR) ? (uint8_t)FM_SLOT_B : (uint8_t)FM_SLOT_A;
}

/**
 * @brief 地址是否落在 App 运行区内
 */
static inline uint8_t fm_addr_in_app(uint32_t addr)
{
    return ((addr >= FM_APP_ADDR) && (addr < (FM_APP_ADDR + FM_APP_SIZE))) ? 1U : 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_MAP_H__ */
