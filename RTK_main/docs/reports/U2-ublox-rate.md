# U2 实施报告：ZED-F9P 定位频率、语句输出周期与 NMEA 高精度

- **任务卡**：`docs/tasks/U2-ublox-rate.md`
- **调研依据**：`docs/reports/U2-ublox-gap.md`、ADR-0010、ADR-0012、ADR-0017、ADR-0018
- **分支 / 基线**：`feat/u2-ublox-rate`（基线 `main` `3bbd6d2`）
- **Profile 与规范**：`esp-idf`（`sdkconfig.defaults`、`idf_component_register`），C++20 T2，严格告警 + `-Werror`，零异常/RTTI，零新增堆分配。
- **目标硬件假设与来源**：ESP32（经典款 Xtensa LX6 双核，4 MB Flash，`AGENTS.md` §1）；P2 v1.41 + ZED-F9P `HPG 1.12`（UART1 接 ESP32 UART2 RX16/TX17，`docs/reports/U1-f9p-driver.md` §10）；四星座 RTK 频率上限 `8 Hz`（`ZED-F9P-01B Data sheet UBX-17051259-R09` §1.2 Table 1，HPG 1.32 为 `7 Hz`）；`CFG-NMEA-HIGHPREC` (`0x10930006`)、`CFG-NMEA-SVNUMBERING` (`0x20930007`) 见 `UBX-22008968-R01` §6.9.15 Table 28。

---

## 1. 改动清单

### 1.1 冻结文件变更（任务卡授权）

| 文件 | 变更说明 | 新 SHA-256 |
| --- | --- | --- |
| `components/gnss/include/gnss/drivers/UbloxReceiver.hpp` | 仅新增 `navRatesHz(Model)` 与 `outputPeriodsMs(OutputMsg, Model)` 两个 `override` 声明及注释 | `189edd2c2736ba57fe954cf457a0dbc0f6f3d0f58916275503cfaca09d55245c` |
| `docs/frozen.sha256` | 仅同步第 36 行 `UbloxReceiver.hpp` 哈希；其余 157 行未动，`sha256sum -c` 通过 | — |

### 1.2 非冻结生产与测试文件

