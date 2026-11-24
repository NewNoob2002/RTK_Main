# U1：u-blox ZED-F9P 驱动补全报告

- 任务卡：`docs/tasks/U1-f9p-driver.md`（范围 §1、§3；§2 基站模式不在本次范围）
- 分支 / 基线：`feat/u1-f9p`（基线 `main` `84025fd`，全量 U1 + 两轮评审修复均为未提交工作树改动）
- 依据：ADR-0010（GNSS）、ADR-0012（接收机驱动）、ADR-0017（Wi-Fi 网页配置）、ADR-0018（设置服务）、`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/reports/U1-review.md`、`docs/reports/U1-rereview.md`
- 参考资料（只读）：
  - SparkFun RTK Everywhere：`../../SparkFun_RTK_Everywhere_Firmware/Firmware/RTK_Everywhere/GNSS_ZED.h`、`GNSS_ZED.ino`
  - 旧 P2 固件：`../../P2_V3.1_IDF/main/main.c`、`../../P2_V3.1_IDF/main/user_src/user_f9p.c`
  - u-blox ZED-F9P Interface Description：`UBX-22008968`（HPG 1.32）

---

## 1. 改动文件

共改动 37 个既有文件、新增 3 个实现文件（不含 `docs/reports/` 报告文件）；冻结文件零改动（`sha256sum -c docs/frozen.sha256` PASS）。

### 1.1 `components/gnss/`

| 文件 | 变更 | 说明 |
|---|---|---|
| `components/gnss/CMakeLists.txt` | 修改 | 将 `src/UbloxExt.cpp` 加入 `_gnss_srcs` |
| `components/gnss/src/drivers/UbloxWire.hpp` | 修改 | 修正 `kValsetHeadBytes = 4U`、修正 `GSA`/`GSV` UART1 键 ID；补全 12 条 HPG 1.12+ 标准 NMEA UART1 静默键表、`CFG-NAVSPG-INFIL_MINELEV`、`CFG-SIGNAL-*_ENA`（含 `GPS_L1CA`/`QZSS_L1CA`）、`kF9pSbasMinVersion = 113U` 与 `UBX-SEC-UNIQID` 常量 |
| `components/gnss/private_include/UbloxExt.hpp` | 新增 | 纯编码/解析头（无堆、ISR 安全）：唯一共享 `ValsetWriter` / `stepFletcher` / `encodeUbxFrame`、NMEA 静默 VALSET、原子单帧截止角+星座 VALSET、HPG 版本与 SBAS 支持判定、`UBX-SEC-UNIQID` 轮询与解析、单在途 `ValsetTracker`（含 300 ms 重同步窗）与键格式化 |
| `components/gnss/src/UbloxExt.cpp` | 新增 | `UbloxExt.hpp` 实现 |
| `components/gnss/src/drivers/UbloxUbx.cpp` | 修改 | 删除重复编码器与 3 字节头旧实现，全部委托 `UbloxExt` 共享 `ValsetWriter` 与 `encodeUbxFrame` |
| `components/gnss/src/drivers/UbloxReceiver.cpp` | 修改 | 严格 `isZedF9pModel`（匹配 `ZED-F9P` / `ZED-F9P-*`，拒绝 `ZED-F9R`/`ZED-F9P2`）；`FWVER=` 回退 `swVersion`；`matchProbe` 校验 Fletcher 校验和；`roverStep` 增补 NMEA 静默步（configure → silence 12 NMEA → 可选改波特率） |
| `components/gnss/src/ControllerImpl.hpp` | 修改 | 声明 `isNonfatalSilenceStep`、`configResync`、`outputResync` 与 `pollMaintenanceAwaitAck` |
| `components/gnss/src/ControllerConfigure.cpp` | 修改 | F9P 12 键静默步 NAK 非致命跳过、超时经 300 ms 重同步推进；`kConfiguring`/`kOutput` VALSET NAK/超时均经 300 ms 重同步丢弃迟到 ACK/NAK |
| `components/gnss/src/ControllerMaintenance.cpp` | 修改 | `kMode` 工作模式 VALSET 超时/NAK 重试增加 300 ms 重同步窗并在窗内丢弃迟到 ACK/NAK（复用 `phase_` 不触碰冻结 `Controller.hpp`） |
| `components/gnss/private_include/PppSequence.hpp` | 修改 | `SatMaskSequence::start` 支持 `Model` 与 `includeSbas`，定义 F9P 原子单步（`kF9pStepCount = 1U`）、600 ms 稳定期（`kF9pSignalSettleMs`）与 300 ms 重同步窗 |
| `components/gnss/src/PppSequence.cpp` | 修改 | `supportsSatMask(Model::kZedF9p) = true`；F9P 单帧原子 VALSET + ACK 后非阻塞 600 ms 稳定期 + 超时 300 ms 重同步；拒绝 `gps=0`；UM98x 6 步 ASCII 逻辑保持不变 |

### 1.2 `components/link_core/`、`components/settings/` 与 `main/`

