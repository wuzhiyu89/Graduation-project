# 外挂 SPI Flash 空间规划（v1.2，2026-11-11）

> 状态：**规划定稿 + 驱动/存储层已落地 + 队列已接上**。
> - 规划部分（本文 §1~§9）：已定"放什么、怎么分、怎么保证掉电不丢"。
> - 代码部分：`app/drivers/xflash/`（底层驱动 + 分区表 + META + 出厂固件区 + 数据环）。
> - `EXT_FLASH_ENABLE` 默认 **1**（新板 W25Q64 已实测）；没焊芯片时探测失败即整体 no-op。
>
> **v1.2（2026-11-11）**：META 从"主/备两份"改成 **16 槽轮转**（队列每 30 s 就要落一次
> 指针，两份交替约 35 天就能把单扇区磨到寿命上限）；数据环从"RAM 缓存满了才溢出"
> 改成 **队列本体**（方案 A）；恢复出厂会清空两个环。详见 §6。
>
> v1.1 相对 v1.0 的变化：**PA7 冲突已解除**（电源检测实际在 PC3，PA7 可直接做 SPI1_MOSI）、
> `EVENT` 1 MB→**2 MB**、`OTA` 1.5 MB→**512 KB**、**新增 `FW_FACTORY` 1 MB 出厂固件区**、
> 分区重排后 `FACTORY` 移到 `0x7A0000`、`SPARE` 缩到 320 KB。

## 1. 为什么加这颗 Flash

| 现状 | 问题 |
|---|---|
| 内部 512 KB 已排满：BL 16K / A 208K / B 208K / 标记 8K / 参数 8K / 配置 16K / 事件 48K | 没有空间再放大缓存 |
| 事件补报缓存 256 条（RAM 7 KB） | 低于文档"≥500 条"的指标；长断网会淘汰最旧的 |
| 遥测补报缓存 128 条（RAM 4 KB） | 4 台 × 300 s 只能存 **2.7 小时**；掉电即失 |
| OTA 只能整包放内部 Slot B | 中途断电就废掉一个槽，不能续传 |
| 日志只在 RAM 环形里 | 掉电即失，现场取证困难 |
| 没有出厂固件的第二副本 | 内部 A/B 双槽都坏就只能返厂 |

外挂 8 MB 后要解决的就是这六件事，**按优先级**：

| 优先级 | 放什么 | 解决什么 | 不做的后果 |
|---|---|---|---|
| **P0** | 液位遥测（逐条） | 断网/掉电能补报，天数级 | 现场断网一天，历史全丢 |
| **P0** | 事件（状态迁移/联动/设备上下线） | 满足"断线不丢 + ≥500 条" | 平台看到的事件流有洞 |
| **P0** | 出厂固件镜像 | 云端"恢复出厂设置"的最后一道保险 | 双槽都坏只能返厂 |
| **P1** | 运行日志（关键行） | 掉电后仍能取证 | 现场"死机"无法定位 |
| **P1** | OTA 整包暂存 | 下载可续传、先校验再写内部槽 | 一次断电要重下整包 |
| **P2** | 配置快照 + 出厂备份 | 配置区被写坏时可回滚 | 极端情况下要返厂 |
| **P3** | 预留区 | 后续（BLE、更大的环、坏块） | —— |

**明确不放**：原始 JSON 文本（浪费 6 倍空间）、高频波形/原始 ADC 采样、文件系统（FATFS 对本场景是负担）、
固件运行参数的**唯一副本**（内部 Flash 的 config 区仍是权威，外挂只是快照）。

## 2. 硬件与接口

### 2.1 选型

- 容量：**8 MB（64 Mbit）**，SPI NOR。
- 推荐型号：**W25Q64JVSSIQ**（SOP-8 208 mil，3.3 V，104 MHz）。
  可替换：GD25Q64E、MX25L6433F、ZB25VQ64 —— 指令集兼容，驱动不用改。
  JEDEC ID 容量字节按 `0x17` 认；不认识的容量按 8 MB 兜底并打日志。