| 文件 | 变更内容 |
| --- | --- |
| `components/gnss/src/drivers/UbloxWire.hpp` | 新增 `kKeyNmeaHighprec = 0x10930006U`、`kKeyNmeaSvNumbering = 0x20930007U`。 |
| `components/gnss/src/drivers/UbloxReceiver.cpp` | 实现 `navRatesHz()`（`{1,2,5}`）与 `outputPeriodsMs()`（GGA/GST/RMC `{0,200,500,1000}`、GSA `{0,1000}`、GSV `{0,1000,5000}`）；`caps()` 去除 F9P 的 `cap::kRate20Hz`；抽取 `computeMsgoutEpochs()` 使 `buildOutput()` 严格执行整倍导航历元校验（拒绝静默取整）。 |
| `components/gnss/src/drivers/UbloxUbx.cpp` | `cmdConfigurePort()` 追加 `CFG-NMEA-HIGHPREC=1`、`CFG-NMEA-SVNUMBERING=1`（载荷 20 B → 30 B）；保持 `cmdConfigureReceiver()` 与 `test/gnss/golden/GoldenUblox.inc` 零改动。 |
| `components/gnss/private_include/UbloxExt.hpp` / `components/gnss/src/UbloxExt.cpp` | 新增 `buildPortConfigFallbackValset()`（检测含高精度键被 NAK 的端口配置 VALSET，剥离两扩展键重建 3 键基础端口配置帧）与 `containsNmeaHighPrecKeys()`。 |
| `components/gnss/src/ControllerImpl.hpp` / `components/gnss/src/ControllerConfigure.cpp` | 新增 `kCfgPhaseFallbackSend` 阶段与 `configRetryOrFail()` 辅助：当 F9P 在 `kConfiguring` 对含高精度键的端口配置 VALSET 回 NAK 时，非致命降级重发 3 键基础端口配置，不进入 `kFailed`；若基础 3 键仍 NAK 则按原重试/失败逻辑处理，无死循环风险。 |
| `main/modules/gnss/GnssModule.hpp` / `GnssModuleProbe.cpp` | ① `publishCaps()` 经 `fillOutputPeriodDomain()` 对 F9P 执行 `isF9pPeriodValid()` 整倍历元过滤（5 Hz 下自动滤除 `500 ms`）；② `makePlan()`、`restoreOutputPeriods()`、`alignF9pOutputConfig()` 增加 F9P 分支：GSA 默认 1 s，非合法周期对齐至不小于原值的最近合法值（超表尾取最大合法值，0 保持 0），并在启动恢复及调频提交成功后持久化至 NVS `gnss/out`；③ `tickPendingApply()` 频率回滚改调 `requestReconfigure(nowMs)` 同时恢复 `CFG-RATE-MEAS` 与旧 `out_cfg_`；④ `updateControllerStatus()` 仅在 `last_state_ == kProbing && curState == kConfiguring` 时置 `ppp_replay_`/`sat_replay_`，调频不重放星座。 |
| `main/modules/gnss/GnssModuleCmds.cpp` | `cmdPatchOutput()` 对 F9P 增加 `isF9pPeriodValid()` 校验；`cmdSetRate()`（离线直落盘与运行期两阶段路径）在 F9P 改频时同步对齐 `out_cfg_` 并记录回滚快照。 |
| `main/modules/gnss/GnssModuleFrames.cpp` | `handleUbxFrame()` 在收到包含 `CFG-NMEA-HIGHPREC`/`CFG-NMEA-SVNUMBERING` 的 VALSET NAK 时，除既有键列表 W 日志外追加显式降级 W 日志。 |
| `main/app/SettingsBackend.cpp` | `currentPeriod()` 将 `kCm680` 分支扩展为 `kCm680 || kZedF9p`，使 `config get gnss.out.*` 与网页 `config` 帧始终反映真实生效周期 `status.rxPeriodMs[index]`。 |
| `test/gnss/TestGnssUbloxReceiver.cpp` | 新增 5 组单元测试并更新 `testCaps`/`testGoldenCaps`/`testConfiguredOutputPort`（保持 `GoldenUblox.inc` 只读不动）。 |
| `test/settings/TestSettingsService.cpp` | 新增 `testF9pFiveHzDomainValidation`（验证 5 Hz 下拒绝 `10 Hz` 与 `500 ms`，放行 `0/200/1000` 及 `gsv=0`）。 |
| `tools/main_host_check/CMakeLists.txt` / `GnssResetCheck.cpp` | `gnss_output_check` 链接真实 `SettingsBackend.cpp`；新增 `verifyF9pRateOutputAndRollback` 端到端 F9P 模拟测试。 |

---

## 2. 设计取舍与技术核实

### 2.1 去除 `caps()` 中的 `cap::kRate20Hz` 及影响评估

- **原因**：`ZED-F9P` 四星座 RTK 最大解算频率依官方数据手册为 `8 Hz`（HPG 1.12）/ `7 Hz`（HPG 1.32），本次开放定位频率 `{1, 2, 5}` Hz，继续声明 `cap::kRate20Hz` 与实际能力矛盾。
- **替代位评估**：`components/gnss/include/gnss/Types.hpp` 为冻结公共头，其 `gnss::cap` 命名空间仅有 `kRtcmIn / kBase / kHeading / kIns / kRate20Hz`，无 `kRate5Hz` 位；同属亚 20 Hz 接收机的 `Cm680Receiver`（`{1,2,5,10}` Hz）同样只返回 `cap::kRtcmIn | cap::kBase`（`0x03`）。因此直接去掉 `cap::kRate20Hz` 无需新增能力位、无需改动冻结头 `Types.hpp`。
- **全局影响核查**：全仓搜索 `kRate20Hz` 确认生产逻辑（设置域、Web、Phone、OutputPlan）均不消费 `cap::kRate20Hz`，唯一可见变化是控制台 `gnss status` 打印 `caps=0x03`（原 `0x13`），以及 `TestGnssUbloxReceiver.cpp` 的 `testCaps()` 与 `testGoldenCaps()`（保留 `GoldenUblox.inc` 原文件不动，在 `testGoldenCaps()` 中显式声明 U2 去除 `kRate20Hz` 的差异，与 U1 在 `PlanCheck.hpp` 处理 GSA/GSV 键修正的做法一致）。

### 2.2 整倍导航历元规则与 `GoldenUblox.inc` 兼容