| 文件 | 变更 | 说明 |
|---|---|---|
| `components/link_core/private_include/PhoneCompatService.hpp` | 修改 | 定义 `kSatCapGpsRequired = 1U << 7U` 能力位（对应冻结头外约定） |
| `components/link_core/services/PhoneCompatService.cpp` | 修改 | 手机 `0x06` 查询按能力掩码裁剪（F9P 强制 GPS=1、HPG 1.12 屏蔽 SBAS=0），设置请求按能力校验并写前拒 `gps=0` |
| `components/settings/src/Registry.cpp` | 修改 | 接入 `gnss.const.{gps,bds,glo,gal}`（2 字节补丁）与 `gnss.cutoff`（`kKeyIdGnssCutoff` 扩展键，0–90 整数 1 字节补丁，持久化 NVS，`access::kStandardAll`）；F9P 屏蔽 `gnss.const.gps`、HPG 1.12 屏蔽 SBAS；`gnss.sat_track` 保持 `kPhoneAndLocal` |
| `main/app/SettingsBackend.cpp` | 修改 | `getConstSetting` 读取星座位、`getExtendedSetting` 读取 `gnss.cutoff`，并在 `getSetting` 中将非 `gnss.out.*` 扩展键路由至 `getExtendedSetting` |
| `main/modules/web/WebLogic.cpp` | 修改 | `buildConfigFrame` 将 `gnss.const.*` 编码为 `0`/`1`，`gnss.cutoff` 按整数下发 |
| `main/web/ui/app.js`、`tools/web_mock/web_mock.py` | 修改 | 前端与本地 mock 接入 `gnss.cutoff`（0–90° 截止仰角）与 F9P 星座能力裁剪 |
| `main/data/AppData.hpp` / `AppData.cpp` | 修改 | `#if RTK_GNSS_ENABLED` 下新增 `fw::Slot<std::array<char, 16U>> gnssBoardSn{}` 并绑定槽锁 |
| `main/modules/gnss/RtcmTxFramer.hpp` | 新增 | 纯字节 RTCM3 发送帧边界状态机（无堆、ISR 安全），保证 UBX/ASCII 写操作不切碎在途 RTCM3 帧 |
| `main/modules/gnss/GnssModule.hpp` / `.cpp` / `Cmds.cpp` / `Probe.cpp` / `Ppp.cpp` / `Frames.cpp` | 修改 | 单在途 `ValsetTracker` 跟踪与 64 B 紧凑键日志；`poll` 与 `pollController` 在 RTCM3 帧边界前门控控制器写（含 200 ms stall 兜底与半帧收尾防死锁）；`seqBusy` 期持续排空 RTCM；单一 `nowMs` 贯通动作；串口写失败清理 `ValsetTracker`；F9P `UBX-SEC-UNIQID` 轮询；旧 `gps=0` blob 重放/补丁归一化 `gps=1` 与 NVS 失败回滚 |
| `main/modules/bt_link/BtLinkModule.hpp` / `BtLinkGnss.cpp` | 修改 | 将 `ctx().data.gnssBoardSn` 同步至 `deviceInfo.boardSn` |

### 1.3 Host 测试

| 文件 | 变更 | 说明 |
|---|---|---|
| `test/gnss/CMakeLists.txt` | 修改 | 链接 `UbloxExt.cpp` 供 `test_gnss_ublox_receiver` 使用 |
| `test/gnss/golden/PlanCheck.hpp` | 修改 | 扩展 F9P 黄金用例：12 键 NMEA 静默帧校验、4 字节 VALSET 头、修正后的 GSA/GSV 键 ID |
| `test/gnss/TestGnssUbloxReceiver.cpp` | 修改 | 字节级 VALSET、NMEA 静默集合、原子单帧星座/截止角、HPG 1.12/1.32 SBAS 门控、600 ms 稳定期、300 ms 重同步、`ValsetTracker`、控制器静默 NAK/超时、GSV-off/on 计划、工作模式 VALSET 超时重同步、`MON-VER` 与 `SEC-UNIQID` |
| `test/gnss/TestGnssUnicoreReceiver.cpp` | 修改 | 验证 `SatMaskSequence` 在 UM98x 上保持原有 6 步与 NACK 立即中止行为无回退 |
| `test/services/TestPhoneCompat.cpp` | 修改 | 覆盖手机 `0x06` 在 F9P 下的能力裁剪、GPS 必开拒绝与 HPG 1.12 SBAS 门控 |
| `test/settings/TestSettingsRegistry.cpp` / `TestSettingsService.cpp` | 修改 | 覆盖 `gnss.const.*`、`gnss.cutoff` 注册、权限、F9P 能力隐藏与 NVS 持久化 |
| `tools/main_host_check/WebServerCheck.cpp` | 修改 | 覆盖网页配置 `gnss.const.*` / `gnss.cutoff` 序列化与 diff |
| `tools/main_host_check/WebModuleCheck.cpp` | 修改 | 覆盖真实 `SettingsBackend` + `rtos::Executor` + `WebModule` 下 `gnss.cutoff` 的 `get`/`hello`/`config`/`set` 端到端链路 |
| `tools/main_host_check/GnssOutputCheck.cpp` / `GnssResetCheck.cpp` | 修改 | F9P 端到端回归：HPG 1.12/1.32 能力、旧 `gps=0` blob 正常与失败重放后 1/2 字节补丁恢复、600 ms 稳定期 `kBusy`、NVS 失败回滚、跨 512 B 块与拆包 RTCM3 帧边界（含控制器写与 200 ms stall）、超时零溢出排空、`writeSeqTx` 写失败清 `ValsetTracker` |

---

## 2. 硬件串口核实、口配置策略与偏差说明

- **实板串口映射**：
  - 本仓板级配置 `main/board/BoardProfile.cpp:28`：`kP2V141` 使用 ESP32 `UART2`（RX=GPIO16、TX=GPIO17）连接接收机 `receiverPort = 1U`（即 **F9P UART1**）。
  - 旧 P2 固件 `../../P2_V3.1_IDF/main/main.c:147` 调用 `GNSS_init("COM1", 921600, 8, 'n', 1, GNSS_Rover)`；`../../P2_V3.1_IDF/main/user_src/user_f9p.c:290,305-307,346,354,416-418` 确认 `COM1` 映射到 `UBX_PORT_UART1`，由 ESP32 `UART_NUM_2` 通信。
- **NMEA 静默、配置层与输出计划**：
  - `roverStep` 第 1 步配置测量周期与目标端口协议（P2 上 `useConfiguredPort = true` 走 **`RAM` 层 `0x01`**；非 `useConfiguredPort` 回退路径走 `RAM|BBR` 层 `0x03`）：`CFG-RATE-MEAS`、`CFG-UART1INPROT-RTCM3X=1`、`CFG-UART1OUTPROT-NMEA=1`；
  - 第 2 步单帧 VALSET（P2 上 `useConfiguredPort = true` 走 **`RAM` 层 `0x01`**，非 `useConfiguredPort` 回退路径走 `RAM|BBR` 层 `0x03`，见 `UbloxReceiver.cpp:157`）将 UART1 上 12 条 HPG 1.12+ 标准 NMEA 消息置 0，显式关闭出厂默认的 `GLL`/`VTG`/`GSV` 等计划外语句；该步若遇固件差异回 NAK 则非致命跳过（超时则经 300 ms 重同步后推进），各记一次 `FW_LOGW`，不阻断启动；
  - 随后的 `OutputPlan` 阶段仅按 ADR-0010 打开计划内语句（`GGA`/`RMC`/`GSA`/`GST`/`GSV` 按当前配置与周期；固件默认配置 `cfg.gsv = true`）；
  - 截止角与星座（`buildSatTrackValset`）走 **`RAM|BBR` 层 `0x03`** 并由本机 NVS 持久化，断电冷启重放保持一致。
