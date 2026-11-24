# U1 复审报告 · ZED-F9P 驱动修复验收

- 复审对象：`feat/u1-f9p` 工作树未提交改动（`git diff 84025fd` 共 35 个已跟踪文件，及未跟踪实现文件 `components/gnss/private_include/UbloxExt.hpp`、`components/gnss/src/UbloxExt.cpp`、`main/modules/gnss/RtcmTxFramer.hpp`）。
- 依据：`docs/tasks/U1-f9p-driver.md`、`docs/reports/U1-review.md`（`RU1-01..RU1-12`）、Architect 裁决、ESP-IDF/C++ 规则与 `builds/a2-*` / `builds/u1-baseline-84025fd` 实测产物。
- 只读代码复审，未改动任何源码/测试/工具文件。

---

## 1. 首审问题逐项核验（RU1-01..RU1-12）

| 编号 | 状态 | 证据（file:line） | 覆盖测试 |
| --- | --- | --- | --- |
| **RU1-01** | **FIXED** | `components/gnss/src/ControllerConfigure.cpp:31-35,109-118,142-148,171-184`（F9P 12 键静默步 NAK 直接 `advanceStep` 不重试不进 `kFailed`，超时经 300 ms `kCfgPhaseResyncAdvance` 推进）；`main/modules/gnss/GnssModuleFrames.cpp:172-186` 与 `GnssModule.cpp:309-312` + `components/gnss/src/UbloxExt.cpp:512-525`（NAK/超时各记一次 `FW_LOGW`）。 | `test/gnss/TestGnssUbloxReceiver.cpp:testF9pControllerSilenceNak`、`testF9pControllerSilenceTimeoutAndResync`、`testValsetTrackerDelayedAndDuplicatedReplies` |
| **RU1-02** | **FIXED** | 能力不可切换：`main/modules/gnss/GnssModuleProbe.cpp:333-341`（不置 `const_bit::kGps`，置 `kConstBitGpsRequired = 1U << 7U`）、`components/settings/src/Registry.cpp:515-519`、`main/app/SettingsBackend.cpp:194-196`、`main/modules/gnss/GnssModulePpp.cpp:119-120,127-128`。写前拒绝 `gps=0`：`components/link_core/services/PhoneCompatService.cpp:773-793,822`、`Registry.cpp:305-327,355`、`GnssModulePpp.cpp:121-124`、`components/gnss/src/PppSequence.cpp:118-121`、`UbloxExt.cpp:323-325`。旧 blob 重放强制 GPS 开并记一次 W 日志：`GnssModulePpp.cpp:173-182,287-290`（重放失败后的锁死边界见 `NRU1-02`）。VALSET 强制写 `GPS_ENA=1`、`GPS_L1CA_ENA=1`（及 `QZSS_ENA=1`、`QZSS_L1CA_ENA=1`）：`components/gnss/src/drivers/UbloxWire.hpp:104-111`、`UbloxExt.cpp:330-336`。 | `test/gnss/TestGnssUbloxReceiver.cpp:testSatMaskConstellationsAndCutoff`、`testF9pSatMaskStateMachineAndValsetKeys`；`test/settings/TestSettingsRegistry.cpp:testF9pCapsAndCutoffKey`；`test/services/TestPhoneCompat.cpp:testF9pSatMaskCapsAndSbasGate`；`tools/main_host_check/WebServerCheck.cpp:testJsonFramesAndDiff`；`tools/main_host_check/GnssResetCheck.cpp:testGnssF9pEndToEnd` |
| **RU1-03** | **FIXED** | `UbloxWire.hpp:65`（`kF9pSbasMinVersion = 113U`）；`UbloxExt.cpp:273-319`（`parseF9pHpgVersion` / `f9pSupportsSbas`，HPG 1.12 或未知版本返回 `false`）；`GnssModulePpp.cpp:84-92,133,181,259` 与 `UbloxExt.cpp:337-339`（不支持时省略 `CFG-SIGNAL-SBAS_ENA` 键）；能力掩码屏蔽：`GnssModuleProbe.cpp:337-339`、`PhoneCompatService.cpp:764,789`、`Registry.cpp:322-324`。 | `test/gnss/TestGnssUbloxReceiver.cpp:testSatMaskConstellationsAndCutoff`、`testF9pSatMaskStateMachineAndValsetKeys`；`test/settings/TestSettingsRegistry.cpp:testF9pCapsAndCutoffKey`；`test/services/TestPhoneCompat.cpp:testF9pSatMaskCapsAndSbasGate`；`tools/main_host_check/GnssResetCheck.cpp:testGnssF9pEndToEnd` |
| **RU1-04** | **FIXED** | `components/gnss/private_include/PppSequence.hpp:97`（`kF9pSignalSettleMs = 600U`）；`PppSequence.cpp:183-188,204-210,246-249`（ACK 后进入 600 ms 非阻塞 `Phase::kSettle`，期间保持 `active()` 并忽略多余帧）；`GnssModulePpp.cpp:80-81`（`canStartSeq()` 在稳定期拒新请求返回 `kBusy`）。正常路径（~50 ms ACK + 600 ms settle + ~50 ms NVS ≈ 700 ms，最慢 ACK 1000 + 600 = 1600 ms）位于 `main/modules/web/WebModule.cpp:40` `kSetTimeoutMs = 2000U` 预算内。 | `test/gnss/TestGnssUbloxReceiver.cpp:testF9pSatMaskStateMachineAndValsetKeys`；`tools/main_host_check/GnssResetCheck.cpp:testGnssF9pEndToEnd`（`verifyF9pSettleBusyAndRollback`） |
| **RU1-05** | **FIXED** | `UbloxExt.hpp:18,232-303`、`UbloxExt.cpp:474-546`（`ValsetTracker` 单在途键跟踪 + 超时后 300 ms resync，未匹配/重复应答判 `kUnmatched`）；`ControllerConfigure.cpp:19-22,121-125,144-155,171-184,325-329,346-359,372-385` 与 `Controller.cpp:202,208`（`kConfiguring`/`kOutput` NAK/超时后 300 ms 重同步丢弃迟到 ACK/NAK）；`PppSequence.hpp:98`、`PppSequence.cpp:173-178,211-217`（F9P 超时 300 ms `kResync`）；`GnssModulePpp.cpp:81`（resync 期拒启新序列）。基站维护步与 1–2 ms 时钟漂移等次要缺口见 `NRU1-04`。 | `test/gnss/TestGnssUbloxReceiver.cpp:testValsetTrackerDelayedAndDuplicatedReplies`、`testF9pSatMaskStateMachineAndValsetKeys`、`testF9pControllerSilenceTimeoutAndResync` |
| **RU1-06** | **FIXED** | 单帧原子 VALSET：`PppSequence.hpp:100`（`kF9pStepCount = 1U`）、`PppSequence.cpp:136-138`、`UbloxExt.cpp:321-344`（`buildSatTrackValset` 单帧打包 `MINELEV` + 全部 `CFG-SIGNAL` 键）。NVS 失败回滚：`GnssModulePpp.cpp:135,249-295`（记录 `sat_prev_cfg_`，`store::writeBlob != ESP_OK` 时尽最大努力重发旧快照并向调用方报告 `fw::Result::kIoError`）。 | `test/gnss/TestGnssUbloxReceiver.cpp:testSatMaskConstellationsAndCutoff`、`testF9pSatMaskStateMachineAndValsetKeys`；`tools/main_host_check/GnssResetCheck.cpp:testGnssF9pEndToEnd`（`verifyF9pSettleBusyAndRollback`） |
| **RU1-07** | **PARTIAL** | 已修：`main/modules/gnss/RtcmTxFramer.hpp` 实现 RTCM3 帧边界跟踪；`GnssModule.cpp:314-328,343-374,402-459` 在 `seqBusy`（ACK/settle/resync）期间持续排空 RTCM；`GnssModulePpp.cpp:201-203,215-217` 与 `GnssModuleProbe.cpp:551-553` 对 `ppp_seq_`/`sat_seq_`/`SEC-UNIQID` 严格门控帧边界。未修：`GnssModuleProbe.cpp:500` `applyWriteAction()` 丢弃 `ensureRtcmFrameBoundary()` 返回值，控制器写仍可能在 RTCM3 帧中部切入（见 `NRU1-03`）。 | `tools/main_host_check/GnssResetCheck.cpp:testGnssF9pEndToEnd`（`verifyF9pRtcmFramingAndDrain`，仅覆盖 `sat_seq_` 跨 512 B 块与超时排空；未测 `applyWriteAction` 半帧切入及 `RtcmTxFramer` 独立边界） |
| **RU1-08** | **PARTIAL** | 已修：`Registry.cpp:29,597-603,821-830,913-914,934-949` 注册 `gnss.cutoff`（0–90 整数，`kVerbGnssSetSatTrack` 1 字节补丁，`access::kStandardAll`），`gnss.sat_track` 维持 `kPhoneAndLocal`（`:904-906`），冻结文件零改动（`sha256sum -c docs/frozen.sha256` PASS）；`GnssModulePpp.cpp:44-50`、`main/web/ui/app.js:254-255`、`tools/web_mock/web_mock.py:170,202,957-965` 已接入。**阻塞缺陷**：`main/app/SettingsBackend.cpp:263-316` 的 `getSetting()` 未将 `kKeyIdGnssCutoff` 分派到 `getExtendedSetting()`（`:249-258` 为死代码），实机 `config` 帧必缺 `gnss.cutoff`、`app.js:837` 隐藏该控件且 `config get gnss.cutoff` 失败（见 `NRU1-01`）。 | `test/settings/TestSettingsRegistry.cpp:testRegistryShape`、`testF9pCapsAndCutoffKey`；`test/settings/TestSettingsService.cpp:testGetDomainAndForEach`、`testPhoneCallerPermissionsAndEncoding`；`tools/main_host_check/WebServerCheck.cpp:testJsonFramesAndDiff`（均使用 mock `get`）；`GnssResetCheck.cpp:testGnssF9pEndToEnd`（直发 `kApplySatTrack`）。`WebModuleCheck.cpp` 未覆盖真实 `SettingsBackend` |
| **RU1-09** | **NOT_FIXED** | Architect 裁决仅需在报告中记录偏离（12 键静默列表不含 `VLW`/`RLM`，仅保证关闭出厂默认计划外 NMEA）；但 `docs/reports/U1-f9p-driver.md` 未作任何更新，未出现 `VLW`/`RLM` 偏差说明（见 `NRU1-06`）。 | no test（纯文档偏离项） |
| **RU1-10** | **FIXED** | `UbloxExt.hpp:21-120` 与 `UbloxExt.cpp:122-209` 抽离唯一共享的 `ValsetWriter`、`stepFletcher`、`encodeUbxFrame`；`components/gnss/src/drivers/UbloxUbx.cpp:80-174,289-404` 全部委托共享实现，旧重复 `PayloadWriter`/`beginValset`/`addU*`/`stepFletcher` 已删除。 | `test/gnss/TestGnssUbloxReceiver.cpp:testValsetByteExact`、`testNmeaSilenceFrame`、`testSatMaskConstellationsAndCutoff`、`testWorkModeSteps`；`test/gnss/golden/PlanCheck.hpp`（`test_gnss_ublox_golden`） |
| **RU1-11** | **NOT_FIXED** | `docs/reports/U1-f9p-driver.md` 未更新：`:67` 仍误称 P2 静默步走 `RAM|BBR`（实为 `RAM`）；`:40,195,199` 未区分手机 `0x06` 线格式（12 B/≥9 B）与内部 6 B NVS 布局；`:209` HIL 步骤未说明默认 `cfg.gsv=true` 需先关闭 GSV 才预期零 GSV；无修复节（见 `NRU1-06`）。注：`TestGnssUbloxReceiver.cpp:testF9pControllerSilenceNak` / `testF9pControllerSilenceTimeoutAndResync` 已补齐 GSV-off/on 两组主机覆盖。 | `test/gnss/TestGnssUbloxReceiver.cpp:testF9pControllerSilenceNak`、`testF9pControllerSilenceTimeoutAndResync`（仅测 GSV 计划；报告本身未修正） |
| **RU1-12** | **PARTIAL** | `builds/u1-baseline-84025fd/rtk_main.{elf,bin}` 已按 `84025fd` 干净重建并验证（`bin = 1,516,736 B`，`nm -C` 确认不含任何 U1 符号）；但 `docs/reports/U1-f9p-driver.md:136-147` 仍保留过时的首版差值（`+3,632 B` bin / `+112 B` DRAM，当前实测为 `+7,088 B` bin / `+144 B` DRAM）且仍缺 `kIo` 栈余量分析（见 `NRU1-05`、`NRU1-06`）。 | 产物核验：`builds/u1-baseline-84025fd/rtk_main.elf` vs `builds/a2-idf/rtk_main.elf`（no test） |

