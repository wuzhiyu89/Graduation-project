/**
 * \file  main_bl.c
 * \brief BootLoader 专用入口
 *
 * ⚠ 本文件【只属于 BL 构建配置】(链接到 0x08000000, 长度 16KB),
 *   **绝对不要**把它加进 App 工程(app/ 那套 CDC 构建)的源文件列表,
 *   否则会出现两个 main() 导致链接失败。
 *
 * BL 与 App 的关系:
 *   BL  (0x08000000, 16KB) 上电先跑 → 检测标记页 → 需要搬运就搬 → 跳 App
 *   App (0x08004000, 224KB) 业务固件
 *
 * 编译要点(2026-11-12 按实际构建链核对: tools\build_bl.ps1 + bootloader\BL.sct):
 *   - 编译宏只有 N32H482 / USE_STDPERIPH_DRIVER / FM_MAP_MODE=2。
 *     BL_APP_ADDR / BL_APP_MAX_SIZE / BL_META_ADDR / BL_OTA_BUFFER_ADDR 全在
 *     bootloader.h 里别名到 flash_map.h 的模式 2 分支, 不写死。
 *     → 改分区表(尤其 FM_SLOT_SIZE / FM_META_ADDR)后必须【重编并重烧 BL】。
 *     (旧注释说"必须带 FM_DUAL_SLOT_ENABLE=1, 否则 FM_APP_ADDR 会是 0x08000000":
 *      该宏全工程零引用、build_bl.ps1 也没定义, 且模式 2 下 FM_APP_ADDR 恒为
 *      0x08004000 —— 已核实为过期描述, 删除。)
 *   - 不要定义 VECT_TAB_OFFSET(App 才需要 0x4000); BL 自己在 0x08000000,
 *     SystemInit() 默认就把 VTOR 设成 FLASH_BASE 即 0x08000000, 正确
 *   - BL.sct: LR_IROM1 0x08000000 0x4000 (16 KB),
 *            RW_IRAM1 0x20000000 0x30000 (192 KB, 与芯片真实 SRAM 一致)
 */
#include "bootloader.h"
#include "n32h47x_48x.h"   /* __NOP() */

int main(void)
{
    /* 有 "OTAV" 标记 → 校验暂存区 CRC32 → 擦 App 区 → 搬运 → 清标记 → 跳 App
     * 无标记           → 直接跳 App
     * 两种情况都不会正常返回 */
    bl_run();

    /* 兜底: 理论上到不了这里 */
    for (;;)
    {
        __NOP();
    }
}
