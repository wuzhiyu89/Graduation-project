# N32H482-Gateway

基于国民技术 **N32H482RE** 的「4G + LoRa」工业网关固件。

- **本地**：LoRa（LLCC68 / SX1261，433 MHz）连接液位传感器（毕设版只有液位这一类从机）
- **上云**：4G 模组（ML307R/ML307Y，UART AT）跑 MQTT，对接 RoadTwin 云协议
- **自治**：水位状态评估 + 失联判定；断网时事件/遥测缓存、恢复后按原时间戳补报
- **运维**：`#` 串口控制台、OTA over HTTP（+MD5）、A/B 健康计数
- **产线**：不插 SIM 也能 `#bind` / `#set` / `#get` / `#unbind` 写出厂字段

> **毕设版范围**：本项目为毕业设计裁剪版，只保留液位传感器数据通道。原独立项目中的
> 「声光报警器 / 一体机」联动设备、`dev.*` / `rule.*` 云命令、`event.action` /
> `event.status_change` 事件均已从固件删除。`MDK-ARM/网关云平台对接使用说明.md`
> 与 `docs/ARCHITECTURE.md` 中的相关段落属历史协议存档，描述的是旧版全功能协议，
> **对毕设版不再适用**；当前生效命令见下文「云协议」与 `app/cloud/cloud_proto.c`。

> 架构、数据流、模块职责、约定与坑：见 **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**。

## 目录结构

```
N32H482-Gateway/
├── app/
│   ├── bsp/            板级支撑：GPIO / 定时器 / 串口
│   ├── common/         通用：CRC/UTF-8、Flash 存储、分区表、环形缓冲
│   ├── drivers/lora/   LLCC68 驱动 + 射频抽象 + 板级引脚
│   ├── links/g4/       4G 链路：AT 传输、URC、建链状态机
│   ├── links/lora/     LoRa 链路：帧协议、从机表、绑定/解绑、出厂字段
│   ├── links/serial/   RS232/RS485 通道（协议待定，仅建通道）
│   ├── cloud/          云协议：信封/13 条命令(sys/cfg/sen/ota)/上行构造、JSON 读写
│   ├── model/          配置模型：6 个配置域、掉电参数、最后值
│   ├── service/        业务服务：水位状态评估、失联判定、事件/遥测缓存、本地告警
│   ├── ota/            OTA：HTTP 下载、分区写入、MD5、A/B 健康
│   ├── log/            日志口（USART1）
│   └── cjson/          cJSON + 静态池适配
├── src/                main.c（启动 + 主循环）、中断向量
├── inc/                公共头文件
├── bsp/                延时 / 日志接口（SDK 支撑层）
├── firmware/           N32H48x 标准外设库（勿改）
├── MDK-ARM/            Keil 工程（CDC.uvprojx）
├── _host_test/         主机测试（gcc，18 套件）
├── archive/            已停用代码（不参与构建，见 archive/README.md）
├── docs/               架构文档
└── 相关资料/            4G / LoRa / MCU 原厂资料
```

## 开发环境

- Keil MDK V5.43a / ARMClang V6.24（target `N32H480` 与 `N32H48X` 现在完全一致）
- gcc（MinGW-w64）+ Python 3（只为主机测试与脚本）
- 芯片：N32H482RE（Cortex-M4，512 KB Flash，192 KB RAM，**栈 4 KB / 堆 512 B**）

## ⚠️ 分区与 BootLoader（改前必读）

片内 Flash 分区的唯一真相源是 `app/common/flash_map.h`（模式 2，双槽 A/B 各 **224 KB**）。
`BL_APP_MAX_SIZE` / `BL_META_ADDR` 是**编译进 BL** 的常量，而现场设备只能 OTA 到 App，
**永远改不到 BL**。因此：

- 改 `FM_SLOT_SIZE` 或任何区地址，必须重编（`tools\build_bl.ps1`）并**同时重烧 BL + App-A**
  （`tools\flash_bl_app.ps1`）。META / PARAM / CFG 三个区会一起平移，首次上电后
  gwId / broker / 认证与全部配置回落出厂默认，**必须重新 provision**。
- **开发阶段可以随便改；量产后 Slot 大小再变就是返厂。**

### 上板核对（2026-11-12 按实际代码复核，不依赖文档注释）

| 项 | 实际情况 |
|---|---|
| `#flash info` | **外挂 SPI Flash** 诊断（`app/core/app_console.c:425`），打印 META / EVENT / TELEM / OTA 等外置分区，**不打印片内分区**，不能用来验收本次重分区 |
| `#status` | 只组一条 `status.report` 报文（`app_console.c:456`），也不是分区信息 |
| 片内分区验证 | 用 J-Link 读回：`0x08000000` BL 向量表、`0x08004000` App-A 向量表、`0x08074000` META、`0x08076000` PARAM、`0x08078000` CFG-A、`0x0807A000` CFG-B |
| 日志口 | **COM21**（`USB-SERIAL CH340`），115200 8N1，已用系统设备枚举核对 |
| J-Link | `C:\Program Files\SEGGER\JLink_V974\JLink.exe`（已确认存在） |