---

## 2. 修复引入的新问题（New Findings）

### NRU1-01 · blocker · `SettingsBackend::getSetting` 漏路由 `gnss.cutoff`，实机网页截止角控件被隐藏且不可读

- **位置**：`main/app/SettingsBackend.cpp:249-258`、`main/app/SettingsBackend.cpp:263-316`、`components/settings/src/Registry.cpp:29,913-914`、`main/modules/web/WebLogic.cpp:291-295`、`main/web/ui/app.js:254-255,437,836-838`、`main/modules/storage/ConfigCmds.cpp:200`。
- **具体故障场景**：
  1. `Registry.cpp:29` 为 `gnss.cutoff` 分配扩展枚举值 `kKeyIdGnssCutoff = static_cast<KeyId>(static_cast<uint8_t>(KeyId::kCount) + 1U)`（值 `21`）。
  2. `SettingsBackend.cpp:249-258` 在 `getExtendedSetting()` 中实现了 `gnss.cutoff` 的快照读取，但 `getSetting()`（`:263-316`）的 `switch (key)` 仅将 `kGnssPpp`/`kGnssSatTrack`/`kPhoneHeight`/`kPhoneSnPn` 交给 `getExtendedSetting()`；`kKeyIdGnssCutoff` 落入 `default: break;`（`:302-304`），随后被 `if (key < KeyId::kGnssOutGga || key > KeyId::kGnssOutRmc) { return false; }`（`:309-311`）直接拒绝，导致 `:249-258` 成为不可达死代码。
  3. 实机浏览器连接 Wi-Fi 配置页后：`hello.caps` 包含 `"gnss.cutoff":{"min":0,"max":90}`，因此 `W.heading`（`app.js:437`）显示 `"Elevation mask"` 标题；紧接着 `buildConfigFrame()`（`WebLogic.cpp:293`）调用 `service.get(spec.id, value)` 返回 `false`，`config` 帧完全省略 `"gnss.cutoff"`；`app.js:837` 执行 `x.row.hidden = !has(caps, x.f.key) || (configured && v === undefined)` 将 stepper 整行隐藏。用户只看到空标题，无法在网页读取或修改截止角；同时控制台 `config get gnss.cutoff` 必报 `unreadable key: gnss.cutoff`。
  4. 所有新增设置/网页单测（`TestSettingsService.cpp`、`WebServerCheck.cpp:234-246`）均使用 mock `get` 回调，未通过链接真实 `SettingsBackend.cpp` 的 `WebModuleCheck.cpp` 回归，因此漏测。
