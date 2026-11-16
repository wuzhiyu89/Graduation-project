# 固件实现 vs 甲方需求 V0.4 差异清单

对照文档：
- 《RoadTwin 网关配置项设计》V0.4（2026-10-28）
- 《RoadTwin 平台-网关通信协议》V0.4（2026-10-28）

对照基线：`MDK-ARM\CDC.uvprojx` 提交版（1.0.0，A 区，-O1）。
结论：**协议骨架已对齐（20 条命令、9 个配置域、错误码、主题/QoS/遗嘱、A/B OTA 全在）**，
剩下的是 **6 条功能缺失 + 8 条口径不一致 + 2 条"多做了"**。按影响排序如下。

> **2026-10-28 更新：A 类 6 项已全部实现（1.0.1）。**
> 涉及条目：1（distance 换算）、2（1004）、3（meta 回 1001）、5（minChangeCm）、
> 6（拒 type=2）、12（configVersion 改 uint16）。
> 第 4 条（`offlineTimeoutSeconds`）按用户裁决不动，改为请甲方把约束写成
> "必须 > 3×reportPeriodSeconds"；第 7~11、13~17 条为口径/产品口径问题，
> 见 `REPLY_TO_CUSTOMER_V0.4.md`。
> 验证：主机测试 14 套件全绿（test_level 107→123、test_telemetry_cache 28→34），
> Keil `0 Error / 0 Warning`，`Code=150612 RO=49472 RW=428 ZI=177924`
> （RAM 178,352 B / 90.7%，Flash 200,512 B / 94.1%，A 槽余 12,480 B）。

---

## 一、没做到（功能缺失，需要动代码）

### 1. `distance` 测量模式没有实现 —— 影响最大 —— ✅ 1.0.1 已修
> **⚠️ 2026-10-30 口径变更：本条已作废，不要按它改代码。**
> 用户核对液位设备固件后确认：传感器侧已实现 `0x01` 的
> `[6] 测量模式 / [7-8] 安装高度 / [9-10] 校准偏移`，并在**设备内部**完成
> distance → 液位 的换算，0x02 帧报上来的就是液位值。
> 因此网关【**不做**换算、原样透传】，`rawMm` 与 `levelCm` 恒相等。
> A1 曾在 `data_cache_up_push()` 里加的那次换算已撤销——否则 distance 模式
> 会重复扣减安装高度，水位系统性偏低。
> 下文保留原始分析，仅作历史记录。

- 文档 3.3 / §4 第 1 步：`measureMode=distance` 时，网关要算
  `水深 = installHeightCm − 距离 + calibOffsetMm`，结果**可为负、不钳位**。
- 现状：三个字段只在配置里存取 + 原样下发传感器，网关侧**零换算**。
  - `app/core/app_uplink.c:270` 明写 `raw_mm = level_mm; /* 液位透传(D-1): 网关不做换算 */`
  - `config_store.h:239-242` 把三者标注为"D-1 决策后为死配置"
- 后果：现场只要挂一台 distance 型传感器（配置文档 §7 示例里就有一台），
  平台拿到的 `levelCm` 是"探头到水面距离"而不是水深，水位分级全错。
- 工作量：换算 + 校准 + 负值处理约 30 行，但会牵动 `rawMm`/`levelCm` 两个字段的口径，
  必须同步改 `test_level` 与遥测断言。

### 2. `sen.set` 目标传感器离线不回 1004 —— ✅ 1.0.1 已修
- 文档 §5.3 第 9 条（1027 联调拍板）：目标传感器离线 → 1004。
- 现状：`CLOUD_EC_DEV_OFFLINE = 1004` **定义了但全工程无人返回**
  （`grep` 只有 `cloud_proto.c:442` 的错误码文案）。`cloud_cmd_sen_set` 直接进
  `cloud_cmd_set_entry`，只校验只读字段，不查在线状态。

