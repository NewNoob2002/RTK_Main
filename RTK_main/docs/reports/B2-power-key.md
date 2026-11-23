# B2：P2 电源按键、关机管理与 SGM41513 I2C USB 检测报告

- 任务卡：`docs/tasks/B2-power-key-shutdown.md`
- 补充修复单：C1–C6（2026-11-21 板测发现 P2 v1.41 GPIO39 nPG 不可用 → 改用 SGM41513 I2C 检测 USB 在位 + 移植 V2.7 充电参数 + 修复关机 GPIO 冲突告警）
- 分支：`feat/b2-power-key`（基线 `main d6db196`）
- 结论：实现与全部自动门禁 **40/40 PASS**（`--prefix b2fix`），冻结清单 0 改动；Architect 于 2026-11-23 在 P2 v1.41 上完成主路径实板验证（见 §9），硬关断与若干注入类待验项仍 `NOT_RUN`。

## 1. 改动文件

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `main/board/PowerLogic.hpp`、`main/board/PowerLogic.cpp` | 新增 | 纯 C++20 电源逻辑（无 IDF 依赖）：`BootMode`、`PowerOnReason`、`decidePowerOn` 启动分支、`PowerKeySm` 按键消抖/短按/长按状态机、`ShutdownSm` 分阶段关机状态机；新增 SGM41513 寄存器常量（BQ25601 兼容映射）、`decodeSgmReg08`、`maskSgmReg02`/`maskSgmReg03`、`decideUsbPresent`（I2C 优先，失败退化到 GPIO2 STAT）。 |
| `main/board/Charger.hpp`、`main/board/Charger.cpp` | 新增 | P2 SGM41513 I2C 驱动（ESP-IDF v6.1 `driver/i2c_master.h`，100 kHz，10 ms 超时，静态互斥锁保护）：幂等 `init`、`readReg`/`writeRegMasked`、`readInputStatus`、`applyChargeParams`（V2.7 REG02[5:0]=51 / REG03=0xF8）、`dumpRegs`（REG00–REG0B）。 |
| `main/modules/power/PowerModule.hpp`、`main/modules/power/PowerModule.cpp` | 新增 | `power` 模块（`kLink`、`ModuleEnable::kAlways`、10 ms 轮询）；仅 P2 `kKeyEmulate` 生效；线程安全 `power::requestShutdown(Reason)`、四阶段关机状态机、充电待机长按 2.5 s 确认保持并重启；运行期 200 ms 节流读 SGM41513 I2C（失败退化到 STAT），`kQuiesce` 改调 `setSafeOutputLevels` 消除 `gpio conflict` 告警。 |
| `main/modules/power/PowerCmds.cpp` | 新增 | DEV 档 `power` 命令组：`power status`（含 `usb_src=i2c|stat|none` 与 `reg08=0x..`）、`power off`（走 `kConsole`）、`power charger`（只读 dump REG00–REG0B 及 REG08/REG0A 解码）。生产档不注册。 |
| `main/board/BoardProfile.hpp`、`main/board/BoardProfile.cpp` | 修改 | 新增 `PowerKey` 与 `ChargerPins`；P2 禁用 `usbDetectGpio = -1`（GPIO39 不可用），登记 `charger`（I2C0、SDA21/SCL22、0x1A、ICHG 51、REG03 0xF8）；E1 全 `-1` 不初始化 I2C。 |
| `main/board/BoardStartup.hpp`、`main/board/BoardStartup.cpp` | 修改 | `startupPower()` 在 `decidePowerOn` 前初始化 SGM41513、下发 V2.7 充电参数（正常开机与充电待机均执行，失败仅记 `FW_LOGW` 不阻断开机）并读 REG08（失败退化到 GPIO2 STAT）；新增 `setSafeOutputLevels()` 仅更新输出电平不重复 `gpio_config`。 |
| `main/app/RadioMode.hpp` | 修改 | 新增 `ModuleEnable::kNormalOnly` 与 `moduleEnabled(BootMode, RadioMode, ModuleEnable)`，充电待机仅启用 `kAlways` 模块，不复制模块表。 |
| `main/module_list.inc`、`main/AppConfig.hpp`、`main/AppContext.hpp`、`main/Kconfig.projbuild`、`main/CMakeLists.txt` | 修改 | 新增 `RTK_POWER_OFF_PRESS_MS`（默认 2500 ms）、`ModuleKind::kPower`、注册 `PowerModule`、`GnssModule` 改为 `kNormalOnly`；`CMakeLists.txt` 接入 `board/Charger.cpp` 与 `esp_driver_i2c`。 |
| `main/app/BootFlow.cpp`、`main/app/BootBuffers.cpp`、`main/app/Executors.cpp` | 修改 | `stepPower` 接入 `startupPower`；充电待机跳过 `stepRadio`、BT/Wi-Fi 启动、GNSS 缓冲分配与 `stepIo`；`kChargeStandbyModules` 复用同一 `module_list.inc`；`kLinkConfig.idleWaitMs = 10`。 |
| `main/app/SettingsBackend.hpp`、`main/app/SettingsBackend.cpp` | 修改 | 新增无锁原子 `setSettingsWritesBlocked(bool)`；关机 `kAnnounce` 起拦截新 `settings::Service::set()` 返回 `kBusy`，已排队请求继续执行；30 s 超时回退时解除拦截。 |
| `main/modules/storage/StoreWriteGate.hpp`、`main/modules/storage/StorageModule.hpp`、`main/modules/storage/StorageModule.cpp` | 修改 | 暴露 `store::inFlightWrites()`；`StorageModule` 新增 `hasPendingWrites()`、`flushAllPending(nowMs)` 与 `formatPendingSummary()`。 |
| `main/modules/bt_link/BtLinkModule.hpp`、`main/modules/bt_link/BtLinkModule.cpp`、`main/modules/web/WebModule.hpp`、`main/modules/gnss/GnssModule.cpp` | 修改 | `BtLinkModule` 新增 `disconnectAll()` 与 `hasPendingSettings()`；`WebModule` 新增 `hasPendingSettings()`；`GnssModule` 维护 `DataCenter::gnssPendingNvs`。 |
| `main/def/SystemDef.hpp`、`main/data/AppData.hpp`、`main/data/AppData.cpp`、`main/modules/debug/TraceText.cpp` | 修改 | `PowerSnapshot` 新增 `usbSource`/`chargerOk`/`chargerReg08`；新增 `DataCenter::powerStatus` 与 `kSys` `PowerTrace`（码段 32–35）。 |
| `main/app/ConsoleRegistration.hpp`、`main/app/ConsoleSetup.cpp` | 修改 | DEV 档注册 `power` 命令与动词组；PRODUCTION / NONE 档不注册。 |
| `tools/main_host_check/PowerLogicCheck.cpp`、`CMakeLists.txt`、`BoardProfileCheck.cpp`、`ConsoleProfileCheck.cpp`、`RadioModeCheck.cpp` | 修改/新增 | 补齐按键、关机、启动分支、充电待机开机、USB I2C/STAT 综合判定与 REG02/REG03 掩码 host 测试（含 ASan/UBSan）。 |
| `docs/tasks/B2-power-key-shutdown.md`、`docs/adr/ADR-0015-board-profiles.md` | 修改 | 同步 GPIO39 不可用板测证据与 SGM41513 I2C 方案（不在 `docs/frozen.sha256` 中）。 |