- **静默列表偏离说明（RU1-09）**：
  - 依 Architect 裁决，12 键静默表（`DTM`/`RMC`/`VTG`/`GNS`/`GGA`/`GSA`/`GSV`/`GRS`/`GST`/`GLL`/`ZDA`/`GBS`）**有意不包含 `VLW` 与 `RLM`**（ZED-F9P UART1 出厂默认关闭且早期 HPG 固件支持不一），仅保证关闭出厂默认计划外标准 NMEA；同时静默步为非致命降级步，不因单键不支持阻断初始化。
- **为何不动 USB / I2C / SPI 口**：
  - P2 v1.41 硬件仅通过 F9P UART1 连接 ESP32，I2C/SPI 未连 MCU，不存在串口带宽占用或解析干扰；
  - 保持 F9P USB 口出厂配置不变，便于现场直接用 u-center 接 USB 口抓包与诊断。

---

## 3. UBX 键 ID、取值与出处对照

全部 UBX 键 ID 与帧格式均已核对 `UBX-22008968`（ZED-F9P HPG 1.32 Interface Description）、SparkFun 参考实现及旧 P2 固件：

| 键 / 消息名 | 键 ID / Class-ID | 类型 | 取值 / 说明 | 本仓位置 | u-blox 文档 (`UBX-22008968`) | SparkFun 参考 | 旧 P2 参考 |
|---|---|---|---|---|---|---|---|
| `CFG-RATE-MEAS` | `0x30210001` | U2 | `1000 / rateHz` ms（1..20 Hz） | `UbloxWire.hpp` | §6.9.19 CFG-RATE | — | `user_f9p.c:440-449` |
| `CFG-UART1INPROT-RTCM3X` | `0x10730004` | L | `1`（开 RTCM3 输入） | `UbloxWire.hpp` | §6.9.33 CFG-UART1INPROT | `GNSS_ZED.ino:383` | `user_f9p.c:290` |
| `CFG-UART1OUTPROT-NMEA` | `0x10740002` | L | `1`（开 NMEA 输出） | `UbloxWire.hpp` | §6.9.34 CFG-UART1OUTPROT | `GNSS_ZED.ino:378` | `user_f9p.c:290` |
| `CFG-UART1OUTPROT-RTCM3X` | `0x10740004` | L | 基站模式预留键 | `UbloxWire.hpp` | §6.9.34 CFG-UART1OUTPROT | `GNSS_ZED.ino:379-380` | — |
| `CFG-UART2INPROT-RTCM3X` | `0x10750004` | L | `1`（`cmdConfigurePort(port=2)`） | `UbloxUbx.cpp` | §6.9.35 CFG-UART2INPROT | `GNSS_ZED.ino:2068` | — |
| `CFG-UART2OUTPROT-UBX/NMEA/RTCM3X` | `0x10760001/02/04` | L | 端口 2 协议开关 | `UbloxWire.hpp` / `UbloxUbx.cpp` | §6.9.36 CFG-UART2OUTPROT | — | — |
| `CFG-UART1/2-BAUDRATE` | `0x40520001` / `0x40530001` | U4 | `portBaud`（如 `921600`） | `UbloxWire.hpp` / `UbloxUbx.cpp` | §6.9.32 / §6.9.35 | `GNSS_ZED.ino:924,1965,1976` | `user_f9p.c:348-354` |
| `CFG-MSGOUT-NMEA_ID_*_UART1`（12 键） | `0x209100A7..0x209100DE` | U1 | `DTM`/`RMC`/`VTG`/`GNS`/`GGA`/`GSA`(`0xC0`)/`GSV`(`0xC5`)/`GRS`/`GST`/`GLL`/`ZDA`/`GBS` 静默置 0；计划语句按历元数开启（UART2=+1） | `UbloxWire.hpp` | §6.9.14 CFG-MSGOUT | `GNSS_ZED.h:161-190` | `user_f9p.c:414-460` |
| `CFG-NAVSPG-INFIL_MINELEV` | `0x201100A4` | I1 | `0`..`90` 度截止仰角 | `UbloxWire.hpp` | §6.9.15 CFG-NAVSPG | `GNSS_ZED.h` | — |
| `CFG-SIGNAL-GPS_ENA` / `GPS_L1CA_ENA` | `0x1031001F` / `0x10310001` | L | F9P 强制 `1`（防止 GPS 失锁/无效组合） | `UbloxWire.hpp` / `UbloxExt.cpp` | §6.9.22 CFG-SIGNAL | `GNSS_ZED.h:28` | — |
| `CFG-SIGNAL-SBAS_ENA` | `0x10310020` | L | `0`/`1`（仅 HPG ≥ 1.13 包含，HPG 1.12/未知省略） | `UbloxWire.hpp` / `UbloxExt.cpp` | §6.9.22 CFG-SIGNAL | `GNSS_ZED.h:32` | — |
| `CFG-SIGNAL-GAL_ENA` | `0x10310021` | L | `0`/`1`（`kConstGal`） | `UbloxWire.hpp` | §6.9.22 CFG-SIGNAL | `GNSS_ZED.h:30` | — |
| `CFG-SIGNAL-BDS_ENA` | `0x10310022` | L | `0`/`1`（`kConstBds`） | `UbloxWire.hpp` | §6.9.22 CFG-SIGNAL | `GNSS_ZED.h:29` | — |
| `CFG-SIGNAL-QZSS_ENA` / `QZSS_L1CA_ENA` | `0x10310024` / `0x10310012` | L | 随 GPS 强制 `1` | `UbloxWire.hpp` / `UbloxExt.cpp` | §6.9.22 CFG-SIGNAL | `GNSS_ZED.h:35` | — |
| `CFG-SIGNAL-GLO_ENA` | `0x10310025` | L | `0`/`1`（`kConstGlo`） | `UbloxWire.hpp` | §6.9.22 CFG-SIGNAL | `GNSS_ZED.h:31` | — |
| `UBX-CFG-VALSET` | `0x06 / 0x8A` | — | 4 字节头 `{version=0x00, layers, res0=0x00, res1=0x00}` + `(key:U4, val)...` | `UbloxWire.hpp` / `UbloxExt.cpp` | §3.10.26 UBX-CFG-VALSET | `GNSS_ZED.ino:451,2020` | — |
| `UBX-MON-VER` | `0x0A / 0x04` | — | 轮询 `B5 62 0A 04 00 00 0E 34`；响应 `swVersion[30]+hwVersion[10]+ext[30]*N` | `UbloxWire.hpp` / `UbloxReceiver.cpp` | §3.14.13 UBX-MON-VER | `GNSS_ZED.ino:217` | `user_f9p.c:513,598-605` |
| `UBX-SEC-UNIQID` | `0x27 / 0x03` | — | 轮询 `B5 62 27 03 00 00 2A A5`；v1 载荷 9 B，偏移 4..8 为 5 B `uniqueId` → 10 位大写 HEX | `UbloxWire.hpp` / `UbloxExt.cpp` | §3.17.1 UBX-SEC-UNIQID | `GNSS_ZED.ino:219-222` | — |
| `UBX-ACK-ACK` / `NAK` | `0x05 / 0x01` & `0x00` | — | 载荷 2 字节 `{clsID, msgID}`，总长 10 字节 | `UbloxWire.hpp` / `UbloxExt.cpp` | §3.9.1–2 UBX-ACK | — | `user_f9p.c:349` |

