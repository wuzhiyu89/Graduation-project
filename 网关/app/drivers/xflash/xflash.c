/**
 * @file  xflash.c
 * @brief 外挂 SPI NOR Flash 底层驱动（SPI1 + 软件 CS，W25Q 指令集）
 *
 * 设计要点（改之前先读）：
 *   1. **全阻塞、无 DMA、无中断** —— Flash 操作都在主循环里跑完。这样与
 *      LoRa 的时序关系简单可预测，也不会有"回调里动 SPI"的重入问题。
 *   2. **每一步等待都有上限** —— 循环上限 + 毫秒上限双保险。MISO 被拉死、
 *      芯片没焊、SPI 时钟没起来，都只会"失败返回"，不会卡死主循环。
 *   3. **失败一次就整体降级** —— 探测不到芯片时 `g_present=false`，
 *      后续所有接口直接返回 false，不再反复重试（避免每个周期都白等超时）。
 *   4. **不隐式擦除** —— NOR 只能 1→0，写之前必须由调用方擦。
 *      隐式擦会"顺手"擦掉同扇区的其它记录，是数据丢失的经典来源。
 */
#include "xflash.h"

#if EXT_FLASH_ENABLE

#include "n32h47x_48x.h"
#include "n32h47x_48x_gpio.h"
#include "n32h47x_48x_rcc.h"
#include "n32h47x_48x_spi.h"
#include "bsp_tim.h"
#include "log.h"

/* ==================== 引脚（新板，见 bsp_gpio.h 的引脚表） ==================== */
#define XF_CS_PORT          GPIOA
#define XF_CS_PIN           GPIO_PIN_4
#define XF_SCK_PIN          GPIO_PIN_5
#define XF_MISO_PIN         GPIO_PIN_6
#define XF_MOSI_PIN         GPIO_PIN_7

#define XF_CS_LOW()         GPIO_ResetBits(XF_CS_PORT, XF_CS_PIN)
#define XF_CS_HIGH()        GPIO_SetBits(XF_CS_PORT, XF_CS_PIN)

/* ==================== 指令 ==================== */
#define XF_CMD_WRITE_ENABLE 0x06u
#define XF_CMD_READ_STATUS  0x05u
#define XF_CMD_READ_DATA    0x03u
#define XF_CMD_PAGE_PROGRAM 0x02u
#define XF_CMD_SECTOR_ERASE 0x20u
#define XF_CMD_BLOCK_ERASE  0xD8u
#define XF_CMD_JEDEC_ID     0x9Fu
#define XF_CMD_RELEASE_PD   0xABu       /* 释放深度掉电，上电后先发一次更稳 */

/* ==================== 超时 ==================== */
#define XF_SPIN_GUARD       200000u     /* 单字节传输的循环上限 */
#define XF_READY_TIMEOUT_MS 3000u       /* 覆盖 64 KB 块擦最大 2 s */
#define XF_PROBE_TIMEOUT_MS 20u

/* ==================== 状态 ==================== */
static bool     g_inited;
static bool     g_present;
static uint32_t g_capacity;
static const char *g_name = "none";
static uint32_t g_err_cnt;

/* ==================== 底层字节收发 ==================== */