- **修复建议**：在 `SettingsBackend.cpp:309-311` 对非 `gnss.out.*` 区间转调 `getExtendedSetting(data, key, out)`，并在 `tools/main_host_check/WebModuleCheck.cpp` 增加经真实 `bindSettings` 的 `gnss.cutoff` `hello`/`config`/`set` 端到端测试。

### NRU1-02 · major · 遗留 `gps=0` NVS blob 在重放失败或未完成时会永久锁死 F9P 网页端 `gnss.cutoff` 与 `gnss.const.*` 修改

- **位置**：`main/modules/gnss/GnssModulePpp.cpp:38-50,114-125,171-183,287-290`。
- **具体故障场景**：
  1. 若设备 NVS 中保留旧接收机（如 UM980）写下的 `gps=0` 卫星配置 blob，换装 F9P 冷启动时 `startPendingReplay()`（`:173-182`）会强制 `gps=1` 重放；但 `finishSatTrack()`（`:287-290`）仅在 `fw::isOk(status)` 时把 `gps=1` 回写到 `ctx().data.gnssSatTrack` RAM 槽。
  2. 一旦开机重放超时/NAK（或用户网页请求在重放完成前的窗口到达），`gnssSatTrack` 仍为 `gps=0`。此后用户在网页修改 `gnss.cutoff`（1 字节补丁）或 `gnss.const.{bds,glo,gal}`（2 字节补丁）时，`cmdApplySatTrack()`（`:116-124`）以 `current`（`gps=0`）为基准解码，得到 `decoded->gps == 0`，命中 `(!f9pGpsPatch && f9pGpsOff)` 直接返回 `fw::Result::kInvalidArg`（网页报 `failed`）。
  3. 由于 F9P 网页已隐藏 `gnss.const.gps`，用户无法在网页打开 GPS，网页卫星与截止角配置被永久锁死（除非手机 `0x06` 下发全量 6 字节覆盖）。另 `:287-290` 在重放失败分支未复位 `sat_replay_forced_gps_`。