---

## 4. SparkFun 与 u-blox 文档 / ADR 分歧说明

1. **配置层选择（RAM / RAM|BBR vs ALL）**：
   - SparkFun 在多处使用 `VAL_LAYER_ALL`（`0x07`，写内部 Flash）；本实现不擦写模块 Flash：P2 启动配置/静默/输出/模式走 `kLayerRam`（`0x01`），卫星追踪配置走 `kLayerRamBbr`（`0x03`）并由本机 NVS 持久化与冷启重放。
2. **卫星追踪单帧原子下发 vs 逐星座分帧**：
   - SparkFun 在 `setConstellations()` 中逐星座分条发 VALSET，每次触发射频重启。本实现通过 `buildSatTrackValset` 将 `MINELEV` 与全部 `CFG-SIGNAL-*_ENA` 键打包进**单条 `RAM|BBR` VALSET** 原子下发，并在 ACK 后非阻塞等待 **600 ms** 信号稳定期，期间拒绝并发新请求；若 NVS 落盘失败则尽最大努力回滚旧快照。
3. **GPS/QZSS 必开与 HPG 1.12 SBAS 门控**：
   - F9P 强制写 `GPS_ENA=1`、`GPS_L1CA_ENA=1`、`QZSS_ENA=1`、`QZSS_L1CA_ENA=1`，能力位隐藏 `gnss.const.gps` 并置 `kConstBitGpsRequired (1<<7)`，写前拒绝 6 字节全量请求中 `gps=0`；遗留 `gps=0` NVS blob 在冷启重放与 1/2 字节网页补丁中自动归一化为 `gps=1` 并记一行 `FW_LOGW`。
   - `CFG-SIGNAL-SBAS_ENA` 仅在解析出 HPG ≥ 1.13 时下发；HPG 1.12 或未知版本在能力掩码与 VALSET 帧中均省略，避免整帧 NAK。
4. **NMEA 静默目标端口与协议载荷区分**：
   - P2 v1.41 通过 **UART1** 连接 ESP32，仅静默 UART1 12 条标准 NMEA，不触碰 USB/I2C/SPI。
   - **手机 `0x06` 线格式与内部布局区分**：手机 `0x06` 线格式为查询应答 **12 B**（`@1` 截止角 0–90、`@4..8` GPS/GLO/BDS/GAL/SBAS）、设置请求 **≥9 B**（经 `PhoneCompatService` 校验并延迟回 `01 01`/`02 02`）；内部 NVS 存储与 `kVerbGnssSetSatTrack` 命令载荷为 **6 B** 全量布局 `{cutoffDeg, gps, glo, bds, gal, sbas}`、**2 B** 单星座补丁 `{constBit, 0/1}`（`gnss.const.*`）与 **1 B** 截止角补丁 `{deg}`（`gnss.cutoff`）。
5. **既有代码缺陷修正**：
   - 修正旧 `UbloxWire.hpp` 3 字节头（改为规范 4 字节头）与错误的 `GSA`/`GSV` UART1 键 ID（原误写为 I2C/SPI 键），并消除 `UbloxUbx.cpp` 与 `UbloxExt.cpp` 之间的重复编码器。

---

## 5. 评审修复

汇总首审（`RU1-01..RU1-12`）与复审（`NRU1-01..NRU1-06`）逐项修复证据与回归测试：