抓启动日志（默认 45 s，可加 `-Seconds 30`）：

```powershell
powershell -ExecutionPolicy Bypass -File _host_test\_capture_com.ps1 -Seconds 30
```

发单条控制台命令：把命令按原始字节写进 `_host_test\_cmd.txt`，再跑
`powershell -ExecutionPolicy Bypass -File _host_test\_send_cmd.ps1 -Port COM21`
（`-Port` / `-Baud` 可覆盖，默认 COM21 / 115200）。

### 管理面 Topic 取证（2026-11-12 任务三）

主题共 10 条：业务面 `rt/gw/{gwId}/{cmd,cmdresp,telemetry,event,status,lwt}`，
管理面 `rt/mgmt/{gwId}/{cmd,cmdresp,checkin,ota.progress}`。两面共用同一个 broker
和同一张命令表，只按**完整 topic** 精确分流（两面 `/cmd` 后缀相同，只看后缀必然串面）。

| 命令 | 用途 |
|---|---|
| `#topic` | 列出当前档位 10 条 topic / QoS / retained；本地档位管理面标为"不启用" |
| `#jsonm <payload>` | 用管理面完整 cmd topic 注入下行，验收应答走 `rt/mgmt/.../cmdresp` |
| `#json <payload>` | 原有离线注入（topic `(console)`，按业务面兼容路径处理），行为未改 |

> ⚠️ 现场 broker 的 ACL 把 publish/sub 权限绑在 `clientId == gwId` 上：从 PC 用随机
> clientId 发 `SUBSCRIBE`/`PUBLISH` 会被立即断连，用 `gwId` 又会把设备挤下线。
> 所以**不要**指望 `mosquitto_pub -t 'rt/mgmt/.../cmd'` 从外部验收，用 `#jsonm`。
> 平台侧要用管理账号下发，需给该账号配 `rt/mgmt/#` 的 publish 权限。
> 详细实测记录见 `docs/下一轮开工任务书-20261112.md` §4.6。

> BootLoader 自身**没有日志输出**（`build_bl.ps1` 的源文件列表里没有 UART / log 驱动）。
> 上电后第一段可见日志来自 App；BL 两个槽的向量表都不可信时会静默停住等 J-Link，
> 此时读 `0x08004000` 的向量表即可判断，不要一直等串口输出。

## 构建

图形界面：打开 `MDK-ARM/CDC.uvprojx`，Build。

命令行（推荐用脚本，见下）：

```powershell
$log = "$env:TEMP\keil.log"
$p = Start-Process 'D:\Keil_v5\UV4\UV4.exe' `
     -ArgumentList '-j0','-b','MDK-ARM\CDC.uvprojx','-o',$log -Wait -PassThru
Get-Content $log -Tail 20
```

产物：`MDK-ARM\bin\CDC.bin`（`fromelf` 由 Keil 的 After-Build 命令生成）。

> **判断标准**：日志里必须是 `0 Error(s)`；退出码 `0`=无错误无警告，`1`=有警告，`≥2`=有错误。
> 同一个工程在 uVision 里开着时，命令行构建不可靠，先关掉。

## 测试（不用板子）

```powershell
powershell -ExecutionPolicy Bypass -File _host_test\run.ps1
```

- 18 个套件，跑在 PC 上（假 ML307R / 假 Flash / 假 LoRa）
- 全绿时最后一行是 `all host tests passed`，退出码 0

一键验证（构建 + 测试）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify.ps1
```

## ⚠️ 脚本必须存成 UTF-8 with BOM

PowerShell 5.1 对**没有 BOM** 的 `.ps1` 按 GBK 解码，中文注释行末的全角标点会把
**下一行整行吞掉**（曾导致测试套件的源文件列表少一项，测试照跑但少测）。
改完任何 `.ps1` 后跑一次：

```powershell
python _host_test\check_ps1.py     # 期望：危险行数 = 0
```

## 运行时模式

编译期三选一（见 `app/drivers/lora/project_config.h`，也可在 Keil 的
C/C++ → Define 里覆盖）：

| 宏 | 模式 | 说明 |
|---|---|---|
| `MAIN_MODE_CLOUD`（默认） | 云端联调 | 4G + LoRa + 云协议全套 |
| `MAIN_MODE_PAIRING` | 从机配对 | 只起射频，`0x04` 配对请求回 ACK |
| `MAIN_MODE_LORA_TEST` | 射频测试 | 每 5 s 发一帧，其余时间收 |

三者必须恰好选一个，多选/少选直接编译报错。