- **修复建议**：仅对 6 字节全量请求（`cmd.argLen == core::kSatConfigBlobBytes`）拒绝 `decoded->gps == 0`；对 F9P 的 1 字节 `gnss.cutoff` 与 2 字节非 GPS 星座补丁，若 `decoded->gps == 0` 则强制归一化为 `1U`（并记一次 `FW_LOGW`），同时在 `finishSatTrack()` 失败路径清零 `sat_replay_forced_gps_`。

### NRU1-03 · major · `GnssModule::applyWriteAction()` 丢弃 `ensureRtcmFrameBoundary()` 返回值，控制器写操作仍可在 RTCM3 帧中部切入

- **位置**：`main/modules/gnss/GnssModuleProbe.cpp:497-503`、`main/modules/gnss/GnssModule.cpp:343-374,409-411`。
- **具体故障场景**：
  1. `pumpRtcmPipe()`（`GnssModule.cpp:409-411`）在 `kRunning`、`kConfiguring`、`kProbing`、`kMaintenance(kSave)` 下均允许持续下发 RTCM3。
  2. 当手机/NTRIP 推送的一帧 RTCM3 被 SPP/网络拆包（例如前半帧已写入 UART、`rtcm_framer_` 位于 `kBody`，后半帧尚未写入 `rtcmPipe`，或 UART TX 环满返回 `0`），此时 `pollController()` 产生 `Action::Kind::kWrite`（如配置步骤重试、`kMaintenance(kSave)` 或探测帧）进入 `applyWriteAction()`（`GnssModuleProbe.cpp:497`）。
  3. `:500` 调用 `static_cast<void>(ensureRtcmFrameBoundary(now));`：因帧尾未到，`ensureRtcmFrameBoundary()` 仅启动 200 ms stall 计时并返回 **`false`**，而 `applyWriteAction()` 忽略返回值直接执行 `uart_write_bytes()`，将 UBX/ASCII 字节插进未完 RTCM3 帧中部，且未复位 `rtcm_framer_`。该 RTCM3 帧 CRC24Q 失效，F9P 还会把 UBX 帧头吞作 RTCM 载荷。