- `CFG-MSGOUT-*` 为 U1 每 `n` 历元分频（`0..255`），物理可实现周期为 `period == 0` 或 `period % Tnav == 0 && 1 <= period / Tnav <= 255`。
- 生产路径（`useConfiguredPort == true`）下 `rateHz ∈ {1,2,5}` 均整除 `1000`，`computeMsgoutEpochs()` 严格按 `periodMs % (1000 / rateHz) == 0` 判定，拒绝 `5 Hz + 500 ms` 静默取整为 `600 ms`。
- 历史黄金向量 `GoldenUblox.inc`（`useConfiguredPort == false`）包含 `rateHz = 3`（`Tnav = 333 ms`、`GSV = 1000 ms`），`computeMsgoutEpochs()` 仅在 `!useConfiguredPort` 时保留 `periodMs == navPeriod` 与 `(periodMs * rate) % 1000 == 0` 兼容分支，从而在严格拒绝生产路径非整倍的同时实现 `testGoldenOutputPlan` 100% PASS 且零改动黄金向量。

### 2.3 改频周期对齐与持久化（与 CM680 行为隔离）

- 现有 `GnssResetCheck.cpp:1501-1521` 明确约束 `CM680` 启动时保留合法高频偏好（如 1 Hz 下 `out_cfg_` 保持 `200 ms`，仅 `rxPeriodMs` 钳到 `1000 ms`）。因此本次对齐+持久化逻辑严格限定在 `Model::kZedF9p` 分支，不影响 CM680。
- 对 F9P：
  - `followsFixRate` 的 GGA/GST/RMC 自动跟随 `1000 / rateHz`（`1000/500/200 ms`），无需改写 `out_cfg_`；`followsFixRate` 的 GSA 在 `makePlan()` 中固定为 `1000 ms`。
  - 显式设置的非 0 周期（如 2 Hz 下设 `gga=500 ms` 后改频至 5 Hz，或 NVS 预存非法周期）按升序静态表对齐到不小于原值的最近合法整倍周期（找不到取最大合法值，0 保持 0），并在两阶段提交成功后同步写入 NVS `gnss/out` 与 `DataCenter::gnssOutCfg`；若改频失败/超时，`restorePendingApply()` 同时恢复旧 `rateHz` 与旧 `out_cfg_`，`tickPendingApply()` 调 `requestReconfigure(nowMs)` 重发旧 `CFG-RATE-MEAS` 与旧 `CFG-MSGOUT-*`。

### 2.4 NMEA 高精度（7 位小数）与长度上限核实

- **下发与 HPG 1.12 NAK 降级**：在 `cmdConfigurePort()` 末尾追加 `CFG-NMEA-HIGHPREC=1`、`CFG-NMEA-SVNUMBERING=1`（单帧 5 键，载荷 30 B < `kValsetPayloadMax = 80 B`）。若 `HPG 1.12` 对任一扩展键回 NAK（u-blox 拒绝整帧 VALSET），`GnssModuleFrames` 记录 W 日志，`ControllerConfigure` 调用 `buildPortConfigFallbackValset()` 剥离两扩展键并立即重发 3 键基础端口配置（保留原配置层、`CFG-RATE-MEAS`、`INPROT-RTCM3X`、`OUTPROT-NMEA`），确保基础配置与定位速率仍生效。
- **7 位小数 NMEA 长度上限逐级核实**（最坏情形 7 位小数 GGA 约 `92–100 B`，RMC ≤ `95 B`，GSA/GSV ≤ `95 B`）：
  1. `demux::nmea::kMaxFrame`（`components/stream_demux/include/demux/decoders/Nmea.hpp:14`）：**256 B**（余量 > 150 B）。
  2. `gnss::NmeaParser`（`NmeaParserNumeric.cpp:40,55`）：`kMaxAngleFracDigits = 7U`、`kMaxMmFracDigits = 9U`（`kMmScale = 3U`），无 82 字节硬限制；已由 `testNmeaHighPrec7DecimalGgaParse` 验证 92 字节 7 位小数 GGA 精确解析至 `1e-9 deg` 与 `mm`。
  3. `GnssLatestCache`（`main/data/GnssOutput.hpp:25`）：GGA/GST/RMC/GSA 槽各 **256 B**，单次查询上限 `kGnssMaxQueryBytes = 256 B`；`LatestGsv::bytes` 为 `proto::kMaxPayload = 256 B`。
  4. 手机 BT SPP 透传：`GnssAppRing` 总容 **4096 B**（单条有效载荷上限 4093 B），`spp_server` `TX.stream` **4096 B**，无 82 字节截断。
  5. 网页端：`WebLogic` 不透传原始 NMEA 文本，而是编码结构化 `gnss::Fix`（`latE9`/`lonE9`/`altMm`），无长度回归风险。
  - **边界提示（需 HIL 确认）**：`NmeaParserSatellites.cpp:35` 限制 `kMaxPrn = 255U`（受冻结头 `Types.hpp:131` `SatInfo::prn` 为 `uint8_t` 约束）。ZED-F9P 默认 NMEA 4.10 下 `SVNUMBERING=1` 扩展号段为 SBAS 152–158、QZSS 193–202、GPS 1–32、GLO 65–96、GAL 1–36、BDS 1–63（均 ≤ 255）；若某固件版本输出 >255 的 PRN（如 401+），`NmeaParser` 会将该 GSA/GSV 计为 `malformed`（不影响原始语句向手机透传，也不影响 GGA 定位），须在实板 HIL 观察 `gnss status` 的 `nmea.malformed` 计数。