| 编号 | 级别 / 轮次 | 修复摘要与代码位置（`file:line`） | 回归测试 |
|---|---|---|---|
| **RU1-01** | 首审 | F9P 12 键静默步 NAK 非致命推进、超时经 300 ms `kCfgPhaseResyncAdvance` 推进并各记一次 `FW_LOGW`：`ControllerConfigure.cpp:31-35,109-118,142-148,171-184`、`GnssModuleFrames.cpp:172-186`、`GnssModule.cpp:309-312`、`UbloxExt.cpp:512-525` | `test/gnss/TestGnssUbloxReceiver.cpp:testF9pControllerSilenceNak`、`testF9pControllerSilenceTimeoutAndResync`、`testValsetTrackerDelayedAndDuplicatedReplies` |
| **RU1-02** | 首审 | F9P GPS 必开：能力屏蔽 `kGps` 并置 `kConstBitGpsRequired (1<<7)`（`GnssModuleProbe.cpp:333-341`、`Registry.cpp:515-519`、`SettingsBackend.cpp:194-196`）；写前拒 `gps=0`（`PhoneCompatService.cpp:773-793,822`、`Registry.cpp:305-327,355`、`GnssModulePpp.cpp:119-128`、`PppSequence.cpp:118-121`、`UbloxExt.cpp:323-325`）；VALSET 强制 `GPS_ENA/L1CA/QZSS/QZSS_L1CA=1`（`UbloxWire.hpp:104-111`、`UbloxExt.cpp:330-336`） | `TestGnssUbloxReceiver.cpp:testSatMaskConstellationsAndCutoff`、`testF9pSatMaskStateMachineAndValsetKeys`；`TestSettingsRegistry.cpp:testF9pCapsAndCutoffKey`；`TestPhoneCompat.cpp:testF9pSatMaskCapsAndSbasGate`；`GnssResetCheck.cpp:testGnssF9pEndToEnd` |
| **RU1-03** | 首审 | HPG ≥ 1.13 SBAS 门控：`UbloxWire.hpp:65`、`UbloxExt.cpp:273-319,337-339`、`GnssModulePpp.cpp:84-92,140,192,271`、`GnssModuleProbe.cpp:337-339`、`PhoneCompatService.cpp:764,789`、`Registry.cpp:322-324` | `TestGnssUbloxReceiver.cpp:testSatMaskConstellationsAndCutoff`、`testF9pSatMaskStateMachineAndValsetKeys`；`TestSettingsRegistry.cpp:testF9pCapsAndCutoffKey`；`TestPhoneCompat.cpp:testF9pSatMaskCapsAndSbasGate`；`GnssResetCheck.cpp:testGnssF9pEndToEnd` |
| **RU1-04** | 首审 | F9P 卫星 VALSET ACK 后非阻塞 600 ms 稳定期（`kF9pSignalSettleMs = 600U`），稳定期内 `canStartSeq()` 拒连发回 `kBusy`：`PppSequence.hpp:97`、`PppSequence.cpp:183-188,204-210,246-249`、`GnssModulePpp.cpp:80-81` | `TestGnssUbloxReceiver.cpp:testF9pSatMaskStateMachineAndValsetKeys`；`GnssResetCheck.cpp:verifyF9pSettleBusyAndRollback` |
| **RU1-05** | 首审 | 单在途 `ValsetTracker` + `kConfiguring`/`kOutput`/`SatMaskSequence` 300 ms 重同步丢弃迟到/重复 ACK/NAK：`UbloxExt.hpp:18,232-303`、`UbloxExt.cpp:474-546`、`ControllerConfigure.cpp:19-22,121-125,144-155,171-184,325-329,346-359,372-385`、`PppSequence.hpp:98`、`PppSequence.cpp:173-178,211-217` | `TestGnssUbloxReceiver.cpp:testValsetTrackerDelayedAndDuplicatedReplies`、`testF9pSatMaskStateMachineAndValsetKeys`、`testF9pControllerSilenceTimeoutAndResync` |
| **RU1-06** | 首审 | 单帧原子 VALSET（`kF9pStepCount = 1U`，`buildSatTrackValset`）+ NVS 落盘失败回滚旧快照并报 `kIoError`：`PppSequence.hpp:100`、`PppSequence.cpp:136-138`、`UbloxExt.cpp:321-344`、`GnssModulePpp.cpp:142,261-304` | `TestGnssUbloxReceiver.cpp:testSatMaskConstellationsAndCutoff`、`testF9pSatMaskStateMachineAndValsetKeys`；`GnssResetCheck.cpp:verifyF9pSettleBusyAndRollback` |
| **RU1-07** | 首审 | `RtcmTxFramer.hpp` 跟踪 RTCM3 帧边界；`seqBusy` 期间持续排空 RTCM；序列与控制器写操作均门控帧边界（完整修复见 `NRU1-03`）：`RtcmTxFramer.hpp`、`GnssModule.cpp:314-374,405-470`、`GnssModulePpp.cpp:201-226`、`GnssModuleProbe.cpp:59-63,551-564` | `GnssResetCheck.cpp:verifyF9pRtcmFramingAndDrain`、`verifyF9pControllerWriteRtcmSplitFrame` |
| **RU1-08** | 首审 | 零改动冻结头前提下注册 `gnss.cutoff`（0–90 整数，`access::kStandardAll`，1 字节补丁）并接通网页与 mock，`gnss.sat_track` 保持 `kPhoneAndLocal`（真实后端读取补齐见 `NRU1-01`）：`Registry.cpp:29,597-603,821-830,913-914,934-949`、`SettingsBackend.cpp:249-258,309-311`、`WebLogic.cpp:291-295`、`app.js:254-255`、`web_mock.py:170,202,957-965` | `TestSettingsRegistry.cpp:testF9pCapsAndCutoffKey`；`TestSettingsService.cpp:testGetDomainAndForEach`；`WebServerCheck.cpp:testJsonFramesAndDiff`；`WebModuleCheck.cpp:testGnssCutoffRealBackend` |
| **RU1-09** | 首审 | 依 Architect 裁决记录 12 键静默列表不含 `VLW`/`RLM` 的偏离说明：本报告 §2 与 §8.1 | 文档偏离项（无代码改动） |
| **RU1-10** | 首审 | 抽离唯一共享 `ValsetWriter` / `stepFletcher` / `encodeUbxFrame`，删除 `UbloxUbx.cpp` 重复实现：`UbloxExt.hpp:21-120`、`UbloxExt.cpp:122-209`、`UbloxUbx.cpp:80-174,289-404` | `TestGnssUbloxReceiver.cpp:testValsetByteExact`、`testNmeaSilenceFrame`、`testWorkModeSteps`；`test_gnss_ublox_golden` |
| **RU1-11** | 首审 | 勘误报告中 P2 静默层（`RAM`）、手机 `0x06` 线格式（12 B/≥9 B）与内部 6 B NVS 布局区分、HIL GSV 默认开启前提，并补齐 GSV-off/on 两组主机覆盖：本报告 §2、§4、§9；`TestGnssUbloxReceiver.cpp:testF9pControllerSilenceNak` / `testF9pControllerSilenceTimeoutAndResync` | `TestGnssUbloxReceiver.cpp:testF9pControllerSilenceNak`、`testF9pControllerSilenceTimeoutAndResync` |
| **RU1-12** | 首审 | 基于干净 `84025fd` 基线产物（`builds/u1-baseline-84025fd`）重算 `builds/u2-idf` 差值并补充 `kIo` 实测栈帧分析：本报告 §6 | 产物核验（`builds/u1-baseline-84025fd` vs `builds/u2-idf`） |
| **NRU1-01** | **blocker**（复审） | `SettingsBackend::getSetting` 将非 `gnss.out.*` 区间分派至 `getExtendedSetting`，使 `gnss.cutoff` 在实机网页 `config` 帧与 `config get` 正常可读：`main/app/SettingsBackend.cpp:201-258,309-311` | `tools/main_host_check/WebModuleCheck.cpp:testGnssCutoffRealBackend`（真实 `SettingsBackend` + `rtos::Executor` + `WebModule`） |
| **NRU1-02** | **major**（复审） | 仅 6 字节全量 `kApplySatTrack` 拒绝 `gps=0`；F9P 的 1 字节 `gnss.cutoff` 与 2 字节非 GPS 星座补丁将遗留 `gps=0` 归一化为 `1` 并记一次 `FW_LOGW`；`finishSatTrack` 无论成败均清零 `sat_replay_forced_gps_`：`main/modules/gnss/GnssModulePpp.cpp:119-145,299-304` | `tools/main_host_check/GnssResetCheck.cpp:verifyF9pLegacyGps0FailedReplay` |
| **NRU1-03** | **major**（复审） | 移除 `applyWriteAction` 对 `ensureRtcmFrameBoundary()` 返回值的丢弃；在 `GnssModule::poll` 与 `pollController` 入口门控 `ensureRtcmFrameBoundary(nowMs)`，并在 `pumpRtcmPipe`（`classifyRtcmTxGate`）放行半帧收尾避免维护/输出态死锁，确保无配置写从 RTCM3 帧中部切入：`main/modules/gnss/GnssModule.cpp:54-68,314-332,437-447`、`main/modules/gnss/GnssModuleProbe.cpp:59-64,500-515` | `tools/main_host_check/GnssResetCheck.cpp:verifyF9pControllerWriteRtcmSplitFrame`（含拆包补齐与 200 ms stall 兜底） |
| **NRU1-04** | **minor**（复审） | 1) `ControllerMaintenance.cpp` 为 `kMode` VALSET 重试加入 300 ms 重同步窗并丢弃迟到 ACK/NAK（`ControllerImpl.hpp:260`、`ControllerMaintenance.cpp:12,47,88,101,148-149,198-200,208-230,245-252`）；2) `pollController`/`applyAction`/`applyWriteAction`/`updateControllerStatus`/`onControllerStateChanged`/`pollUbloxBoardSn` 全链复用单一 `nowMs`（`GnssModule.hpp:495,503,533,785,799`、`GnssModuleProbe.cpp:59-115,134-154,500-515,551-564,580-605`）；3) `writeSeqTx` 与 `applyWriteAction` 先写 UART 成功再记录 `ValsetTracker`，失败清零 `valset_tracker_.reset()`（`GnssModulePpp.cpp:155-171`、`GnssModuleProbe.cpp:500-515`） | `test/gnss/TestGnssUbloxReceiver.cpp:testF9pModeValsetRetryResync`；`tools/main_host_check/GnssResetCheck.cpp:verifyF9pSeqWriteFailClearsTracker` |
| **NRU1-05** | **minor**（复审） | 缩减 `logValsetIssue` 局部缓冲 `kKeysTextBytes` `192 B → 64 B`（同时打印总键数 `n=%lu`），`logValsetIssue` 单帧栈从 `256 B` 降至 `128 B`，`kIo` RX NAK 路径自有栈总计 `896 B`（详见 §6.2）：`main/modules/gnss/GnssModuleFrames.cpp:172-179` | ELF 反汇编实测 `builds/u2-idf/rtk_main.elf`（见 §6.2） |
| **NRU1-06** | **minor**（复审） | 全面重写本报告：新增 `## 评审修复` 覆盖 `RU1-01..12` 与 `NRU1-01..06`、补齐 RU1-09 偏离与 RU1-11 勘误、采用 `builds/u1-baseline-84025fd` 干净基线差值与 `u2` 门禁汇总 | 本报告 §1–§9 |