## 2. DRAM / IRAM / bin 差值

对比基线 `d6db196`（`builds/b2-baseline`）、修复前 B2（`builds/b2-idf`）与当前 `builds/b2fix-idf`（`python -m esp_idf_size --diff`）：

| 指标 | 基线 `d6db196` | 修复前 B2 | 当前 B2 (`b2fix`) | 相对基线 `d6db196` | 本次 C1–C6 增量 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `rtk_main.bin` | 1,457,472 B | 1,465,808 B | **1,483,680 B** | **+26,208 B** (+25.59 KiB) | +17,872 B |
| Flash Code (`.text`) | 1,044,136 B | 1,050,216 B | **1,062,676 B** | **+18,540 B** | +12,460 B |
| Flash Data (`.rodata` 含段头) | 246,092 B | 248,124 B | **252,788 B** | **+6,696 B** | +4,664 B |
| IRAM (`.text`) | 114,715 B | 114,715 B | **115,435 B** | **+720 B**（88.07%，剩余 15,637 B） | +720 B |
| DRAM (`.data` + `.bss`) | 96,931 B | 97,163 B | **97,307 B** | **+376 B**（78.11%，剩余 27,273 B） | +144 B |

说明：本次增量主要来自新接入的 ESP-IDF `esp_driver_i2c` 组件（含 720 B IRAM ISR/驱动代码与静态互斥锁/句柄）。

## 3. 延迟写 / 去抖写路径盘点（`kSave` 阶段）

| 路径 | 所在文件 | 延迟机制 | 关机 `kSave` 处理 |
| --- | --- | --- | --- |
| 报警器白名单 (`whitelist`) | `main/modules/storage/StorageModule.cpp` | 500 ms 去抖 + 失败指数退避（1 s..30 s） | `StorageModule::flushAllPending()` 跳过去抖与退避立即落盘 |
| 手机 5 项 Blob（`height`/`pn`/`ntrip`/`json_srv`/`tb`） | `main/modules/storage/StorageModule.cpp` | 500 ms 去抖 + 退避 | 同上，`flushAllPending()` 立即落盘 |
| Settings Service 在途请求（BT / Web） | `SettingsBackend.cpp`、`BtLinkModule`、`WebModule` | 异步投递至 `kLink`/`kIo` | `kAnnounce` 拦截新 `set()` 返回 `kBusy`；`kSave` 等待在途凭据清零 |
| GNSS 异步配置落盘（`rate`/`out`/`ppp`/`sat_track`/`gga_gate`） | `main/modules/gnss/GnssModule*.cpp` | 两阶段提交（等接收机 ACK 后写 NVS） | 通过 `DataCenter::gnssPendingNvs` 等待至多 1000 ms |
| 跨任务在途 NVS 写 | `main/modules/storage/StoreWriteGate.hpp` | `WriteGuard` 原子计数 | `kSave` 检查 `store::inFlightWrites() == 0` |
| 其他模块（`debug`、`rtcm`、`system`、`BootConfig`） | 各自模块 | 无延迟/去抖写 | 无需额外冲刷 |