static uint8_t xf_xfer(uint8_t out)
{
    uint32_t guard;

    guard = XF_SPIN_GUARD;
    while (SPI_I2S_GetStatus(SPI1, SPI_I2S_TE_FLAG) == RESET)
    {
        if (--guard == 0u)
        {
            g_err_cnt++;
            return 0xFFu;
        }
    }
    SPI_I2S_TransmitData(SPI1, (uint16_t)out);

    guard = XF_SPIN_GUARD;
    while (SPI_I2S_GetStatus(SPI1, SPI_I2S_RNE_FLAG) == RESET)
    {
        if (--guard == 0u)
        {
            g_err_cnt++;
            return 0xFFu;
        }
    }
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

/** 发 24 位地址（高字节在前） */
static void xf_send_addr(uint32_t off)
{
    (void)xf_xfer((uint8_t)((off >> 16) & 0xFFu));
    (void)xf_xfer((uint8_t)((off >> 8) & 0xFFu));
    (void)xf_xfer((uint8_t)(off & 0xFFu));
}

static void xf_write_enable(void)
{
    XF_CS_LOW();
    (void)xf_xfer(XF_CMD_WRITE_ENABLE);
    XF_CS_HIGH();
}

bool xflash_wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = get_tick_ms();
    uint8_t  st = 0xFFu;

    for (;;)
    {
        XF_CS_LOW();
        (void)xf_xfer(XF_CMD_READ_STATUS);
        st = xf_xfer(0xFFu);
        XF_CS_HIGH();

        if ((st & 0x01u) == 0u)
        {
            return true;
        }
        if ((uint32_t)(get_tick_ms() - t0) >= timeout_ms)
        {
            g_err_cnt++;
            return false;
        }
    }
}

bool xflash_read_status(uint8_t *out)
{
    if ((out == NULL) || (!g_present)) { return false; }

    XF_CS_LOW();
    (void)xf_xfer(XF_CMD_READ_STATUS);
    *out = xf_xfer(0xFFu);
    XF_CS_HIGH();
    return true;
}

/* ==================== 初始化 ==================== */

static void xf_gpio_init(void)
{
    GPIO_InitType g;

    RCC_EnableAHB1PeriphClk(RCC_AHB_PERIPHEN_GPIOA, ENABLE);

    GPIO_InitStruct(&g);

    /* CS：普通推挽输出，默认高（不选中） */
    g.Pin             = XF_CS_PIN;
    g.GPIO_Mode       = GPIO_MODE_OUTPUT_PP;
    g.GPIO_Pull       = GPIO_PULL_UP;
    g.GPIO_Slew_Rate  = GPIO_SLEW_RATE_FAST;
    g.GPIO_Current    = GPIO_DC_8mA;
    g.GPIO_Alternate  = GPIO_NO_AF;
    GPIO_InitPeripheral(XF_CS_PORT, &g);
    XF_CS_HIGH();

    /* SCK / MOSI：AF1 推挽 */
    g.Pin             = XF_SCK_PIN | XF_MOSI_PIN;
    g.GPIO_Mode       = GPIO_MODE_AF_PP;
    g.GPIO_Pull       = GPIO_NO_PULL;
    g.GPIO_Slew_Rate  = GPIO_SLEW_RATE_FAST;
    g.GPIO_Current    = GPIO_DC_8mA;
    g.GPIO_Alternate  = GPIO_AF_SPI1_SCK_PA5;   /* PA5/PA7 同为 AF1 */
    GPIO_InitPeripheral(XF_CS_PORT, &g);

    /* MISO：主模式下必须为输入（与 LoRa 的 SPI3 同一写法） */
    g.Pin             = XF_MISO_PIN;
    g.GPIO_Mode       = GPIO_MODE_INPUT;
    g.GPIO_Pull       = GPIO_NO_PULL;
    g.GPIO_Alternate  = GPIO_AF_SPI1_MISO_PA6;
    GPIO_InitPeripheral(XF_CS_PORT, &g);
}

static void xf_spi_init(void)
{
    SPI_InitType s;

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_SPI1, ENABLE);

    SPI_InitStruct(&s);
    s.DataDirection = SPI_DIR_DOUBLELINE_FULLDUPLEX;
    s.SpiMode       = SPI_MODE_MASTER;
    s.DataLen       = SPI_DATA_SIZE_8BITS;
    s.CLKPOL        = SPI_CLKPOL_LOW;            /* Mode 0 */
    s.CLKPHA        = SPI_CLKPHA_FIRST_EDGE;
    s.NSS           = SPI_NSS_SOFT;
    s.BaudRatePres  = XFLASH_SPI_BR_PRES;
    s.FirstBit      = SPI_FB_MSB;
    s.CRCPoly       = 7u;
    SPI_Init(SPI1, &s);
    SPI_Enable(SPI1, ENABLE);

    /* 清掉可能残留的 RX 数据，避免第一次读就拿到脏字节 */
    (void)SPI_I2S_ReceiveData(SPI1);
}