- 封装/焊位：SOP-8，**新板已布**（老板子没焊，这也是驱动默认关闭的原因）。

### 2.2 接口（已定）

| 信号 | 引脚 | 复用 |
|---|---|---|
| CS | **PA4** | 软件控制（普通推挽输出，上电默认高），不用硬件 NSS |
| SCK | **PA5** | AF1（`GPIO_AF_SPI1_SCK_PA5`） |
| MISO | **PA6** | AF1（`GPIO_AF_SPI1_MISO_PA6`） |
| MOSI | **PA7** | AF1（`GPIO_AF_SPI1_MOSI_PA7`） |

- 外设：**SPI1**，主机模式，Mode 0（CPOL=0 / CPHA=1Edge），8 bit，MSB first。
- 初速：**15 MHz 起步**（`XFLASH_SPI_BR_PRES=3`，PCLK2=120 MHz ÷ 8）。
  实测波形干净后再往上提，目标 30 MHz（`XFLASH_SPI_BR_PRES=2`）。
- 与 LoRa 的 SPI3（PC10/11/12 + PA15）**物理独立**，不存在总线争用 —— 这是选 SPI1 而不是共用 SPI3 的主要原因。
- 上电默认：CS 拉高（不选中），WP#/HOLD# 上拉到 3.3 V（或软件里写状态寄存器关掉这两脚功能）。

### 2.3 PA7 冲突已解除（v1.1 更新）

**用户 2026-11-04 确认：电源电压检测在 PC3**（不是 PA7）。

- 老板子上 `app/service/power_mgr.c` 用的是 **PA7 = ADC2_IN2**，那份代码是给老板子写的；
  新板电源检测挪到 PC3 后，**PA4–PA7 整组空出来，直接给 SPI1**，不需要分时复用、不需要改板。
- `app/bsp/bsp_gpio.c` 顶部"PA6 是新板 BAT_AD"那句注释是过期的，按本文为准。

**驱动里怎么处理这个差异**：`EXT_FLASH_ENABLE` 一个宏管两头。

| 板子 | `EXT_FLASH_ENABLE` | 行为 |
|---|---|---|
| 老板（无 Flash，电池走 PA7） | **0**（默认） | `xflash_*` 全部 no-op 返回 false；SPI1/PA4–PA7 一个都不碰；电池采样照原样 |
| 新板（有 Flash，电源检测走 PC3） | **1** | 初始化 SPI1 + 四根脚，探测 JEDEC；读不到芯片自动降级，不阻塞主流程 |

> ⚠ 新板固件必须确保 `power_mgr` 已改用 PC3 再打开这个开关，否则 PA7 会被 SPI1 抢走、电池读数失效。

## 3. 分区表（8 MB = 128 × 64 KB 块）

全部按 **64 KB 块**对齐（块擦除快，环内再用 4 KB 子扇区），地址从 0 开始。
**这张表与 `app/drivers/xflash/xflash_store.c` 的 `s_regions[]` 必须逐字节一致**，改一边必须改另一边。

| 区 | 起始 | 大小 | 块数 | 放什么 |
|---|---|---|---|---|
| `META` | `0x000000` | 64 KB | 1 | 超级块 **16 槽轮转（各 4 KB）** + 坏块位图/寿命计数（预留） |
| `CFG` | `0x010000` | 64 KB | 1 | 配置快照，4 KB × 16 份，A/B 交替 + CRC |
| `EVENT` | `0x020000` | **2 MB** | 32 | 事件补报缓存（替代 RAM 的 256 条） |
| `TELEM` | `0x220000` | 3 MB | 48 | 液位遥测逐条记录（**最大的一块，数据量最高**） |
| `LOG` | `0x520000` | 1 MB | 16 | 关键日志（掉电保留） |
| `OTA` | `0x620000` | **512 KB** | 8 | OTA 整包暂存（断点续传） |
| `FW_FACTORY` | `0x6A0000` | **1 MB** | 16 | ★**出厂固件镜像**（恢复出厂用，运行期只读）★ |
| `FACTORY` | `0x7A0000` | 64 KB | 1 | 出厂参数备份：校准值、设备表默认值，双份 + CRC |
| `SPARE` | `0x7B0000` | 320 KB | 5 | 预留（BLE / 扩环 / 坏块替换） |
| **合计** | | **8 MB** | **128** | |