---

## 3. 测试映射（修复前必败 → 修复后通过）

| 修复项 | 回归测试位置 | 修复前结果 | 修复后结果 |
| --- | --- | --- | --- |
| F9P `navRatesHz`/`outputPeriodsMs` 覆写 | `TestGnssUbloxReceiver.cpp::testF9pNavRatesAndOutputPeriods` | **FAIL** (`Expected 3 Was 0`) | **PASS** |
| `buildOutput` 严格整倍历元校验（拒绝 5 Hz+500 ms） | `TestGnssUbloxReceiver.cpp::testBuildOutputStrictMultiple` | **FAIL** (`Expected 1 Was 0`) | **PASS** |
| `caps()` 去除 F9P `cap::kRate20Hz` | `TestGnssUbloxReceiver.cpp::testCaps` & `testGoldenCaps` | **FAIL** (`kRate20Hz` 位为 1；`Expected 3 Was 19`) | **PASS** |
| `cmdConfigurePort` 含 `CFG-NMEA-HIGHPREC=1` & `SVNUMBERING=1`（字节级） | `TestGnssUbloxReceiver.cpp::testConfiguredOutputPort` & `testConfiguredPortNmeaHighPrecBytes` | **FAIL** (`Expected 38 Was 28`) | **PASS** |
| HPG 1.12 端口配置高精度键 NAK 非致命降级 | `TestGnssUbloxReceiver.cpp::testF9pControllerPortConfigNakFallback` | **FAIL** (`sawFiveKey` FALSE；降级前重试后 `kFailed`) | **PASS** |
| 7 位小数高精度 GGA（92 B）解析验证 | `TestGnssUbloxReceiver.cpp::testNmeaHighPrec7DecimalGgaParse` | PASS（基线确认解析能力） | **PASS** |
| F9P 5 Hz 设置域校验（拒绝 `10 Hz`/`500 ms`，放行 `gsv=0`） | `TestSettingsService.cpp::testF9pFiveHzDomainValidation` | 新增覆盖 | **PASS** |
| F9P 端到端：caps、`gnss.out.gsv 0` VALSET、1→5 Hz/2→5 Hz 500 ms 对齐与持久化、启动 `restoreOutputPeriods` 对齐持久化、频率回滚恢复 `CFG-RATE-MEAS`、改频不触发星座 VALSET、HPG 1.12 NAK 降级 | `tools/main_host_check/GnssResetCheck.cpp::verifyF9pRateOutputAndRollback` | **FAIL** (`caps1.navRateCount == 3U` 失败) | **PASS** |

---

## 4. 门禁结果

执行命令：
```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env COMP_WORDBREAKS=' '
ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix u3 --base 3bbd6d2
```
（注：非交互 shell 下 ESP-IDF v6.1 `export.sh` 在 `set -u` 下访问 `$COMP_WORDBREAKS`，传入 `COMP_WORDBREAKS=' '` 不改动脚本。）