### 3. `cfg.set` 携带 `meta` 域没有拒绝 —— ✅ 1.0.1 已修
- 文档 §5.2 / §3.9：`sections` 带 `meta` → **显式拒绝、回 1001**。
- 现状：`cloud_cmd_cfg.c:114-117` 把 `meta` 列为"认识的域"，
  `config_store_merge_sections()` 对它**静默忽略** → 回 `code 0`，
  `applied`/`rejected` 都不含 meta。平台会以为"改了但没生效"。

### 4. `offlineTimeoutSeconds` 是死字段，离线判定用的是 3×上报周期 —— ⏸ 不改（已向甲方提出修订约束）
- 文档 3.3：按 `offlineTimeoutSeconds`（默认 90）判离线。
- 现状：`app/service/sensor_offline.c:12-22` 固定 `3 × reportPeriodSeconds`
  （`SENSOR_OFFLINE_PERIOD_MULT = 3`）；配置字段只在解析/校验/序列化出现，
  无任何消费者（`config_store.c:561` 自述）。
- 后果：周期 60s 时我们 180s 才判离线，比文档晚 90s，平台的"离线告警"会迟到。

### 5. 遥测的 `minChangeCm` 提前触发被关掉 —— ✅ 1.0.1 已修
- 文档 §7.1：周期上报 **或** 水位变化超过 `minChangeCm` 时触发。
- 现状：`app_uplink.c:387-393` 整段被 `#if 0` 关掉，只保留周期触发
  （2026-10-18 为"遥测与心跳严格同拍"做的取舍）。水位突变要等满一个周期才上报。

### 6. `dev.bind` 接受 `type=2`（纯 LED） —— ✅ 1.0.1 已修
- 文档 §5.4：type 只支持 1/3，type=2 暂无 uid 编号空间、**暂不支持绑定**。
- 现状：`cloud_cmd_devices.c:653-663` 判 `1 ≤ type ≤ 3` 即通过，type=2 能绑进去。

---

## 二、口径不一致（改常量/一小段逻辑即可）

| # | 项 | 文档 V0.4 | 现状 | 位置 |
|---|---|---|---|---|
| 7 | 容量上限 | sensors 1~5、devices 0~5 各自独立 | 总数≤5、液位≤4、声光≤3 | `config_store.c:937-949` |
| 8 | 绑定默认 `reportPeriodSeconds` | 60 | **300** | `config_store.c:1096` |
| 9 | 绑定默认 `offlineTimeoutSeconds` | 90 | **900** | `config_store.c:1097` |
| 10 | 绑定默认 `lowBatteryPercent` | 0（禁用） | **30** → ✅ 1.0.1 已改（用户拍板取 30，与甲方 Q3 的 0 不一致，待同步） | `config_store.c:1127` |
| 11 | 配置 JSON 字段 | `network.tls` 已删（⑨）、`ota.upgradePolicy` 已删（⑦） | 两个字段**仍在序列化输出** | `config_serialize.c` |
| 12 | `meta.configVersion` | uint16 | **uint32** → ✅ 1.0.1 已改 uint16 | `config_store.h:320` |
| 13 | `status.report` 字段数 | 12 | **13** → ✅ 1.0.1 补 `slot`/`currentStatus`；`bufferedEvents`（曾为第 14 个）已于 2026-11-12 按甲方要求移除 | `cloud_uplink.c:436` |
| 14 | `rsrp` 来源 | 真 RSRP（dBm，给出 -80/-95/-110 判读档） | **CSQ 换算的 RSSI** | `app_uplink.c:54/113` |
| 15 | 下行报文上限 | 4160，超限**不回任何应答** | 收 `CFG_JSON_MAX+64 = 6208` | `app_downlink.c:46` |

补充两条不算"错"但要知道：
- `rule.mode` / `dev.ctrl` 会把 `rules.mode` **落 Flash 并把 configVersion +1**；
  文档只说 manual 不跨重启（上电 `app_boot.c:186` 已强制回 auto，行为对），
  但版本号会被无谓 +1，平台可能因此多触发一次配置比对。