bool xflash_id(uint8_t out[3])
{
    if (out == NULL) { return false; }

    XF_CS_LOW();
    (void)xf_xfer(XF_CMD_JEDEC_ID);
    out[0] = xf_xfer(0xFFu);
    out[1] = xf_xfer(0xFFu);
    out[2] = xf_xfer(0xFFu);
    XF_CS_HIGH();
    return true;
}

/** 按 JEDEC 的容量字节推算容量：0x15=2MB 0x16=4MB 0x17=8MB 0x18=16MB */
static uint32_t xf_capacity_from_id(uint8_t cap_code, const char **name)
{
    /* 0x15 -> 2^21, 0x16 -> 2^22, 0x17 -> 2^23, 0x18 -> 2^24 */
    if ((cap_code >= 0x10u) && (cap_code <= 0x1Fu))
    {
        uint32_t bytes = 1u << cap_code;

        if (cap_code == 0x17u)      { *name = "W25Q64(8MB)"; }
        else if (cap_code == 0x16u) { *name = "4MB"; }
        else if (cap_code == 0x18u) { *name = "16MB"; }
        else if (cap_code == 0x15u) { *name = "2MB"; }
        else                        { *name = "SPI-NOR"; }
        return bytes;
    }
    *name = "unknown";
    return 0u;
}

bool xflash_init(void)
{
    uint8_t id[3];
    uint32_t cap;

    if (g_inited)
    {
        return g_present;
    }
    g_inited = true;

    xf_gpio_init();
    xf_spi_init();

    /* 芯片可能停在上次掉电前的深度掉电模式，先放它出来（失败也无所谓） */
    XF_CS_LOW();
    (void)xf_xfer(XF_CMD_RELEASE_PD);
    XF_CS_HIGH();
    (void)xflash_wait_ready(XF_PROBE_TIMEOUT_MS);

    (void)xflash_id(id);

    /* 全 0 或全 1 = 总线上没人回答（没焊芯片 / MISO 被拉死 / SPI 没起来） */
    if (((id[0] == 0x00u) && (id[1] == 0x00u) && (id[2] == 0x00u)) ||
        ((id[0] == 0xFFu) && (id[1] == 0xFFu) && (id[2] == 0xFFu)))
    {
        g_present = false;
        g_name    = "absent";
        log_info("[XFLASH] 未检测到芯片 (JEDEC=%02X %02X %02X), 外挂 Flash 功能关闭\r\n",
                 id[0], id[1], id[2]);
        return false;
    }

    g_name = "SPI-NOR";
    cap    = xf_capacity_from_id(id[2], &g_name);
    if (cap == 0u)
    {
        /* ID 认得厂商但容量字节不认识：按 8 MB 用，并在日志里明说 */
        cap = XFLASH_CAPACITY_8MB;
        log_info("[XFLASH] 容量字节 0x%02X 不认识, 暂按 8MB 处理(分区表按 8MB 写死)\r\n",
                 id[2]);
    }
    g_capacity = cap;
    g_present  = true;

    log_info("[XFLASH] 就绪: JEDEC=%02X %02X %02X, %s, 容量 %u KB, SPI1 %u MHz(分频码=%u)\r\n",
             id[0], id[1], id[2], g_name,
             (unsigned)(g_capacity / 1024u),
             (unsigned)(120u / (2u << XFLASH_SPI_BR_PRES)),
             (unsigned)XFLASH_SPI_BR_PRES);
    return true;
}

bool     xflash_present(void)      { return g_present; }
uint32_t xflash_capacity(void)     { return g_present ? g_capacity : 0u; }
const char *xflash_name(void)      { return g_name; }
uint32_t xflash_error_count(void)  { return g_err_cnt; }