| 门禁项 | 结果 | 备注 |
| --- | --- | --- |
| `G1-frozen` (`sha256sum -c docs/frozen.sha256`) | **PASS** | 仅更新授权的 `UbloxReceiver.hpp` 对应行 |
| `G2-*` / `G2b-*-asan`（13 个 host 套件 + `gnss-all` 共 28 项） | **27 PASS + 1 重跑 PASS** | 全量跑时 `G2b-t13_host_check-asan` 命中已知 `t13_gap_pairing_window` 偶发超时；单独执行 `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir builds/u3-t13_host_check-asan --output-on-failure` **45/45 PASS (8.00s)** |
| `G3-idf`（默认配置，0 warning） | **PASS** | `builds/u3-idf/rtk_main.bin` |
| `G3-idf-trace-off` | **PASS** | 0 warning |
| `G3-idf-wifi-off` | **PASS** | 0 warning |
| `G3-idf-ota-off` | **PASS** | 0 warning |
| `G3-idf-production` | **PASS** | 0 warning |
| `G3-idf-console-none` | **PASS** | 0 warning |
| `G3-idf-none-wifi` | **PASS** | 0 warning |
| `G3b-rtk_pure-esp32` | **PASS** | Xtensa 纯组件编译 0 warning |
| `G3b-rtk_pure-esp32c3` | **PASS** | RISC-V 纯组件编译 0 warning |
| `G5-format` (`clang-format --dry-run --Werror`) | **PASS** | 全部 C/C++ 文件通过 |
| `G5b-tidy` (`tidy_check.py`) | **PASS** | 151 个文件，0 告警 |
| `G6-doc_check` (`doc_check.py --base 3bbd6d2`) | **PASS** | 改动函数全部合格，不合格 0 个 |
| `G7b-runtime` (`g7b_check.py`) | **PASS** | 零新增堆分配，无异常/RTTI 符号 |
| `G7c-no-cli-deps` | **PASS** | `none-wifi` 配置无 CLI 残留依赖 |
| 实板 HIL（P2 v1.41 + ZED-F9P HPG 1.12） | **NOT_RUN** | 任务卡禁止烧录与触碰串口，交由 Architect 执行 |

---

## 5. 体积对比（相对干净基线 `3bbd6d2`）

干净基线构建于 `builds/u2-base/`（修改前编译），新构建于 `builds/u3-idf/`：

| 指标 | 基线 (`3bbd6d2`) | 本次 (`u3-idf`) | 增量 | 说明 |
| --- | ---: | ---: | ---: | --- |
| `rtk_main.bin` | 1,528,272 B | 1,531,024 B | **+2,752 B** | 分区 `0x1e0000`，剩余 `0x6a370` (22%) |
| `Total image size` | 1,528,162 B | 1,530,902 B | **+2,740 B** | — |
| Flash Code (`.text`) | 1,094,648 B | 1,097,196 B | **+2,548 B** | `cpp_cost.py diff` `text +2548` |
| Flash Data (`.rodata`) | 265,324 B | 265,516 B | **+192 B** | `cpp_cost.py diff` `data +192` |
| **IRAM** | 116,139 B (88.61%) | 116,139 B (88.61%) | **+0 B** | 剩余 14,933 B |
| **DRAM** (`.data`+`.bss`) | 96,763 B (77.67%) | 96,763 B (77.67%) | **+0 B** | 剩余 27,817 B（`.data 52011` / `.bss 44752`） |

---

## 6. 未验证项与风险

1. **P2 v1.41 + ZED-F9P HPG 1.12 实板行为（`NOT_RUN`）**：
   - `HPG 1.12` 对 5 键 `cmdConfigurePort`（含 `CFG-NMEA-HIGHPREC=1` + `CFG-NMEA-SVNUMBERING=1`）是否直接返回 ACK，或触发降级 W 日志。
   - 实板开启 `CFG-NMEA-HIGHPREC=1` 后 GGA 经纬度是否为 7 位小数、高程 3 位小数。
   - 实板开启 `CFG-NMEA-SVNUMBERING=1` 后，默认 NMEA 4.10 下 `gnss status` 的 `nmea.malformed` 是否为 `0`（确认无 >255 PRN 触发 `NmeaParserSatellites` 拒绝）。
   - 5 Hz 4 星座室外 RTK 满载 RTCM3 时，500 ms `VALSET` 超时是否偶发超时及 SPP 吞吐表现。
2. **10 Hz 定位频率**：按任务卡范围暂未开放（留待室外 3/4 星座 RTK HIL 评估）。

---

## 7. HIL 验证步骤（供 Architect 在 P2 v1.41 + ZED-F9P HPG 1.12 执行）

1. **烧录与启动检查**：
   - 烧录 `builds/u3-idf` 固件并打开 UART0 控制台（2 Mbaud）。
   - 确认探测日志 `<probe> detected ZED-F9P at 460800`，且无 `<ubx> NMEA high-precision VALSET NAK` 告警（若出现该 W 日志则确认接收机仍正常进入 `running`，并记录该版本固件行为）。
   - 执行 `gnss status`：确认 `model=ZED-F9P`、`caps=0x03`（不再含 `0x10`）、`state=running`、`nmea.malformed=0`。