## 4. 门禁结果

执行命令：`unset IDF_PATH; . $HOME/esp/esp-idf/export.sh; bash tools/ci/run_gates.sh --prefix b2fix`（基线 `d6db196`），结果：**40 passed, 0 failed**。

| 门禁 | 结果 | 说明 |
| --- | --- | --- |
| `G1-frozen` | PASS | `sha256sum -c docs/frozen.sha256` 全部通过，无冻结文件改动 |
| `G2-*` / `G2b-*-asan`（13 套，含 `main_host_check` 与 `power_logic_check`） | PASS | 普通与 ASan/UBSan 构建全部通过 |
| `G3-idf`（6 档：默认 / `trace-off` / `wifi-off` / `production` / `console-none` / `none-wifi`） | PASS | 全部 0 warning，`rtk_main.bin` 1,483,680 B |
| `G3b-rtk_pure-esp32` / `G3b-rtk_pure-esp32c3` | PASS | 0 warning |
| `G5-format` | PASS | `clang-format --style=file --dry-run --Werror` 全部通过 |
| `G5b-tidy` | PASS | 146 个文件，0 条发现 |
| `G6-doc_check` | PASS | 相对 `d6db196` 全部 102 个新增/改动函数 Doxygen 注释合格 |
| `G7b-runtime` | PASS | 无违规堆分配/异常/RTTI 符号 |
| `G7c-no-cli-deps` | PASS | `NONE + Wi-Fi` 构建未链入 `powerCmds` 或任何 CLI/REPL 符号 |
| 实板 HIL（含 C1–C5 验证） | NOT_RUN | 规则禁止烧录与连串口，待 Architect 实板验收 |

## 5. 已知限制、偏离与待实板确认项

- **已知限制**：若 SGM41513 I2C 通信故障且电池已充满（GPIO2 STAT 为高），退化路径无法检测到 USB 在位，关机插充电器仍会按无 USB 走全功能开机。
- **偏离**：无冻结文件改动；`kLinkConfig.idleWaitMs` 调整为 10 ms 以匹配 10 ms 按键轮询。
- **待实板确认项**：
  1. `power charger` 在插/拔充电器两种状态下 dump `REG00–REG0B`，核对 SGM41513 `REG08`（`VBUS_STAT[7:5]`、`CHRG_STAT[4:3]`、`PG_STAT[2]`）与 `REG0A[7] VBUS_GD` 位定义是否与当前 BQ25601 兼容映射完全一致。
  2. 核对启动后 `REG02[5:0]` 是否确为 `51 (0x33)`（且高 2 位保留）、`REG03` 是否确为 `0xF8`，以及 SGM41513 实际充电电流/预充/终止电流档位。
  3. 关机状态插充电器 → `power_on=charge` 进入 `charge_standby`（`usb=1 usb_src=i2c`），待机长按 2.5 s 正常开机。
  4. 正常运行插充电器时执行 `power off` → 无 `gpio: conflict found` 告警，U3 关断后重启进 `charge_standby`。
  5. 实板校准 `senseOnMv`、U3 关断时长与无 MCU 硬关断时长后，由 Architect 更新 ADR-0015 §2.4/§4/§5。

## 6. 第二轮修复

分支 `feat/b2-power-key`，任务基线 `d6db196`。下表路径相对 `RTK_main`。原任务卡 §0、§6 沿用。