/* ==================== 读 ==================== */

bool xflash_read(uint32_t off, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t i;

    if ((!g_present) || (buf == NULL) || (len == 0u)) { return false; }
    if ((off >= g_capacity) || (len > (g_capacity - off))) { return false; }

    XF_CS_LOW();
    (void)xf_xfer(XF_CMD_READ_DATA);
    xf_send_addr(off);
    for (i = 0u; i < len; i++)
    {
        p[i] = xf_xfer(0xFFu);
    }
    XF_CS_HIGH();
    return true;
}

/* ==================== 写（按页拆分） ==================== */

static bool xf_page_program(uint32_t off, const uint8_t *p, uint32_t n)
{
    uint32_t i;

    xf_write_enable();
    if (!xflash_wait_ready(XF_PROBE_TIMEOUT_MS)) { return false; }

    XF_CS_LOW();
    (void)xf_xfer(XF_CMD_PAGE_PROGRAM);
    xf_send_addr(off);
    for (i = 0u; i < n; i++)
    {
        (void)xf_xfer(p[i]);
    }
    XF_CS_HIGH();

    return xflash_wait_ready(XF_READY_TIMEOUT_MS);
}

bool xflash_write(uint32_t off, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    if ((!g_present) || (buf == NULL) || (len == 0u)) { return false; }
    if ((off >= g_capacity) || (len > (g_capacity - off))) { return false; }

    while (len > 0u)
    {
        /* 一页 256 B，且【不能跨页边界】——跨页会回卷覆盖页首 */
        uint32_t page_left = XFLASH_PAGE_SIZE - (off & (XFLASH_PAGE_SIZE - 1u));
        uint32_t n         = (len < page_left) ? len : page_left;

        if (!xf_page_program(off, p, n)) { return false; }

        off += n;
        p   += n;
        len -= n;
    }
    return true;
}

/* ==================== 擦 ==================== */

static bool xf_erase_cmd(uint8_t cmd, uint32_t off, uint32_t align)
{
    if ((!g_present) || ((off % align) != 0u) || (off >= g_capacity)) { return false; }

    xf_write_enable();
    if (!xflash_wait_ready(XF_PROBE_TIMEOUT_MS)) { return false; }

    XF_CS_LOW();
    (void)xf_xfer(cmd);
    xf_send_addr(off);
    XF_CS_HIGH();

    return xflash_wait_ready(XF_READY_TIMEOUT_MS);
}

bool xflash_erase_4k(uint32_t off)
{
    return xf_erase_cmd(XF_CMD_SECTOR_ERASE, off, XFLASH_SECTOR_SIZE);
}

bool xflash_erase_64k(uint32_t off)
{
    return xf_erase_cmd(XF_CMD_BLOCK_ERASE, off, XFLASH_BLOCK_SIZE);
}

#else /* EXT_FLASH_ENABLE == 0 —— 老板：一个字节的 SPI 都不碰 */

bool     xflash_init(void)                     { return false; }
bool     xflash_present(void)                  { return false; }
uint32_t xflash_capacity(void)                 { return 0u; }
const char *xflash_name(void)                  { return "disabled"; }
uint32_t xflash_error_count(void)              { return 0u; }
bool     xflash_id(uint8_t out[3])             { (void)out; return false; }
bool     xflash_read(uint32_t o, void *b, uint32_t l)  { (void)o; (void)b; (void)l; return false; }
bool     xflash_write(uint32_t o, const void *b, uint32_t l) { (void)o; (void)b; (void)l; return false; }
bool     xflash_erase_4k(uint32_t off)         { (void)off; return false; }
bool     xflash_erase_64k(uint32_t off)        { (void)off; return false; }
bool     xflash_read_status(uint8_t *out)      { (void)out; return false; }
bool     xflash_wait_ready(uint32_t ms)        { (void)ms; return false; }

#endif /* EXT_FLASH_ENABLE */