### 3.1 为什么这么分

- **TELEM 3 MB（最大）**：它按"每台每次上报一条"增长，是唯一会被日常流量打满的区。
- **EVENT 2 MB**（v1.0 是 1 MB）：事件是平台看到的"过程记录"，断网时比遥测更容易被漏看；
  单条 28 B，2 MB 能存 **约 7.4 万条**，足够覆盖"长断网 + 长期掉电"。
- **OTA 512 KB**（v1.0 是 1.5 MB）：内部 Slot 只有 208 KB，512 KB 已是 **2.5 倍余量**；
  真涨到 512 KB 以上先改这里再改分区，不要提前占空间。
- **FW_FACTORY 1 MB**：内部 App 区 208 KB 的 5 倍，够放整包 bin（甚至 bin + 压缩包）。
  见 §3.4。
- **LOG 1 MB**：只记关键行，够存一周以上（见 §3.3）。
- **META 只占 1 块**：它写得最频繁（每次推进写指针），单独一块方便整块擦。

### 3.2 容量实算（这才是"能存多久"的答案）

遥测记录 32 B/条（与 RAM 的 `tc_record_t` 同构，可直接 memcpy）：

| 场景 | 产生速率 | 3 MB 可存 |
|---|---|---|
| 4 台 × 300 s（生产默认） | 48 条/时 | 98,304 条 → **约 85 天** |
| 4 台 × 60 s（调试/高精度） | 240 条/时 | **约 17 天** |
| 1 台 × 300 s | 12 条/时 | **约 284 天** |
| 4 台 × 30 s（极限） | 480 条/时 | **约 8.5 天** |

事件记录 28 B/条，**2 MB = 74,898 条**：按 200 条/天算 **约 375 天**，按 1000 条/天算 **约 75 天**。

> 对比现状：RAM 遥测 128 条 ≈ **2.7 小时**（4 台 300 s）。扩到 Flash 后是 **85 天**，约 **2000 倍**。
> 8 MB 已经能覆盖"半个月到三个月"的所有合理需求 —— 除非要存 30 s 周期 + 全量，那才需要再议容量。

### 3.3 日志区格式（★2026-11-11 已实现，格式按实际代码订正★）

与 EVENT/TELEM 共用同一套记录格式，**64 B 定长槽**：

```
12 B 记录头  [magic(2) | len(2) | seq(4) | crc32(4)]
52 B 负载    [ts(4) | tick_ms(4) | len(1) | flags(1) | rsv(2) | 文本(≤40)]
```

- `ts` = UTC 秒（未同步时为 0，此时看 `tick_ms`）；`flags.bit0` = 文本被截断；
- 正文超长按 **UTF-8 字符边界**截断，绝不切出半个汉字；
- 1 MB = **16,384 条**。

**两条必须遵守的记录策略**（都是实测踩出来的）：

1. **不记 `[STAT]` / `[STAT2]`**：它们每秒各一条（17 万条/天），会把环彻底淹没 ——
   实测环里最新 25 条全是 `[STAT]`，一条有效事件都没留下。
   `app_status_report()` 首尾用 `xf_log_pause_capture()` 屏蔽落盘，串口照常打印。
2. **启动阶段直写、稳态走队列**：主循环起来之前 `xf_log_poll()` 不会被调用，
   而 `[BOOT]`/`[XFLASH]`/`[G4]`/`[LORA]` 在几毫秒内打上百行，队列瞬间满
   （实测丢 226 条）。所以启动阶段 `xf_log_set_direct(true)` 每行直接写盘，
   进主循环前才切回队列；稳态限速 12 条/秒。

导出：串口 `#flash log [dump [n] | stat | clear | on | off]`，默认打印最新 40 条。
后续可加云端 `sys.log.dump`（本期不做）。