---

## 6. 资源占用差值与 `kIo` 栈帧实测

### 6.1 固件镜像与内存差值（干净基线 `builds/u1-baseline-84025fd` vs `builds/u2-idf`）

| 指标 | 干净基线 (`84025fd`) | 本次 (`u2-idf`) | 差值 | 备注 |
|---|---:|---:|---:|---|
| `rtk_main.bin` | 1,516,736 B | 1,524,336 B | **+7,600 B** (+7.42 KiB) | `0x1e0000` app 槽剩余 `441,744 B`（`0x6bd90 B`，22.5%） |
| Flash Code (`.text`) | 1,084,304 B | 1,091,356 B | **+7,052 B** | `UbloxExt.cpp`、F9P 状态机/重同步/帧边界与设置路由 |
| Flash Data (`.rodata` 等) | 264,396 B | 264,796 B | **+400 B** | `.rodata` `264,124 B → 264,524 B`（+400 B） |
| IRAM | 116,139 B | 116,139 B | **0 B** | `.iram0.text` `115,111 B`（0 B），88.61% 已用，剩余 14,933 B |
| 静态 DRAM | 96,483 B | 96,627 B | **+144 B** | `.dram0.data` `51,739 B → 51,883 B`（+144 B），`.dram0.bss` `44,744 B`（0 B）；77.56% 已用，剩余 27,953 B |
| 网页资源 `index.html.gz` | — | 18,217 B | — | ADR-0017 §2.4 预算 ≤ 24 KiB（PASS） |

### 6.2 `kIo` 栈帧实测（`builds/u2-idf/rtk_main.elf`，`kIoStackBytes = 4,096 B`）

通过 `xtensa-esp32-elf-objdump -d builds/u2-idf/rtk_main.elf` 检查各函数入口 `entry a1, N`：