2. **NMEA 高精度（7 位小数）核实**：
   - 开启 D 级日志或抓取手机 BT SPP 透传 GGA 语句：确认经纬度字段具备 7 位小数（`ddmm.mmmmmmm`）、高程 3 位小数，且 `gnss status` 定位解坐标更新正常、无截断/丢弃。
3. **控制台 `gnss.rate` 与 `gnss.out.*` 读写与掉电保持**：
   - `config get gnss.rate` → 预期 `1`；`config get gnss.out.gga` → `1000`；`config get gnss.out.gsv` → `5000`。
   - `config set gnss.out.gsv 0` → 预期成功；确认串口不再输出 GSV；断电重启后 `config get gnss.out.gsv` 仍为 `0`；再设回 `5000`。
   - `config set gnss.rate 2` → 预期成功，且日志**无**星座 `CFG-SIGNAL-*` VALSET 重放；设 `config set gnss.out.gga 500` → 成功。
   - `config set gnss.rate 5` → 预期成功、**无**星座 VALSET 重放；检查 `config get gnss.out.gga` 已自动对齐为 `1000`；确认 `config set gnss.out.gga 500` 返回 `bad_value`；确认实测 GGA 输出为 1 Hz（1000 ms）、RMC/GST 按 5 Hz（200 ms）、GSV 为 0.2 Hz（5000 ms）；断电重启后 `gnss.rate=5` 与 `gnss.out.gga=1000` 保持不变。
4. **网页端联动验证**：
   - 启动 Wi-Fi 配置页，检查 `hello`/`caps` 包含 `gnss.rate: [1,2,5]` 与 `gnss.out.{gga,gsa,gsv,gst,rmc}`；切换 `gnss.rate` 在 `1/2/5` Hz 间切换时，前端 `gnss.out.{gga,gst,rmc}` 下拉项动态刷新为 `[0,1000]` / `[0,500,1000]` / `[0,200,1000]` 且当前值不出现空白或失步。

## 7. 评审修复补充（Architect）

- RU2-01：降级 3 键端口 VALSET 再被 NAK 时不再清零重试计数，避免配置态无限循环；回归测试 `testF9pControllerPortConfigFallbackNakFails`（修复前失败，修复后通过）。详见 `docs/reports/U2-review.md`。
- 复跑全量门禁 `--prefix u4 --base 3bbd6d2`：**43 passed, 0 failed**；`rtk_main.bin` 1,531,008 B。

## 8. 实板 HIL（2026-11-24，P2 v1.41 + ZED-F9P HPG 1.12，室内无星）

固件 `builds/u4-idf`（4ed2191），控制台 `/dev/ttyACM1`。

| 项 | 结果 |
| --- | --- |
| 5 键端口 VALSET（含 HIGHPREC/SVNUMBERING，payload 30 B）在 HPG 1.12 上 | **ACK**，`nacks=0`，未走 3 键降级 |
| `gnss rate 5` | CFG-RATE-MEAS=200 ms；GSA 自动对齐为 5 历元（1 s），GSV 25 历元（5 s）；全程 `nacks=0 fail=0` |
| 改频率不重放星座 | 改频率只发 7 帧 VALSET（无 52 B 星座帧） |
| 5 Hz 下非整数倍周期 | `gnss out gga 2` → `ERR invalid argument`；`config set gnss.out.gga 500` → `ERR bad value`；合法值 `gnss out gga 1/5`、`config set gnss.out.gga 200` 成功 |
| `gnss rate 1` 回到 1 Hz | 200 ms 周期自动对齐到 1000 ms（W 日志 `aligned F9P output[0] from 200 to 1000 ms`），`config get gnss.out.gga=1000` |
| NMEA 统计 | 5 Hz 约 20 s：`ign=0 bad=0` |

未验证（需室外有星）：GGA 7 位小数；SVNUMBERING 在真实卫星下 `nmea.bad=0`；5 Hz 四星座 RTK + RTCM 满载下 VALSET 超时与 SPP 吞吐。
附注（既有，不属 U2）：`gnss out show` 在 F9P 上仍列出 Unicore 专用的 `sats`（SATSINFO）为 on，实际 `unsupported` 位已正确置位，仅显示误导。