| 清单 ID | 改动文件及入口 | Host 回归 |
| --- | --- | --- |
| K1 | `main/board/PowerLogic.cpp:140`。初次松开经 30 ms 消抖才布防，释放候选期间抑制长按，在弹跳撤销后继续按住计时。 | `PowerLogicCheck.cpp` 的 `testKeyReleaseBoundaries`、`testKeyStateMachine`，含 2490 ms 松开、首样本毛刺、精确 2500 ms 与回绕。 |
| S1 | `main/app/PowerRequests.hpp:17`、`main/app/SettingsBackend.cpp:277`。原子关闭准入并等已获准的投递结束。`main/modules/bt_link/BtLinkModule.cpp:188`、`main/modules/bt_link/BtLinkSessions.cpp:199` 接入手机三个直写和白名单收集。`main/modules/power/PowerModule.cpp:291` 等待 kLink 队尾确认，向 kIo 投递 `kFlushGgaGate`，并观察 `main/modules/gnss/GnssModule.cpp:292`、`main/modules/gnss/GnssModuleCmds.cpp:181` 发布的实际待写状态。 | `PowerLogicCheck.cpp` 的 `testPowerArbitration`、`MainHostCheck.cpp` 的 `power_no_reopen`、`GnssResetCheck.cpp` 的 `testGnssSettingsBusAndGgaPersist` 中 GGA 失败退避与立即冲刷段。 |
| X2 | `main/app/PowerRequests.hpp:47` 统一关机与普通重启仲裁。`main/modules/bt_link/BtLinkModule.cpp:596`、`main/modules/web/WebModule.cpp:419` 在受理和执行处拦截冲突的重启。控制台重启入口也走同一仲裁。Web 恢复出厂在实际擦除前取得重启权。 | `MainHostCheck.cpp` 的 `phone_nvs_readback` 确认手机已排定重启可被关机取消。`WebModuleCheck.cpp` 的 `testShutdownArbitrationAndSoftApResume` 覆盖已排定与新到达的 Web exit。 |
| T1 | `main/modules/power/PowerModule.cpp:251`、`main/board/PowerLogic.cpp:190`。待机每轮读 GPIO25，连续三次为高后释放 GPIO4 并重启。到达 5 s 截止时释放并留在待机。运行期确认无需申请 ADC 对象。 | `MainHostCheck.cpp` 的 `power_standby` 调用真实 PowerModule，验证 GPIO4 边沿、超时、重试和成功后重启。`PowerLogicCheck.cpp` 的 `testChargeStandbyLongPressPowerOn` 覆盖数字电平毛刺与回绕。 |
| P12 | `main/modules/power/PowerCmds.cpp:104`、`main/modules/power/PowerModule.cpp:149` 拒绝待机关机。`main/board/PowerLogic.cpp:232` 在离开静默时处理 USB 已在位且 U3 已关的情况。 | `MainHostCheck.cpp` 的 `power_standby`、`power_no_reopen` 及 `PowerLogicCheck.cpp` 的 `testCutPowerBoundaries`。 |
| W1 | `main/modules/web/WebServer.cpp:1080` 在 HTTP 任务暂停和恢复 SoftAP，暂停时拒绝 WebSocket 并保留已分配的服务器。kLink 最多等 5.5 s，超时记录告警。`main/modules/bt_link/BtLinkModule.cpp:544`、`main/modules/bt_link/BtLinkSessions.cpp:199` 暂停 GAP 接入、断开已有连接并拒绝晚到 OPEN。kLink 最多等 150 ms。`main/modules/power/PowerModule.cpp:458` 的断电超时回退恢复无线接入。 | `WebModuleCheck.cpp` 的 `testShutdownArbitrationAndSoftApResume` 覆盖队列拒绝后重试与原服务器恢复。`MainHostCheck.cpp` 的 `power_bt_pause` 覆盖关闭确认、拒绝晚到 OPEN 和恢复接入。 |
| C1 | `main/board/Charger.cpp:222` 对 REG08 尝试立即取锁，争用保留样本，实际通信失败切换 STAT。`main/board/Charger.cpp:263` 的诊断逐寄存器持锁。`main/modules/power/PowerModule.cpp:205` 静默完成时强刷一次，其他静默轮次沿用缓存。 | `PowerLogicCheck.cpp` 的 `testUsbPresentAndChargerMasks` 验证好样本在争用时保留、传输失败时退化、成功空输入可覆盖旧样本。 |
| X3 | `main/board/BoardStartup.cpp:527`、`main/board/PowerLogic.hpp:213`。低 U3 且初次未识别 USB 时，每 20 ms 复读至多 350 ms，再确认 GPIO25 后选启动分支。单次 I2C 读取另受 10 ms 传输超时约束。 | `PowerLogicCheck.cpp` 的 `testBootUsbDelay` 覆盖 240 ms 后 REG08 更新、无 USB 到期与 U3 已高快速退出。 |
| X8 | `main/board/PowerLogic.cpp:286` 在 USB 模式切换时重置无 USB 脉冲相位。`main/modules/power/PowerModule.cpp:488` 在日志/UART 检查及强刷 REG08 后读取断电阶段起始时钟。 | `PowerLogicCheck.cpp` 的 `testCutPowerBoundaries`、`testShutdownNoUsb` 覆盖 10 ms 插拔重现、首段 2 s 和后续 3.1 s 边沿。 |
| X11 | `main/modules/storage/StorageModule.cpp:322`、`main/modules/storage/StorageModule.cpp:591`、`main/modules/power/PowerModule.cpp:441`。只在进入保存阶段跳过一次既有退避，后续普通轮询保留失败间隔。新到达的已准入延迟设置在保存阶段跳过去抖。白名单和 Blob 写仍归 Storage 的 kLink 轮询执行。 | `MainHostCheck.cpp` 的 `power_save_backoff` 验证进入保存前保留去抖，写失败后 990 ms 内没有第二次尝试。 |
| R3 | `main/board/Charger.cpp:238` 移除 REG03 读改写并更正电流编码说明。`main/board/PowerLogic.hpp:190` 和 `main/modules/power/PowerCmds.cpp:1` 修正文档。 | `PowerLogicCheck.cpp` 的 `testUsbPresentAndChargerMasks` 保留 REG02 高位、USB 来源和错误退化验证。REG03 总线写由 HIL 核对。 |
| E1 | `main/app/Executors.cpp:177`、`main/board/BoardProfile.hpp:120` 按板型选择 kLink 空闲等待。待机 debug/trace 缓冲裁剪暂缓，当前日志环、启动降级流程和 DEV 诊断共用既有装配，裁剪需同步调整这三处。 | `BoardProfileCheck.cpp` 的 `checkProfiles` 检查 E1 20 ms、P2 10 ms。 |
| 日志 | `main/modules/power/PowerModule.cpp:278`、`main/modules/power/PowerModule.cpp:541`。两条电源重启路径逐轮检查日志环和 UART，最长等待 150 ms。移除 `ShutdownOutput::sampleSenseInLowPhase`，运行期 GPIO25 随每轮电源状态采样。 | `MainHostCheck.cpp` 的 `power_standby`、`power_no_reopen`、`PowerLogicCheck.cpp` 的 `testShutdownNoUsb`。实际日志输出完整性留待 HIL。 |
| 测试 | `tools/main_host_check/CMakeLists.txt:62` 将真实 PowerModule 链入两种手机策略的 host 测试。`tools/main_host_check/MainHostCheck.cpp:319` 注入 GPIO、时间、充电状态和重启替身，替代测试中自行实现的待机分支。 | `power_standby`、`power_no_reopen`、`power_bt_pause`、`power_save_backoff` 在普通与 ASan/UBSan 两份构建运行。 |