| 函数 | 单帧栈开销 | 说明 |
|---|---:|---|
| `GnssModule::wait` | 576 B (`0x240`) | 含 `kUartReadChunkBytes` 接收缓冲 |
| `GnssModule::handleFrame` | 112 B (`0x70`) | 帧分发与日志标签 |
| `GnssModule::handleUbxFrame` | 80 B (`0x50`) | `ValsetTracker` 与 `SEC-UNIQID` 分发 |
| `GnssModule::logValsetIssue` | **128 B** (`0x80`) | `[[gnu::noinline]]` 独立冷路径；NRU1-05 将 `keysText` 从 192 B 缩至 64 B，单帧净减 128 B |
| `GnssModule::poll` | 64 B (`0x40`) | 主轮询入口 |
| `GnssModule::pollController` | 48 B (`0x30`) | 控制器动作循环 |
| `GnssModule::applyWriteAction` | 304 B (`0x130`) | 含 TX 日志格式化缓冲 |
| `GnssModule::ensureRtcmFrameBoundary` | 32 B (`0x20`) | 帧边界等待 |
| `GnssModule::pumpRtcmPipe` | 48 B (`0x30`) | RTCM 串口写入 |

- **RX UBX NAK 告警最深自有链**（`wait → handleFrame → handleUbxFrame → logValsetIssue`）：`576 + 112 + 80 + 128 =` **`896 B`**（较复审前 `1,024 B` 净降 **128 B**），在 `kIoStackBytes = 4,096 B` 内留出 **3,200 B** 余量供 `Demux::feed` 与 ESP-IDF `esp_log_write/vprintf` 使用。
- **TX 写命令最深自有链**（`poll → pollController → applyWriteAction`）：`64 + 48 + 304 =` **`416 B`**（`poll → pollController → ensureRtcmFrameBoundary → pumpRtcmPipe` 为 `192 B`），远低于 `4,096 B` 预算。
- 实板 HIL 仍建议在连续 NAK 场景下抽检一次 `uxTaskGetStackHighWaterMark`。

---

## 7. 门禁逐项结果（`u2`，基线 `84025fd`）

执行命令：
```bash
export IDF_PATH=$HOME/esp/esp-idf-v6.1
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
. "$IDF_PATH/export.sh"
ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix u2 --base 84025fd
```

- 总计：**42 passed, 1 failed**（仅 `G2b-t13_host_check-asan` 在全量并发负载下 44/45 通过，`t13_gap_pairing_window` 偶发超时，与 U1 无关；常规 `G2-t13_host_check` 45/45 PASS）。
- 按已知 ASan 负载抖动约定对 `G2b-t13_host_check-asan` 单跑复测一次：`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir builds/u2-t13_host_check-asan --output-on-failure` → **45/45 PASS（100%）**。

| 门禁 | 结果 | 关键信息（日志在 `builds/u2-logs/`） |
|---|---|---|
| `G1-frozen` | PASS | `docs/frozen.sha256` 全部一致 |
| `G2-host` / `G2b-host-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-app_fw` / `G2b-app_fw-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-services` / `G2b-services-asan` | PASS | 7/7 通过（含 `test_phone_compat` F9P 能力与 SBAS 门控） |
| `G2-stream_demux` / `G2b-stream_demux-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-bt_reconnect` / `G2b-bt_reconnect-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-cli` / `G2b-cli-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-gnss` / `G2b-gnss-asan` | PASS | 9/9 通过（`test_gnss_ublox_receiver` 24/24，含 `testF9pModeValsetRetryResync`） |
| `G2-nvs_store` / `G2b-nvs_store-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-web_proto` / `G2b-web_proto-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-settings` / `G2b-settings-asan` | PASS | 2/2 通过（含 `gnss.const.*` 与 `gnss.cutoff`） |
| `G2-ota` / `G2b-ota-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-main_host_check` / `G2b-main_host_check-asan` | PASS | 103/103 通过（含 `web_module_check` 真实 `SettingsBackend` `gnss.cutoff` 与 `gnss_output_check` F9P 全链路回归） |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | PASS（复测 PASS） | 非 ASan 45/45 PASS；ASan 全量跑 44/45（`t13_gap_pairing_window` 负载超时），单跑复测 **45/45 PASS** |
| `G2-gnss-all` / `G2b-gnss-all-asan` | PASS | 10/10 通过（含 ComNav/Unicore/u-blox 全部驱动） |
| `G5-format` | PASS | `clang-format --style=file` 0 差异 |
| `G6-doc_check` | PASS | 改动函数 132 个，不合格 0 个 |
| `G3-idf` | PASS | 默认配置 0 warning，`rtk_main.bin` 1,524,336 B |
| `G3-idf-trace-off` | PASS | `CONFIG_RTK_TRACE_ENABLE=n`，0 warning |
| `G3-idf-wifi-off` | PASS | `RTK_WIFI_CONFIG_ENABLE=n`，0 warning |
| `G3-idf-ota-off` | PASS | 0 warning |
| `G3-idf-production` | PASS | PRODUCTION 档，0 warning |
| `G3-idf-console-none` | PASS | NONE 控制台档，0 warning |
| `G3-idf-none-wifi` | PASS | NONE+Wi-Fi 档，0 warning |
| `G3b-rtk_pure-esp32` | PASS | 0 warning |
| `G3b-rtk_pure-esp32c3` | PASS | 0 warning |
| `G5b-tidy` | PASS | 检查 150 个文件，发现 0 条 |
| `G7b-runtime` | PASS | 139 目标文件，堆放行 0、未定义 `__cxa_guard` 0、异常白名单 6/6 |
| `G7c-no-cli-deps` | PASS | NONE+Wi-Fi 构建无 CLI/REPL 依赖 |

---

## 8. 偏离、冻结文件说明与未决问题

### 8.1 冻结文件零改动处理与协议偏差说明

本次严格保持冻结文件零改动（`docs/frozen.sha256`、`components/settings/include/settings/Types.hpp`、`docs/protocol/*`、`docs/web/ws-protocol.md`、`docs/adr/*` 均未编辑）：

- **`components/gnss/include/gnss/Receiver.hpp` / `Controller.hpp` 未改**：`UBX-SEC-UNIQID` 解析出的 10 位大写 HEX 板卡序列号经 `AppData::gnssBoardSn` 槽路由至 `BtLinkGnss.cpp` 填充 `deviceInfo.boardSn`；`ControllerMaintenance` 模式重同步子态复用空闲 `phase_` 字段，不改变冻结 `Controller` 布局。
- **`components/settings/include/settings/Types.hpp` 未改**：`gnss.const.*` 与 `gnss.cutoff` 复用 `kVerbGnssSetSatTrack`，按载荷长度（6 B 全量 / 2 B 单星座 / 1 B 截止角）无歧义区分；`gnss.cutoff` 暂用扩展枚举 `kKeyIdGnssCutoff = KeyId::kCount + 1`，`kConstBitGpsRequired = 1U << 7U` 暂置于非冻结实现层。
- **RU1-09 静默列表偏离**：12 键 NMEA UART1 静默表不包含 `VLW`、`RLM`（出厂默认关闭），仅保证关闭出厂默认计划外 NMEA 并容忍固件差异。