- `{level}` 取整用 `level_mm / 10`（`level_eval.c:216`）：正数是向下取整没问题，
  **负值会向 0 取整**（-5mm → 0 而不是 -1）。等 distance 模式落地时要改成真正的 floor。

---

## 三、多做了 / 与文档相反（需要产品侧拍板）

| # | 项 | 文档 | 现状 |
|---|---|---|---|
| 16 | 断线期间的遥测 | §5：**遥测不缓存，断线期间永久丢失** | 实现了 `telemetry_cache`（128 条），重连后补报（`app_uplink.c:476/525`） |
| 17 | 事件缓存容量 | 512 条（但注明"以 RAM 测算为准，可能调整"） | 256 条（已注明是有意取舍，可接受） |

第 16 条建议明确：补报的遥测会带上原始 `ts`，平台若不按 `ts` 去重，会在重连瞬间
看到一批"迟到数据"，可能干扰曲线与统计。要么平台接受，要么把缓存关掉（一个宏）。

---

## 四、固件单方面做不到（要流程/设备侧配合）

1. **`fwVersion` 末位奇偶 = A/B 分区**：固件只上报编译期 `MAIN_FW_VERSION` 与实际槽位，
   而 `tools/build_ab.ps1` 只有 `-Version` 一个参数，A/B 两镜像同号。
   要让"偶=A、奇=B"成立，**构建流程必须给 B 镜像自动 +1**（如 A=1.0.0 / B=1.0.1），
   否则这条约定只在 A 区成立。
2. **LED 半屏 6 字 / 全屏 4 字 + 滚动**：设备侧（声光报警器）行为，网关只负责下发文本，
   不在本期固件范围。
3. **`type=2` 纯 LED 的 uid 编号空间**：需要设备侧先定前缀，固件才能放开绑定。
4. **`gwId` 改动后 `clientId` 同步**：文档写"应同步修改"。我方 `client_id` 是独立字段，
   改 `gwId` 不会自动跟随；目前依赖平台同时下发（否则 MQTT 会被踢，表现为反复重连）。

---

## 五、已经对齐、不用再查的部分

- **命令集**：20 条全部注册（`cloud_proto.c:377-401`，`sys.time_sync` 已移除，
  再发走未知命令回 1001）。
- **协议骨架**：`msgId` 4 条幂等重放、`ts` 原样回带、`code/msg/data` 信封、
  错误码 1001/1002/1003/1005/1006/1007/1008/1009/1010/1011/2001。
- **主题与 QoS**：`cmd / cmdresp / telemetry / event / status / lwt` 六条，
  **全部 QoS1**，`status` 与 `lwt` retained，遗嘱独立到 `rt/gw/{gwId}/lwt`。
- **配置域**：9 域齐全；JSON 存 Flash、A/B 双区、先写备份再写主区、上电回退、CRC32。
- **`cfg.set`**：`expectVersion` + 1002、`applied[]`/`rejected[]` 逐域事务、`effectiveMode`。
- **`cfg.query`**：分域查询、`username`/`password` 脱敏为 `*****`、应答过大回 1011。
- **绑定/解绑**：异步 `bound`/`bind_failed`/`unbound`/`unbind_failed`、
  窗口默认 60s、一窗一台、窗口超时上报 `bind_failed`、上限 1009。
- **`dev.ctrl`**：`uid`/`"all"`、`gear`、`ledText1/2`、`ttsText` 覆盖（**空串也是覆盖**）、
  `results[{uid,code}]`、进入 manual、`manualTimeoutSeconds` 到期回归。
- **`rule.set/query/mode`**：字段与默认值（flashMode/ledShowMode/ledColor/两段模板/TTS）
  与文档 3.5 出厂默认表逐项一致；切回 auto 会强制重评估。