> 实测（2026-11-11 烧录验证）：启动 199 条有效日志完整落盘，含 `[BOOT]` 全流程、
> 4G 建连、MQTT 上报；静置 90 s 后 `dropped` 停在 171 不再增长 —— 丢的只是建连期
> `[G4_TX]` 回显的一次性爆发，稳态零丢弃。

### 3.4 出厂固件区（`FW_FACTORY`，新增）

用途：云端下发 **"恢复出厂设置"** 时，把设备拉回一个**确定的、出厂的固件版本**，
即使当前固件跑飞、内部 A/B 双槽都坏也能救回来。

布局（`xf_fw_header_t` 定义见 `xflash_store.h`）：

```
0x6A0000  ┌──────────────────────────────┐
          │ 头 64 B（占满 1 个 4 KB 扇区）│  magic "FWF1" / size / crc32 / md5 /
          │  其余 0xFF                    │  version / ver_str / flags / hdr_crc
0x6A1000  ├──────────────────────────────┤
          │ 镜像正文（整包 bin）          │  ≤ 1 MB - 4 KB
          │  ...                          │
0x7A0000  └──────────────────────────────┘
```

- **头单独占一个扇区**：NOR 的擦除粒度是 4 KB，头与正文同扇区时"只更新头"就必须重写镜像开头 4 KB。
  分开后头与正文互不牵连。
- **写顺序（产线/工装）**：擦全区 → 写正文 → 回读校验 CRC32/MD5 → **最后写头**。
  "头写了"才代表这份镜像可用，中途断电不会留下"半份可用"的假象。
- **运行期只读**：OTA 流程永远不碰这个区，它是"不可变基线"。
- **恢复出厂流程（后续实现）**：
  1. 云端 `sys.factory.reset`（或长按按键 N 秒）→ 校验 `xf_fw_factory_present()`；
  2. 分块把镜像读出来，写进内部 Slot（与 OTA 写槽同一条路径，逐块 CRC 校验）；
  3. 写标记页 → 重启从该槽启动；
  4. 配置区恢复成 `FACTORY` 备份里的默认值（先备份用户配置到 LOG 区留证）。
- **没写镜像时**：`xf_fw_factory_header()` 返回 false，日志打"出厂固件区为空(产线尚未写入)"，
  `sys.factory.reset` 应**拒绝执行并明确回报**，不能"假装成功然后重启"。

## 4. 记录格式

三个环共用一套（按区改魔数），**只追加、不原地改**：

```
偏移  长度  字段
0     2     魔数：EVENT=0xE1C5 / TELEM=0xE2C5 / LOG=0xE3C5
2     1     版本 = 1
3     1     标志位（bit0 = 已确认送达；其余保留）
4     4     序号 seq（单调递增，跨回绕，用于排序/去重/断点续传）
8     4     ts（Unix 秒，原始发生时刻，补报不改）
12    2     负载长度 N
14    2     保留（对齐）
16    N     负载（EVENT=ec_record_t 的字段 / TELEM=tc_record_t 去掉 ts / LOG=64 B 定长）
16+N  2     CRC16-CCITT（覆盖 0 .. 16+N-1）
```

- **掉电判据**：读记录时魔数 + 长度合法 + CRC 通过才算有效；最后一条写一半 → CRC 不过 → 当作环尾。
- **为什么 CRC 放在记录尾部**：写是按字节顺序落到 NOR 的，最后写的字节如果没落下去，CRC 必然不匹配 ——
  不需要额外的"提交标记"，也不需要双写。
- **跨 4 KB 边界**：整条记录不跨扇区；放不下就跳到下一个扇区开头，前面剩余字节填 `0xFF`。

## 5. 环形管理、磨损与掉电恢复