需 Architect 在后续解冻/文档窗口收口的冻结头与协议文档项（详见 `docs/reports/U1-rereview.md` §4）：
1. `components/settings/include/settings/Types.hpp`：正式加入 `KeyId::kGnssCutoff` 与 `const_bit::kGpsRequired` 并重算 `docs/frozen.sha256`。
2. `docs/web/ws-protocol.md`、`docs/adr/ADR-0017-wifi-web-config.md`、`docs/adr/ADR-0018-settings-service.md`：同步 `gnss.cutoff` 与 F9P 隐藏 `gnss.const.gps` 语义。
3. `docs/protocol/phone_protocol.md`、`docs/adr/ADR-0012-gnss-receiver-drivers.md`：更新 `0x06` 卫星追踪、`0x01` `boardSn` 实填状态以及 F9P 静默降级/300 ms 重同步/600 ms 稳定期约定。

### 8.2 未决问题

- **SBAS 独立网页键**：冻结 `Types.hpp` 仅含 `gnss.const.{gps,bds,glo,gal}` 四个星座键；SBAS 当前经手机 `0x06` / `gnss.sat_track` 6 字节配置控制（且 HPG 1.12 自动屏蔽）。
- **F9P 实板 HIL 验证待做**：执行者未烧录/未连串口，以下实板表现待 Architect 在 P2 v1.41 + ZED-F9P 样机上确认（含实际 HPG 固件版本与 `kIo` 高水位实测）。

---

## 9. Architect HIL 验证步骤（P2 v1.41 + ZED-F9P）

1. **冷启动探测、序列号与 NMEA 静默验证**：
   - 烧录 `builds/u2-idf` 固件到 P2 v1.41（贴 ZED-F9P）并冷启动；
   - 确认串口日志探测到 `ZED-F9P`、解析出固件版本（`HPG x.xx`）与 `boardSn`（`UBX-SEC-UNIQID` 10 位 HEX，若未返回则为空且无报错）、切速至目标波特率且无关键 VALSET NAK；
   - 注意默认配置 `cfg.gsv = true`（输出计划包含 `GSV`）：先通过控制台或网页将 `gnss.out.gsv` 设为 `0`，再连续抓取 60 s NMEA 串口输出，确认仅包含计划内语句（`GGA`/`RMC`/`GSA`/`GST`），无 `GLL`/`VTG`/`GSV` 等计划外语句。
2. **网页与蓝牙截止角/星座配置与持久化验证**：
   - 连接 Wi-Fi 配置页，确认 `gnss.cutoff`（Elevation mask）正常显示当前值（默认/已存值），且 F9P 下隐藏 `gnss.const.gps`；
   - 通过网页修改 `gnss.cutoff = 15` 与 `gnss.const.glo = 0`，并通过手机蓝牙 `0x06` 查询确认同步生效（同时验证 `0x06` 尝试置 `gps=0` 被拒回 `02 02`）；
   - 断电重启板卡后再次检查网页与蓝牙 `0x06`，确认 `15°` 与星座配置保持生效。
3. **RTCM 差分固定解与帧边界验证**：
   - 手机通过蓝牙向设备转发 RTCM3 差分流（连接可用基站/CORS），在持续 RTCM 流期间通过网页修改 `gnss.cutoff` 或定位频率；
   - 确认 F9P UART1 正常收敛并保持 RTK Fixed 固定解，无 RTCM 帧切碎、丢字节或管道溢出；如遇连续 VALSET NAK，检查 `kIo` 任务栈高水位余量。

## 10. 实板 HIL（Architect，2026-11-24）

板子：P2 v1.41 + ZED-F9P，ESP32-D0WD v1.0，`/dev/ttyACM1`，固件 `builds/u2-idf`（ce72ed9），室内（0 星）。

| 项 | 结果 | 证据 |
| --- | --- | --- |
| 探测 | PASS：`ZED-F9P`、`HPG 1.12`、boardSn `3F41CA671D`、115200→460800，`ctrl steps=7 nacks=0 to=0` | 开机日志、`gnss` |
| 计划外 NMEA | PASS：两次开机共约 2 min `ign=0`（无 GLL/VTG/TXT）；GSV 按计划 5000 ms 输出。接入差分后 `ign` 升到 4，未抓原文（疑为 TXT） | `gnss` 计数 |
| 截止角/星座（控制台） | PASS：4 次修改每次单条 52 B VALSET（`uart tx` 差值），无 NAK；重启后 cutoff/GLO 保持并重放一次 | `config get`、`uart tx` |
| 关 GPS | PASS：控制台 `ERR unsupported`；手机 0x06 关 GPS 后重新查询仍为开；网页 `set gnss.const.gps 0` → `unsupported` | 控制台/手机/WS |
| 手机 0x06 | PASS：查询显示 cutoff 15、GLO 开；关北斗后固件 `gnss.const.bds=0`，`uart tx − pipe written` 多 52 B | 用户 + 控制台 |
| 网页 | PASS：`caps["gnss.cutoff"]={min:0,max:90}`，`gnss.const.gps` 不在 caps 中；`set gnss.cutoff 12` ok 且 config 回推 12；`91` → `bad_value` | `wsprobe.mjs` |
| RTCM 透传 | PARTIAL：手机转发约 150 KB RTCM，`pipe drop=0`、`demux resyncs=0`，其间配置修改正常；室内无星，RTK Fixed 保持未验证 | `gnss` |
| F9P 输出周期/定位频率 | 既有缺口（非 U1 引入）：u-blox 驱动未实现 `navRatesHz()`/`outputPeriodsMs()`，`gnss.out.*`、`gnss.rate` 在 caps 中缺失，`config set gnss.out.gsv 0` → `unsupported` | `Receiver.cpp:13-22` |