- **修复建议**：不要在已消费控制器状态转移的 `applyWriteAction()` 内强行写串口；在 `GnssModule::poll()` 调 `pollController()` 之前对允许 RTCM 下发且不在帧边界的情形先调用 `ensureRtcmFrameBoundary(nowMs)` 门控（或仅当控制器无待发写时放行，待帧尾到达或 200 ms stall 超时后再推进 `pollController()`）。

### NRU1-04 · minor · `ControllerMaintenance::handleModeAck` 与 `ValsetTracker` 存在三处次要状态同步缺口

- **位置**：`components/gnss/src/ControllerMaintenance.cpp:216-247`、`main/modules/gnss/GnssModuleProbe.cpp:499-501`、`main/modules/gnss/GnssModulePpp.cpp:151-158,222-225`。
- **具体故障场景**：
  1. **工作模式 VALSET 重试无重同步窗**：F9P 基站/模式切换（`modeStep` 发 `UBX-CFG-VALSET`）超时时，`ControllerMaintenance.cpp:225-230` 立即同轮重发、不经过 300 ms resync，迟到的首次 ACK 仍会误推进下一步，且 `applyWriteAction` 会立刻覆盖 `valset_tracker_` 刚进入的 resync 态（基站模式在 U1 §2 范围外，故降级为 minor）。
  2. **1–2 ms 时钟漂移吞掉超时告警**：`applyWriteAction()`（`GnssModuleProbe.cpp:499-501`）重新采样 `rtos::nowMs()` 给 `ValsetTracker` 定时，而非复用 `poll(nowMs)` 传给 `Controller` 的 `nowMs`。若 `ensureRtcmFrameBoundary()` 耗时跨 1–2 个 tick，在 `Controller` 超时切入 resync 后的 1–2 ms 窗内到达的迟到 ACK 会被 `ValsetTracker` 误判 `kAck` 并静默清掉在途标志，丢失 `<ubx> VALSET timeout` 日志。
  3. **UART 写失败后 `ValsetTracker` 残留 `in_flight_`**：`writeSeqTx()`（`GnssModulePpp.cpp:151-158`）在 `uart_write_bytes()` 之前调用 `recordValsetTx()`；若 UART 写失败（返回 `< tx.size()`），`sat_seq_` 已立即清空并结单 `kIoError`（`:222-225`），但 `valset_tracker_` 仍挂着 `in_flight_ = true` 1000 ms，1 s 后误报 `<ubx> VALSET timeout` 并进入 300 ms resync 阻塞 `canStartSeq()`。