- **单位**：环内以 **4 KB 子扇区**为单位擦写（W25Q64 的最小擦除是 4 KB）；OTA 区用 **64 KB 块擦**（快 8 倍）。
- **写指针推进**：写满当前子扇区 → 先擦**下一个**子扇区 → 再写。**绝不边写边擦**。
- **磨损核算**（这是"要不要做磨损均衡"的答案）：
  - TELEM：48 条/时 × 32 B = 1.5 KB/时 ⇒ 一个 4 KB 扇区约 **2.7 小时**写满；
    3 MB = 768 个扇区 ⇒ 单扇区约 **85 天**轮到一次 ⇒ **约 4.3 次/年**。
  - EVENT：2 MB = 512 个扇区，单扇区更慢，可以忽略。
  - W25Q64 的 4 KB 扇区保证 **10 万次**擦写 ⇒ 寿命 **> 2 万年**。
  - **结论：线性环 + 顺序推进就够，不需要 FTL、不需要动态磨损均衡**（别把简单问题做复杂）。
- **掉电恢复**（★2026-11-11 起 META 是 16 槽轮转，不再只有主/备两份）：
  1. 扫 `META` 的 16 个槽（各 4 KB，带 CRC + 递增 seq），取 seq 最大且校验通过的那份；
     只在开机扫一次，之后走 RAM 缓存（落盘是每 30 s 一次的常规动作，每次都扫 16 个扇区
     ≈ 90 ms 会把主循环拖死）；
  2. 从记录里的 `head` **向前**扫描连续有效记录（magic + len + CRC），把"写了但没落指针"
     的那几条补回来；`head` 正好停在扇区边界时不扫（下一扇区可能是上一轮的旧数据）；
  3. 16 份全坏 → 走格式化重建（`seq` 取"现有最大 + 1"，避免旧份压过新份）。
- **坏块**：NOR 出厂坏块极少（<1%），但 4 KB 扇区可能擦失败。擦除返回失败 → 在 META 的坏块位图里标记，跳过该扇区。
  **不做**复杂的坏块重映射（没必要）。

## 6. 与缓存的关系（★2026-11-11 起：Flash 就是队列，RAM 不留副本★）

**方案 A 已实施**（用户 2026-11-11 拍板，见 `docs/缓存分层规划.md` §4）：
待补报的遥测/事件**直接写进 Flash 环**，RAM 里不再保留 128/256 条的副本。

```
传感器 0x02 上报（发不出去时）
   ├─► last_value（最新值，供遥测/告警用）        —— RAM，不变
   └─► TELEM / EVENT 环（Flash）                  —— 唯一副本
```

- **什么时候写**：只在"发不出去"时（离线 / publish 失败）。在线且成功 → 根本不进队列，
  **零 Flash 写**（用户 Q1）。
- **顺序**：写入顺序 = 采集顺序 = 补报顺序，严格按 `ts` 递增（用户 Q2）。以前
  "RAM 装尾段、Flash 装头段"导致的倒挂随之消失。
- **掉电**：记录写入那一刻就在 Flash 里了，META 只是指针；开机顺着 `head` 向前扫描
  补回"写了但没落指针"的那几条。尾部指针落后最坏导致重复补报几条，而补报 msgId 是
  `(uid, ts, msg_seq)` 确定性重建的，平台按 msgId 去重即可。
- **失败**：Flash 不在 / 擦写失败 → 该条计入 `dropped` 并打日志，**不重试、不阻塞**
  （补报跑在主循环里，卡住它比丢一条更糟）。开发阶段已确认"现在都是新板子"，
  所以没有 RAM 后端兜底（用户 Q3）。
- **清空**：恢复出厂（`sys.factory_reset`）会调 `telemetry_cache_reset()` /
  `event_cache_reset()`，把两个环的 head/tail 归零 —— 否则换绑后会把上一位用户的
  历史补报给新账号。

## 7. OTA 外置暂存

```
1. ota.start（总大小/CRC/版本）-> 应答受理，擦 OTA 区前 N 块（512 KB = 8 块）
2. 分包下载 -> 顺序写入 OTA，每包把进度写进 META（8 KB 一存，不是每包都写）
3. 收齐 -> 校验整包 CRC32 + 版本号 + 机型
4. 通过 -> 分块读出，擦/写内部 Slot B（208 KB，一次 4 KB）
5. 写内部标记页 -> 重启切槽
6. 新固件起来后清 OTA 区与进度
```