- **水位算法**：`confirmCount` 防抖、`minChangeCm` 与 `eventMinIntervalSeconds` 事件去抖、
  告警取**最大值**、遥测 `effective.levelCm` 也取**最大值**（2026-11-04 由平均改最大）、`Resolved` 四态冻结、
  档位不变不重复下发。
- **fail-safe**：`keep`/`alarm`/`off` 三种（含 off 故障画面）、部分失联生成 `event.device`。
- **OTA**：`ota.start`（1006/2001/1010）、`ota.query`（无 `done`，READY→`installing`）、
  `ota.abort`（installing 后 1006）、每 10% 或 10s 一条 `ota.progress`、MD5 + 双槽。
- **上电行为**：不恢复最近采样（`lastLevelCm`/`batteryPercent`/`lastTs` 重启为空）、
  manual 回 auto、`factory_reset` 清空 sensors/devices 且 `configVersion=0`、`keepNetwork`。

---

## 六、落地顺序与实际结果

| 优先级 | 事项 | 理由 | 状态 |
|---|---|---|---|
| P0 | #1 distance 换算 | 现场一旦用 distance 传感器，水位全错，属功能错误 | ✅ 完成 |
| P0 | #2 `sen.set` 离线回 1004、#3 `meta` 回 1001、#6 拒 type=2 | 都是一二十行，但平台按文档写分支，不回会卡住交互 | ✅ 完成 |
| P1 | #4 离线判定改按 `offlineTimeoutSeconds`、#5 恢复 `minChangeCm` 触发 | 直接影响平台观感（离线迟到 / 突变迟报） | #5 ✅ / #4 ⏸ 按裁决改为请甲方修订约束 |
| P1 | #7~#10 默认值与容量 | 联调时平台对照文档会逐条比对，差异最好一次抹平 | ⏸ 已书面请甲方按固件实际值修订 |
| P2 | #11~#15 字段清理（删 tls/upgradePolicy、configVersion 收窄、status 去 currentStatus、rsrp 换真 RSRP、下行限 4160） | 属"契约洁癖"；rsrp 那条例外，建议提前确认模组是否支持 AT+CESQ | #12 ✅ / 其余已书面告知甲方 |
| 待拍板 | #16 遥测缓存去留、B 镜像版本号 +1 的构建规则 | 需要平台/产品确认，不是纯固件问题 | 遥测缓存已在函件 B6 说明口径；B 版本号按用户要求不再提 |

### 1.0.1 具体实现位置

| 条目 | 文件 | 做法 |
|---|---|---|
| #1 | `level_eval.c` `level_convert_mm()` + `data_cache.c` `data_cache_up_push()` | 在"所有传感器上报的唯一漏斗"里换算一次，快照表存的已是水深；`raw_mm` 单列一栏保证遥测/补报的 `rawMm` 仍是原始值。`{level}` 改向下取整（负值不再显示成 0）。`telemetry_cache` 每条 28→32 B（+512 B RAM） |
| #2 | `cloud_cmd_devices.c` `cloud_cmd_set_entry()` | 仅当命令携带需下发字段（`device_dirty`）时才判在线、才回 1004；只改 name/enabled 不拒绝（否则设备离线时平台连改名都做不了）。判定与失联事件同源：`sensor_offline_threshold_ms()` |
| #3 | `cloud_cmd_cfg.c` | `sections` 出现 `meta` 即**整条**回 1001，不落地任何域；混合报文（合法域 + meta）也整条拒，避免"先写后拒" |
| #5 | `app_uplink.c` `uplink_telemetry_ready()` | 恢复 `|Δ| >= minChangeCm` 提前触发，周期触发保留 |
| #6 | `cloud_cmd_devices.c` `dev.bind` | 仅接受 `type=1/3` |
| #12 | `config_store.h` `cfg_meta_t.config_version` | uint32 → uint16（`config_store_version()` 与 `cloud_status_t` 保持 uint32 承载，值域 0~65535）；配置在 Flash 里是 JSON 文本，改宽度不涉及存储迁移 |