### NRU1-05 · minor · `kIo` 接收路径在 UBX NAK 日志分支栈帧增加，4096 B 栈余量尚未经 HIL 实测

- **位置**：`main/modules/gnss/GnssModuleFrames.cpp:172-197`、`main/modules/gnss/GnssModuleProbe.cpp:497-537`、`main/AppContext.hpp:83`。
- **具体故障场景**：
  - 反汇编 `builds/a2-idf/rtk_main.elf` 实测单帧栈开销：`GnssModule::wait` = `576 B`（`0x240`）、`handleFrame` = `112 B`、`handleUbxFrame` = `80 B`、`logValsetIssue` = `256 B`（含 192 B `keysText`），接收路径自有栈帧合计 `1,024 B`（较首审 `handleUbxFrame` 单帧 256 B 净增 `80 B`），再叠加 `Demux::feed` 与 ESP-IDF `esp_log_write/vprintf`。`applyWriteAction` 单帧亦达 `304 B`（`0x130`）。在 `kIoStackBytes = 4096 B` 下建议缩减 `keysText` 或在 HIL 实测记录 NAK 突发下的 `uxTaskGetStackHighWaterMark`。

### NRU1-06 · minor · 实施报告 `docs/reports/U1-f9p-driver.md` 停留在首审前，多处数字与描述失实