收益：**中途断电可续传**（按已收字节数继续），不再"一次失败废一个槽"。

> ⚠ 前提：内部 App 区 208 KB ⇒ 固件体积必须 < 208 KB（当前 **131.7 KB**，余量约 76 KB）。
> OTA 区 512 KB 是 2.5 倍余量；若哪天固件接近 512 KB，必须先把 OTA 区扩到 1 MB 以上再发版。

## 8. 驱动分层（已落地 1/2 层，见「代码状态」）

```c
/* 层 1：xflash.c —— 纯芯片操作，不认识业务           ★已实现*/
bool     xflash_init(void);                       /* 读 JEDEC ID，识别容量，超时保护 */
bool     xflash_present(void);
uint32_t xflash_capacity(void);
const char *xflash_name(void);
bool     xflash_read(uint32_t off, void *buf, uint32_t len);
bool     xflash_write(uint32_t off, const void *buf, uint32_t len);  /* 自动按 256 B 页拆 */
bool     xflash_erase_4k(uint32_t off);
bool     xflash_erase_64k(uint32_t off);
bool     xflash_wait_ready(uint32_t timeout_ms);
uint32_t xflash_error_count(void);

/* 层 2：xflash_store.c —— 分区表 + 超级块 + 出厂固件区   ★已实现*/
const xf_region_info_t *xf_region_info(xf_region_t r);
bool     xf_store_init(void);       /* 探测 + 读/建 META */
bool     xf_store_ready(void);
bool     xf_meta_read(xf_meta_t *out);
bool     xf_meta_write(xf_meta_t *m);   /* 16 槽轮转 + seq 递增 + 回读校验 */
void     xf_meta_invalidate(void);       /* 丢掉 RAM 缓存，强制重扫（工装/调试用） */
bool     xf_store_format(void);
void     xf_store_dump(void);           /* #flash 打印分区表/超级块 */
/* 数据环（★2026-11-11 起是队列本体，不再是溢出层） */
bool     xf_ring_push(xf_ring_id_t id, const void *rec, uint16_t len);
bool     xf_ring_peek(xf_ring_id_t id, void *buf, uint16_t cap, uint16_t *out_len);
bool     xf_ring_peek_at(xf_ring_id_t id, uint32_t idx, void *buf, uint16_t cap, uint16_t *out_len);
bool     xf_ring_pop(xf_ring_id_t id, void *buf, uint16_t cap, uint16_t *out_len);
bool     xf_ring_drop_n(xf_ring_id_t id, uint32_t n);   /* 补报一页后集中出队 */
uint32_t xf_ring_count(xf_ring_id_t id);
bool     xf_ring_clear(xf_ring_id_t id);                /* 恢复出厂/换绑 */
void     xf_ring_poll(void);                            /* 空闲 30 s 落 META */
/* 出厂固件区 */
bool     xf_fw_factory_header(xf_fw_header_t *out);
bool     xf_fw_factory_present(void);
bool     xf_fw_factory_read(uint32_t off, void *buf, uint32_t len);
bool     xf_fw_factory_erase(void);
bool     xf_fw_factory_write(uint32_t off, const void *buf, uint32_t len);
bool     xf_fw_factory_set_header(const xf_fw_header_t *h);

/* 层 3：记录环（EVENT/TELEM/LOG 共用）—— 待实现 */
bool     xring_push(xring_id_t id, uint32_t ts, const void *payload, uint16_t len);
bool     xring_peek(xring_id_t id, xring_rec_t *out);
void     xring_pop(xring_id_t id);
uint32_t xring_count(xring_id_t id);

/* 层 4：业务适配 —— 只在这层碰 telemetry_cache / event_cache 的记录结构 —— 待实现 */
bool     xflash_telem_append(const tc_record_t *rec);
bool     xflash_event_append(const ec_record_t *rec);
```