### 第二轮资源差值

以 `stat -c %s` 读取实际 bin，以 `python -m esp_idf_size --diff` 比较各目录的 `rtk_main.map`。数值来自默认 ESP32 档构建。

| 指标 | 第二轮 `b2fix2-idf` | 相对第一轮 `b2fix-idf` | 相对基线 `d6db196` |
| --- | ---: | ---: | ---: |
| `rtk_main.bin` | 1,486,784 B | +3,104 B | +29,312 B |
| DRAM | 97,355 B（78.15%，剩余 27,225 B） | +48 B | +424 B |
| IRAM | 115,435 B（88.07%，剩余 15,637 B） | 0 B | +720 B |

### 第二轮新增 HIL 待验项

1. 冷插 USB 测量第一拍及首次有效的 REG08、GPIO25 电压和 GPIO4 波形，核实识别窗口及充满电时的分支。
2. 注入启动按住释放毛刺、2490 ms 松开、持续 2500 ms，并分别检查按键计数、GPIO4 与开关机行为。待机强制保持失败时核实 GPIO4 释放、重试及 kLink TWDT、栈水位。
3. 持续物理按住直至 U3 先关、USB 在位时执行关机。测断电阶段入口应保持 GPIO4 低。插拔 USB 并同时运行 `power charger`，核对无 USB 脉冲时间与 USB 采样缓存行为。
4. 蓝牙连接、重连及 Web socket 停滞时关机，观察 SPP CLOSE、SoftAP 停止、kLink 最大轮耗时。令 U3 保持 30 s 未掉电，验证原 SoftAP/HTTP、蓝牙接入及设置写入恢复。
5. 在保存前交错提交 GNSS、白名单与手机 Blob 设置，注入 NVS 错误，核对关门后的协议回执、GGA 立即冲刷、保存期限及失败重试间隔。用上电读回确认成功落盘的数据。
6. 捕获 USB 关机及待机开机重启前的日志，并核对两路 GPIO4 和 U3 波形。重新转储 REG02、REG03 核对编码和电流。

### 第二轮门禁结果

`unset IDF_PATH; . "$HOME/esp/esp-idf/export.sh"; bash tools/ci/run_gates.sh --prefix b2fix2` 最终退出码 0（**40 passed, 0 failed**，日志位于 `builds/b2fix2-logs/`）。将本机缓存的 lizard 1.26.1 单独加入 PATH。沙箱内 LeakSanitizer 自检失败，复跑设置 `ASAN_OPTIONS=detect_leaks=0`，保留地址与未定义行为检测。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| G1 冻结校验 | PASS | 冻结清单校验通过。 |
| G2 / G2b Host 普通与 ASan/UBSan | PASS | 13 套各两次，`main_host_check` 各 91 用例全部通过。 |
| G3 / G3b ESP-IDF | PASS | 默认、trace-off、wifi-off、production、console-none、none-wifi 与两个纯组件测试工程均通过零告警门禁。 |
| G5 / G5b / G6 | PASS | 格式合格。clang-tidy 146 个文件、0 条发现。Doxygen 门禁 150 个函数合格。 |
| G7b / G7c | PASS | 运行期符号门禁及无控制台档 CLI 依赖检查通过。 |
| `idf.py -B builds/b2fix2-idf size`、`esp_idf_size --diff`、`git diff --check` | PASS | 产物核对见上表，工作树差异检查通过。 |
| 实板 HIL | NOT_RUN | 由 Architect 执行，第二轮待验项见上文。 |

## 7. 第三轮修复

分支 `feat/b2-power-key`，任务基线 `d6db196`。任务卡 §0、§6 继续作为板级前提。