- **位置**：`docs/reports/U1-f9p-driver.md:40,67,136-147,195-210`。
- **具体故障场景**：
  - 缺少首审修复节；未按 RU1-09 记录 12 键静默列表不含 `VLW`/`RLM` 的偏离；`:67` 仍写 P2 静默步走 `RAM|BBR`（实为 `RAM`）；`:40,195,199` 未区分手机 `0x06` 线格式（12 B/≥9 B）与内部 6 B NVS 格式；`:209` HIL 抓包无 GSV 步骤未说明默认 `cfg.gsv=true` 的前提；`:136-147` 仍列首审前的旧差值（`+3,632 B` bin / `+112 B` DRAM），与干净基线 `builds/u1-baseline-84025fd` 实测差值不符（见 §3）。

---

## 3. 构建、门禁与资源核验摘要（只读读取 `builds/`）

- **冻结文件校验**：`sha256sum -c docs/frozen.sha256` — **PASS**（全部未改动）。
- **网页资源预算（ADR-0017 §2.4 ≤ 24 KiB）**：`builds/a2-idf/esp-idf/main/index.html.gz` = **18,217 B** — **PASS**。
- **资源差值（`builds/u1-baseline-84025fd` vs `builds/a2-idf`）**：
  - `rtk_main.bin`：`1,516,736 B` → `1,523,824 B`（**+7,088 B** / +6.92 KiB；app 槽剩余 `0x6bf90` B，22%）。
  - `.flash.text`：`1,084,304 B` → `1,090,908 B`（**+6,604 B**）；`.flash.rodata`：`264,124 B` → `264,460 B`（**+336 B**）。
  - `.iram0.text`：`115,111 B` → `115,111 B`（**0 B**）。
  - 静态 DRAM：`.dram0.data` `51,739 B` → `51,883 B`（**+144 B**），`.dram0.bss` `44,744 B` → `44,744 B`（**0 B**），合计 **+144 B**。
- **`builds/a2-logs/` 门禁现状**：`G1-frozen`、`G2-{gnss,gnss-all,settings,services,main_host_check,t13_host_check,...}`、`G2b-{gnss,gnss-all,settings,services,main_host_check,...}-asan`、`G3-idf*`（含 trace-off/wifi-off/ota-off/production/console-none/none-wifi/rtk_pure-esp32/esp32c3）、`G5-format`、`G5b-tidy`、`G6-doc_check`（121 函数 0 不合格）、`G7b-runtime`（堆放行 0）、`G7c-no-cli-deps` 均 **PASS**。仅 `G2b-t13_host_check-asan` 中与 U1 无关的 `t13_disconnect_reuse_across_slots` 偶发等待 `SPP_SERVER_EVT_OPEN` 1000 ms 超时（常规 `G2-t13_host_check` 45/45 PASS）。

---

## 4. 需 Architect 在解冻/文档窗口完成的冻结协议与 ADR 更新清单（RU1-08 / RU1-11）

1. **`components/settings/include/settings/Types.hpp`**（**冻结文件**，需同步重算 `docs/frozen.sha256`；对应 RU1-08）：
   - 在 `enum class KeyId` 的 `kCount` 前正式加入 `kGnssCutoff`，移除 `Registry.cpp:29` 的 `kKeyIdGnssCutoff = KeyId::kCount + 1` 临时变通，恢复 `kRegistry.size() == KeyId::kCount` 与下标一一对应不变式。
   - 在 `namespace settings::const_bit` 中正式定义 `kGpsRequired = 1U << 7U`，消除分散魔数，并与 `PhoneCompatService.hpp:24` 的 `kSatCapGpsRequired` 以 `static_assert` 绑定。