**关键约束**：层 1/2/3 必须能在**主机上跑**（用文件模拟 8 MB），否则没法做单元测试 ——
沿用现有 `_host_test` 的做法，下一步给 `xflash` 加一个 `XF_HOST_SIM` 后端（S0 的后半段）。

**超时保护（已实现）**：单字节收发有 `XF_SPIN_GUARD` 循环上限，芯片忙等有毫秒上限
（扇区擦 3 s、块擦 3 s），探测失败即 `g_present=false` **永久降级**，后续调用直接返回 false。
任何情况下都不会因为 MISO 被拉死而卡住主循环。

## 9. 资源开销（实测）

| 项 | 增量 |
|---|---|
| 代码（层 1+2 + 诊断命令） | **+2,960 B**（Code 131,652 → 134,612） |
| 静态 RAM | **+8 B**（RW 428 → 436），页缓冲走栈，无 4 KB 静态缓冲 |
| 启动时间 | 探测 + 读 META < 5 ms；芯片不在时几乎为 0 |
| 后续（层 3+4 全做完） | 预计 +6~9 KB Flash、+4.5 KB RAM |

## 10. 落地顺序（按依赖排，不要并行）

| 阶段 | 内容 | 状态 |
|---|---|---|
| **S0a** | `xflash` 层（读/写/擦/ID/超时/降级） | ✅ 代码完成，双开关编译通过 |
| **S0b** | 主机模拟后端 `XF_HOST_SIM` + 主机单测 | ⬜ 下一步（不需要板子） |
| **S1** | `xflash_store`：分区表 + `META` 16 槽轮转 + 掉电恢复 + 出厂固件区 | ✅ 完成（`test_xflash` 100 项） |
| **S2** | `TELEM` 环接 `telemetry_cache_push`（Flash 单写，删 RAM 环） | ✅ 完成（`test_telemetry_cache` 37 项） |
| **S3** | `EVENT` 环接 `event_cache_push`（Flash 单写 + 待提交槽合并） | ✅ 完成（`test_event_cache` 65 项） |
| **S4** | `LOG` 环 + `#flash log dump` | ✅ 完成（`test_xf_log` 61 项，已上板验证） |
| **S5** | `OTA` 区 + 续传 | ⬜ |
| **S6** | 出厂固件写入（产线工装）+ `sys.factory.reset` | ⬜ 待平台协议 |

**先做 S0b/S1 的单测**：它们是所有区的地基，且完全可以在没有板子的情况下用主机模拟写完并测完。

## 11. 待确认

1. **新板焊位**：SOP-8 位置已布（用户口述"新板在路上"），到货后第一件事是 `#flash` 看 JEDEC 是否为 `EF 40 17`。
2. ~~**`power_mgr` 何时改到 PC3**~~ —— **已解决（2026-11-11）**：核对后确认那套 ADC
   采样从未进过 `CDC.uvprojx`、无任何调用点，且网关电池电压实际由 4G 模组 AT 查询
   得到（`g4_query_vbat_mv`）。整个 `power_mgr.c/.h` 已删除，PA7 冲突不复存在。
   将来若要本地测电池，用 **PC3 + 对应 ADC 通道**，别再碰 PA7。
3. ~~**电池采样频率**~~ —— 随 `power_mgr.c` 一并删除，此项不再适用。网关电池电压
   走 4G 模组 AT 查询（日志里 `VBAT=4144mV`），采样频率由模组侧决定。
4. **出厂固件的写入方式**：产线工装通过串口 `#flash fwif`（本期未实现）还是烧录器直接写？
   —— 影响是否需要实现"分段接收 + 校验"的串口命令。
5. **恢复出厂的触发源**：云端命令、按键长按、还是两者都要？配置是否一起恢复（`FACTORY` 备份）？
6. **日志是否要云端拉取**（`sys.log.dump`）？本期建议只做串口导出。
7. **遥测量**：TELEM 3 MB 是按"每条 0x02 上报都存"算的。如果后期要存"30 s 聚合遥测"而不是逐条，
   格式和容量都要重算（会小很多）。