| 清单 ID | 文件与入口 | Host 回归 |
| --- | --- | --- |
| A1 | `components/link_core/services/PairService.cpp:119` 和 `components/link_core/LinkManager.cpp:297` 在手机 0x44 清表与报警器学习之前检查准入。`main/modules/bt_link/BtLinkSessions.cpp:207` 登记已受理的写，`main/modules/bt_link/BtLinkModule.cpp:936` 在同轮接收结束后交给 Storage 再结束登记。`main/support/ConsoleSupport.cpp:139` 在投递前登记控制台持久化命令。`main/modules/gnss/GnssModule.cpp:292` 将延期、执行中及落盘中的切速纳入待写快照。 | `power_phone_whitelist_closed` 验证协议失败码、报警器学习、三个手机 Blob 入口及 RAM/NVS 一致。`power_phone_whitelist_admitted` 验证已回成功的清表能落盘。`power_no_reopen` 验证关门前的控制台白名单命令先于栅栏执行，关门后拒绝同类命令和 GGA 写。`gnss_output` 检查跨轮切速的待写快照。 |
| A2 | `main/board/Charger.cpp:222` 返回 REG08 刷新结果。`main/modules/power/PowerModule.cpp:207` 在争用时保留显示缓存并于下一轮重试。`main/board/PowerLogic.cpp:301` 在 U3 已低且缺少有效 USB 样本时保持 GPIO4 低，同时保留断电超时。 | `power_busy_at_cut` 重放切断入口 REG08 争用、STAT 无效及 U3 已低，验证下一轮刷新、零 GPIO4 高沿和 USB 重启。 |
| A3 | `components/spp_server/SppServerWorker.c:21` 把扫描关闭、恢复与 SDK 投递结果交给静态请求槽和 SPP worker。`main/modules/bt_link/BtLinkModule.cpp:563` 在 kLink 轮询，提交失败后限频重试。关机期间应用层仍拒绝晚到 OPEN。 | `t13_scan_pause_worker` 在真实驱动上堵住 SDK 调用并请求恢复，验证调用方仍可轮询和变更目标。`power_timeout_recovery` 验证扫描未提交时静默仍有界退出。 |
| A4 | `main/modules/web/WebModule.cpp:398` 在取得重启权后才发 exit 成功 ACK 和 bye，取得前被关机抢占则回 busy。`main/modules/web/WebModule.cpp:574` 覆盖仍在等待 GGA 冲刷的 exit。 | `web_module_check` 与 `web_module_portal_off_check` 核对成功 ACK→bye→重启，以及 exit 排定→关机→busy→SoftAP 恢复的帧序列。 |
| A5 | `main/modules/storage/StoreWriteGate.hpp:23` 发布保存阶段起点与静默状态。`main/modules/storage/StorageModule.cpp:322` 在调度延迟写前检查截止。`main/modules/gnss/GnssModuleCmds.cpp:555` 保留 GGA 待写，`main/modules/gnss/GnssModuleMaintenance.cpp:334` 保留已完成的接收机切速结论。断电失败后恢复提交。 | `power_timeout_recovery` 让 Storage 先于 Power 在保存第 1000 ms 轮询，验证拒绝新写，回退后重试成功。`gnss_output` 验证到期后的 GGA 冲刷及切速均保留待写，恢复后读回 NVS。 |
| A6 | `main/modules/power/PowerModule.cpp:258` 在待机 GPIO25 保持确认后取得重启权。`main/modules/power/PowerCmds.cpp:108` 使用通用电源动作冲突提示。 | `power_standby` 验证日志等待期间其他入口无法再次取得重启权。 |
| A7 | `main/modules/debug/DebugModuleLog.cpp:117` 由日志消费者在 pop 前标记交付开始，向输出端交付后确认序号。`main/modules/power/PowerModule.cpp:288` 同时检查消费者、文本日志环及 UART，原 150 ms 上限继续生效。 | `power_log_drain` 在最后一条日志弹出且交给 UART 前推进 30 ms，验证重启仍等待，交付后继续。 |
| A8 | `main/modules/power/PowerModule.cpp:301` 和 `main/modules/power/PowerModule.cpp:464` 覆盖保存栅栏与超时回退。 | `power_no_reopen` 与 `power_fence_timeout` 绑定真实 kLink/kIo 执行器，验证 `kSaveFence`/`kFlushGgaGate` 排队顺序、等待和取消。kIo 用可控命令替身核对顺序，实际 GGA 落盘由 `gnss_output` 验证。`power_timeout_recovery` 与两个 Web 变体运行真实 PowerModule 至 30 s 回退，验证 GPIO4、BT/Web 接入恢复及 BT 路径二次关机。 |

A3 的 ESP-IDF v6.1 GAP API 只确认 BTC 入队。应用层 OPEN 拒绝和 SPP CLOSE 独立处理，射频扫描状态由 HIL 确认。请求槽使用静态存储，SDK 本身的 BTC 投递分配和等待发生在 SPP worker。A5 在截止前受理的同步 NVS 调用仍由 SDK 自行返回，截止后拒绝启动新提交。

待机 Debug/trace 裁剪继续暂缓。日志环和输出任务仍承担待机长按重启与降级启动的日志，裁剪需同步调整启动装配和日志出口，并量测待机电流。

### 第三轮 HIL 待验项

1. 在手机清表或报警器学习成功后立即长按关机，上电读回白名单。关门后重放 0x44、手机 Blob 写和 DEV 持久化命令，核对失败回执与 NVS。
2. 在 USB 插入、充满且 GPIO25 已低时用诊断访问制造 REG08 争用。捕获 GPIO4，核对争用解除后的下一轮采样和充电待机重启。
3. 在 BT 流量和 BTC 队列拥塞时关机，测量 kLink 轮耗时及 TWDT，捕获 SPP CLOSE 和空中可发现性。强制 U3 维持 30 s，检查恢复扫描后新手机能连接。
4. 向 Web exit 发包后并发长按，核对 busy、重连和 SoftAP/HTTP 恢复。保存截止附近注入 NVS 失败，核对断电期间无新写，回退后白名单、GGA 和切速结果成功落盘。
5. 在 trace_out 刚取到重启日志时延迟 UART 输出，测量交付与重启时差。待机 U3 保持确认完成后并发执行 `sys reboot`，核对只有一次重启。

