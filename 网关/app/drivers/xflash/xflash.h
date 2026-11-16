/**
 * @file  xflash.h
 * @brief 外挂 SPI NOR Flash 底层驱动（W25Q64 系，8 MB，SPI1 / PA4-PA7）
 *
 * ============================================================================
 * 【开关与默认值】
 *   `EXT_FLASH_ENABLE` 现在默认 **1**：新板（W25Q64 8 MB，电源检测在 PC3）已实测通过。
 *   ★2026-11-11 起它不只是"增强项"★ —— 待补报的遥测/事件直接写在这颗 Flash 里
 *   （方案 A：Flash 就是队列），所以新板上它属于功能路径，不再是可有可无。
 *   没焊芯片的板子上 `xflash_init()` 探测失败即整体降级为 no-op，此时离线期间的
 *   补报记录会被计入 dropped（用户已确认：开发阶段只有新板，不做 RAM 兜底）。
 *
 *   历史：v1 默认 0，因为当时手上的板子没焊 Flash、且误以为 PA7 是电池 ADC
 *   （power_mgr.c 里那套 ADC2_IN2 采样）；后经原理图核对确认 BAT_AD 在 PC3，
 *   PA7 就是 SPI1_MOSI。那个 power_mgr.c 已于 2026-11-11 整体删除 —— 它从未
 *   进入 CDC.uvprojx、无任何调用点，且网关电池电压实际由 4G 模组 AT 查询得到。
 *
 * 【新板接线（已定）】
 *   CS   = PA4   软件控制，普通推挽输出，上电默认高（不选中）
 *   SCK  = PA5   AF1
 *   MISO = PA6   AF1（主模式下配成输入）
 *   MOSI = PA7   AF1
 *   SPI1 挂 APB2（PCLK2 = 120 MHz），8 分频 = **15 MHz** 起步。
 *   与 LoRa 的 SPI3（PC10/11/12 + PA15）物理独立，**没有总线争用**。
 *
 * 【硬件不存在的处理】
 *   即使 EXT_FLASH_ENABLE=1，`xflash_init()` 也会读 JEDEC ID：
 *   读到全 0x00 / 全 0xFF 就判定"没焊芯片"，把 g_present 置 false，
 *   之后所有接口立刻返回 false，**不阻塞、不重试、不影响主流程**。
 *   —— 外挂 Flash 是增强项，任何情况下都不能让它把网关拖死。
 *
 * 【超时】
 *   每一条等待（TE/RNE/芯片 BUSY）都有循环上限 + 毫秒级上限，绝不裸 while 死等。
 *   扇区擦 4 KB 典型 45 ms、最大 400 ms；64 KB 块擦最大 2 s ⇒ 擦除超时给 3 s。
 * ============================================================================
 */
#ifndef __XFLASH_H__
#define __XFLASH_H__

#include <stdint.h>
#include <stdbool.h>

/* ==================== 编译开关 ==================== */
/** 1 = 外挂 Flash 参与编译与初始化；0 = 完全 no-op
 *  ★2026-11-10 新板实测★ W25Q64 (JEDEC EF 40 17, 8 MB) 已焊, SPI1 链路正常,
 *    因此默认改为 1。老板(无芯片)升级上来也安全: xflash_init() 探测失败即
 *    整体降级为 no-op, 只多几百微秒, 不碰任何业务区。 */
#ifndef EXT_FLASH_ENABLE
#define EXT_FLASH_ENABLE        1
#endif

/** SPI1 时钟分频：0=2分频 … 7=256分频。PCLK2=120MHz 时 3 -> 15MHz，2 -> 30MHz */
#ifndef XFLASH_SPI_BR_PRES
#define XFLASH_SPI_BR_PRES      3
#endif

/* ==================== 几何参数 ==================== */
#define XFLASH_PAGE_SIZE        256u        /**< 页编程粒度（不能跨页写） */
#define XFLASH_SECTOR_SIZE      4096u       /**< 最小擦除单位 */
#define XFLASH_BLOCK_SIZE       65536u      /**< 64 KB 块擦（快，OTA 用） */

/** 8 MB 标准容量；换成 4 MB / 16 MB 芯片时由 JEDEC ID 自动纠正 */
#define XFLASH_CAPACITY_8MB     0x800000u

/* ==================== 接口 ==================== */

/**
 * @brief 初始化 SPI1 + 引脚 + 探测芯片（幂等，可重复调用）
 * @return true = 芯片存在且可用；false = 未启用 / 没焊 / ID 不认识
 * @note  必须在 `bsp_tim1_init_1ms()` 之后调用（内部用 get_tick_ms 做超时）。
 */
bool     xflash_init(void);

/** 芯片是否可用（初始化失败后恒为 false，不会再重试） */
bool     xflash_present(void);

/** 容量（字节）；不可用时返回 0 */
uint32_t xflash_capacity(void);

/** 芯片型号字符串（"W25Q64"/"GD25Q64"/"unknown"），仅用于日志 */
const char *xflash_name(void);

/** 读 JEDEC ID（3 字节：厂商/类型/容量） */
bool     xflash_id(uint8_t out[3]);

/**
 * @brief 读数据（任意长度，可跨页/跨扇区）
 * @return true = 成功
 */
bool     xflash_read(uint32_t off, void *buf, uint32_t len);

/**
 * @brief 写数据（自动按 256 B 页拆分、自动发 WREN）
 * @note  ★调用方必须先擦除目标区域★ —— NOR 只能把 1 写成 0，不擦就写会得到
 *        "旧数据 AND 新数据"的垃圾。本函数【不做】隐式擦除（会误擦整扇区）。
 * @return true = 全部写入且每一步都等到芯片空闲
 */
bool     xflash_write(uint32_t off, const void *buf, uint32_t len);

/** 擦 4 KB 扇区（off 必须 4 KB 对齐） */
bool     xflash_erase_4k(uint32_t off);

/** 擦 64 KB 块（off 必须 64 KB 对齐；比 16 次 4 KB 快得多） */
bool     xflash_erase_64k(uint32_t off);

/** 读状态寄存器 1（bit0 = BUSY，bit1 = WEL） */
bool     xflash_read_status(uint8_t *out);

/** 等芯片空闲；超时返回 false */
bool     xflash_wait_ready(uint32_t timeout_ms);

/** 累计错误次数（诊断用：>0 说明总线/芯片有问题） */
uint32_t xflash_error_count(void);

#endif /* __XFLASH_H__ */