2. **`docs/web/ws-protocol.md`**（Architect 维护协议文档；对应 RU1-08）：
   - §4 可改键表新增 `gnss.cutoff`（`0`–`90` 十进制整数，`caps` 形态 `{"min":0,"max":90}`，走 `kVerbGnssSetSatTrack` 1 字节补丁，持久化 NVS）。
   - §4 更新 `gnss.const.{gps,bds,glo,gal}` 备注为已接通 UM98x/CM680/ZED-F9P（2 字节补丁，持久化 NVS），并注明 ZED-F9P 因 GPS 必开限制在 `caps` 与 `config` 中省略 `gnss.const.gps`。
3. **`docs/protocol/phone_protocol.md`**（**冻结文件**，需同步重算 `docs/frozen.sha256`；对应 RU1-11）：
   - §4 `0x06 卫星追踪`（`:51`）：将“12 B 全 0 / 回 `01 01`（占位）”更新为真实协议——12 B 查询应答（`@1` 截止角 0–90、`@4..8` GPS/GLO/BDS/GAL/SBAS 按能力掩码，F9P 强制 `@4=1`、HPG 1.12 屏蔽 `@8=0`），≥9 B 设置请求走能力校验与延迟应答（成功落 NVS 回 `01 01`，失败/非法回 `02 02`）。
   - §4 `0x01 型号版本`（`:46`）：更新备注，说明 `@56` 板卡型号、`@64` 板卡 SN（F9P 由 `UBX-SEC-UNIQID` 填 10 位大写 HEX，读不到留空）、`@80` 板卡固件已真实填充，不再是“预留占位”。
4. **`docs/adr/ADR-0017-wifi-web-config.md`**（对应 RU1-08）：
   - §2.6 页面内容表 GNSS 可改项增补“截止仰角（0–90°）”并更新星座勾选已支持 F9P/UM98x/CM680。
5. **`docs/adr/ADR-0018-settings-service.md`**（对应 RU1-08）：
   - §2.3/§2.4 增补 `gnss.cutoff` 键以及 `GnssCaps::constMask` 的 `kGpsRequired`（不可关闭星座隐藏网页开关且拒绝置 0）语义。
6. **`docs/adr/ADR-0012-gnss-receiver-drivers.md`**（及冻结头 `components/gnss/include/gnss/Receiver.hpp`，对应 RU1-11 与报告 §7.1）：
   - 决定是否在解冻窗将 `boardSn` 纳入 `ProbeResult` 或正式记录 `AppData::gnssBoardSn` 旁路约定；同步记录 F9P 静默步非致命降级、300 ms VALSET 重同步窗与 600 ms 信号稳定期约定。

---

## 5. 最终结论

**REJECT**

- 阻塞原因：存在 1 项 **blocker** 新缺陷（`NRU1-01`：`SettingsBackend.cpp:263-316` 漏分派 `gnss.cutoff` 导致实机网页截止角控件被隐藏、`config get gnss.cutoff` 必败），以及 2 项 **major** 新缺陷（`NRU1-02` 旧 `gps=0` blob 在重放失败后永久锁死网页卫星/截止角配置；`NRU1-03` `applyWriteAction()` 丢弃 `ensureRtcmFrameBoundary()` 返回值可能切碎 RTCM3 帧）；同时 `docs/reports/U1-f9p-driver.md` 未补齐修复记录、RU1-09 偏离、RU1-11 勘误与 `84025fd` 新差值（`NRU1-06`）。
- 放行条件：修复 `NRU1-01`、`NRU1-02`、`NRU1-03`（建议一并收口 `NRU1-04`），在 `WebModuleCheck.cpp` 补充真实 `SettingsBackend` 的 `gnss.cutoff` 回归测试并补测 `applyWriteAction`/遗留 `gps=0` 失败恢复路径，同步更新 `docs/reports/U1-f9p-driver.md` 后复验。