### 第三轮资源差值

默认 ESP32 档。bin 使用 `stat -c %s`，DRAM 与 IRAM 使用 `idf.py -B builds/b2fix3-idf size` 并与第二轮及基线的 map 做 `python -m esp_idf_size --diff` 对照。

| 指标 | 第三轮 `b2fix3-idf` | 相对第二轮 `b2fix2-idf` | 相对基线 `d6db196` |
| --- | ---: | ---: | ---: |
| `rtk_main.bin` | 1,489,008 B | +2,224 B | +31,536 B |
| DRAM | 97,387 B（78.17%，剩余 27,193 B） | +32 B | +456 B |
| IRAM | 115,435 B（88.07%，剩余 15,637 B） | 0 B | +720 B |

### 第三轮门禁结果

`unset IDF_PATH; . "$HOME/esp/esp-idf/export.sh"; bash tools/ci/run_gates.sh --prefix b2fix3` 最终退出码 0，**40 passed, 0 failed**。日志位于 `builds/b2fix3-logs/`。沿用本机缓存的 lizard 1.26.1 与 `ASAN_OPTIONS=detect_leaks=0`，地址和未定义行为检测保留。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| G1 冻结校验 | PASS | `sha256sum -c --quiet docs/frozen.sha256`。 |
| G2 / G2b Host 与 ASan/UBSan | PASS | 13 套各两次，`main_host_check` 每套 103/103，`t13_host_check` 每套 45/45。 |
| G3 / G3b ESP-IDF | PASS | 六档主固件与 ESP32、ESP32-C3 纯组件测试工程均通过零告警门禁。 |
| G5 / G5b / G6 | PASS | 格式合格，clang-tidy 146 个文件、0 条发现，Doxygen 门禁 178 个函数合格。 |
| G7b / G7c | PASS | 运行期符号和无控制台档 CLI 依赖检查通过。 |
| `idf.py -B builds/b2fix3-idf size`、`esp_idf_size --diff`、`git diff --check` | PASS | 以最终构建产物统计，资源数据见上表。 |
| 实板 HIL | NOT_RUN | 由 Architect 执行，复核项见上文。 |

## 8. 第四轮修复

分支 `feat/b2-power-key`，任务基线 `d6db196`。仅修复复审报告 `b2-gemini-review3.md` 中的两项（P2-1、P3-1），未改动冻结文件。

| 清单 ID | 文件与行号 | Host 回归 |
| --- | --- | --- |
| P2-1 | `main/board/PowerLogic.cpp:321-330`（`ShutdownSm::stepCutPower`）：`cutFirstHigh_` 期间若 `cutHigh_` 为低（从 `std::nullopt` 或有 USB 低电平恢复为无 USB），立即置 `cutHigh_ = true` 并以当前 `nowMs` 重置首段计时起点，保证首段满 `kShutdownCutInitHighMs`（2000 ms）后再进入 3.1 s 交替；同时保留 `!usbPresent.has_value()` 路径（A2：未知样本且 U3 已低时 GPIO4 保持低）。 | `tools/main_host_check/PowerLogicCheck.cpp:316-348`（`testCutPowerNulloptRecovery`）：验证进入 `kCutPower` 首拍 `nullopt` 保持低、恢复 `false` 后立即高并持续满 2000 ms 再进入 3.1 s 交替，以及 `true -> nullopt -> false` 序列。 |
| P3-1 | `main/support/ConsoleSupport.cpp:39-47`（`persistsSettings`）：核实 `GnssVerb::kSetRate` 与 `GnssSubVerb::kSetOut` 均在 `GnssModuleCmds.cpp` 写入 NVS 后，将两动词补入 `ModuleKind::kGnss` 分支；`tools/main_host_check/CMakeLists.txt:149` 为 `template_check` 补入 `components/gnss/include`。 | `tools/main_host_check/MainHostCheck.cpp:481-482`（`power_no_reopen`）：补关门后 `GnssSubVerb::kSetOut` 经 `submitRawTo` 被拒断言。 |

### 第四轮资源差值

默认 ESP32 档。bin 使用 `stat -c %s`，DRAM 与 IRAM 使用 `idf.py -B builds/b2fix4-idf size` 并与第三轮（`builds/b2fix3-idf`）及基线（`builds/b2-baseline`）的 map 做 `python -m esp_idf_size --diff` 对照。

| 指标 | 第四轮 `b2fix4-idf` | 相对第三轮 `b2fix3-idf` | 相对基线 `d6db196` |
| --- | ---: | ---: | ---: |
| `rtk_main.bin` | 1,489,056 B | +48 B | +31,584 B |
| DRAM | 97,387 B（78.17%，剩余 27,193 B） | 0 B | +456 B |
| IRAM | 115,435 B（88.07%，剩余 15,637 B） | 0 B | +720 B |

### 第四轮门禁结果

执行命令：`unset IDF_PATH; . $HOME/esp/esp-idf/export.sh >/dev/null; bash tools/ci/run_gates.sh --prefix b2fix4`，退出码 0，**40 passed, 0 failed**（日志位于 `builds/b2fix4-logs/`）。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| G1 冻结校验 | PASS | `sha256sum -c docs/frozen.sha256` 0 改动。 |
| G2 / G2b Host 与 ASan/UBSan | PASS | 13 套各两次，`main_host_check` 每套 103/103，`t13_host_check` 每套 45/45。 |
| G3 / G3b ESP-IDF | PASS | 六档主固件与 ESP32、ESP32-C3 纯组件测试工程均 0 warning 通过。 |
| G5 / G5b / G6 | PASS | 格式合格，clang-tidy 146 个文件 0 条发现，Doxygen 门禁 178 个函数 0 不合格。 |
| G7b / G7c | PASS | 运行期符号和无控制台档 CLI 依赖检查通过。 |
| `idf.py -B builds/b2fix4-idf size`、`esp_idf_size --diff` | PASS | 以最终构建产物统计，资源数据见上表。 |
| 实板 HIL | NOT_RUN | 规则禁止烧录与连串口，待 Architect 实板验证。 |

## 9. Architect 实板 HIL（2026-11-23）

- 板卡：P2 v1.41（`id=1631–1633 mV`），CM680 GNSS，SN `20261121`；固件 `612ff8a`，`idf.py -B builds/b2-hil build`，2 Mbaud 烧录 `/dev/ttyACM0`。
- 串口：USB-TTL 带一键烧录自动复位电路，打开端口时改动 DTR/RTS 会复位（电池供电时会直接掉电）。不改 DTR/RTS 直接打开端口不复位（`uptime` 连续），本次全部观测均用此方式。
- 判据只来自串口日志与命令回读；3V3 电压、GPIO4/U3 波形未用仪表测量。

| # | 场景 | 供电 | 结果 | 证据 |
|---|---|---|---|---|
| 1 | 关机状态插 USB → 充电待机 | USB | PASS | `power_on=charge sense=128 mV usb=1 src=i2c`，`radio skipped (charge standby)`，`u3_on=0`，`charger reg08=0x54` |
| 2 | 待机长按 → 正常开机 | USB | PASS | `power hold ok sense=2593 mV after 29 ms`，蓝牙上线，CM680 115200 探测成功 |
| 3 | 正常运行长按关机（USB 在位） | USB | PASS | `power off reason=key usb=1` → `device powered down`（+44 ms）→ `u3 down with usb, restarting`（+420 ms）→ `power_on=charge` 待机 |
| 4 | 正常运行 `power off`（USB 在位） | USB | PASS | `reason=console` → `device powered down` → 约 2.9 s 后 `u3 down with usb, restarting` → 待机；无 `gpio: conflict found` |
| 5 | 待机拔 USB → 完全关机 | 电池 | PASS（仅串口） | 拔出后命令无响应 |
| 6 | 电池按键开机 | 电池 | PASS | < 1 s 短按不开机（串口无任何输出，含 ROM）；约 1.5 s 单次按压开机，`power_on=key sense=2388–2392 mV usb=0` |
| 7 | 电池长按 2.5 s 关机 | 电池 | PASS（仅串口） | `power off reason=key usb=0` → `device powered down`（+44 ms）→ `E BOD: Brownout detector was triggered`（电源跌落，预期）→ 命令无响应 |
| 8 | 关机前设置保持 | 电池 | PASS | `config set phone.height 000000000000F83F`（1.5）→ 长按关机 → 开机读回 1.5；已恢复原值 `79E9263108ACFE3F`（1.917） |
| 9 | 蓝牙 SPP 连接中长按关机 | 电池 | PASS | `sessions 1 / 2`（PHONE `68:58:A0:EC:C0:7E`）；`power off reason=key usb=0` → `device powered down` → `BT_RFCOMM ... Closed (res: 19)`（`PORT_CLOSED`）；手机侧显示正常断开、无数据 |
| 10 | 固件卡死时 10 s 硬关断 | 电池 | NOT_RUN | 本固件无挂死注入命令 |

- 观察项：
  1. U3 关断耗时与关机触发方式有关：按键路径用户仍在按压，`device powered down` 后约 0.42 s 关断；`power off` 路径由 GPIO4 模拟按压，约 2.9 s 关断。与 ADR-0015 估计的 U3 约 3 s 连续按压关断一致，可据此更新 ADR-0015 §2.4。
  2. 蓝牙场景 RFCOMM 关闭日志在 `device powered down` 之后约 0.49 s 才出现（任务卡要求 `kQuiesce` 先断开 SPP）。手机侧为正常断开，推测为 CLOSE 事件回调滞后；若需严格顺序，应让 `kQuiesce` 等待 SPP CLOSE 事件后再打 `device powered down`。
  3. 开机 `power status` 的 `key_count short=0 long=0`，开机按压不计入按键计数。
  4. 串口复位不会关 U3：正常模式下端口复位后仍以 `power_on=key` 重新进入正常模式。
- 仍未验证：§5 第 1、2、5 项（REG00–REG0B 全量位核对、REG02/REG03 编码、`senseOnMv` 与硬关断时长校准）；第二、三轮待验项中的毛刺/2490 ms 边界注入、U3 30 s 未掉电回退、BT/BTC 拥塞、Web exit 并发、NVS 错误注入、日志交付时差、`sys reboot` 并发。
