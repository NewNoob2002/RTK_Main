# OL1 实施报告：P2 OLED（SH1106）驱动 + WouoUI 只读状态页

- **任务卡**：`docs/tasks/OL1-oled.md`
- **分支 / 基线**：`feat/ol1-oled`（基线 `3bbd6d2`）
- **Profile 与依据**：`esp-idf`（`sdkconfig.defaults`、`idf_component_register`），C++ 等级 T2
- **并行边界确认**：未修改 `components/gnss/`、`main/modules/gnss/`、`main/settings/`、`main/app/SettingsBackend.*` 及任何冻结文件（`G1-frozen` PASS）。
- **未执行动作确认**：未提交 git、未烧录、未连接串口。

---

## 1. 改动清单

### 新增文件
- `docs/adr/ADR-0020-oled-display.md` — ADR 草稿（状态 `Proposed`），覆盖硬件事实、I2C0 总线共享、模块与任务调度、页面清单、按键/关机时序、MPL-2.0 合规、资源预算与控制台档位。
- `components/third_party/CMakeLists.txt` — 第三方组件注册（双模支持 ESP-IDF 与主机静态库 `third_party_wououi`），仅编译 `WouoUI_graph.c` 与 `WouoUI_font.c`。
- `components/third_party/wououi/{LICENSE,README.md,WouoUI_common.h,WouoUI_conf.h,WouoUI_font.h,WouoUI_font.c,WouoUI_graph.h,WouoUI_graph.c}` — WouoUI-PageVersion（MPL-2.0）最小图形与字体子集。
- `main/board/I2cBus.hpp`、`main/board/I2cBus.cpp` — 板级 I2C0 共享总线单一所有者（幂等建总线一次 + 静态 FreeRTOS 互斥锁 `BusLock` + 探测/挂载/卸载设备/总线复位/总线扫描；控制块全零落 `.bss`）。
- `main/board/Sh1106.hpp`、`main/board/Sh1106.cpp` — SH1106 128×64 I2C 驱动（400 kHz、列偏移 2、P2 180° 旋转、GPIO12 电源控制与 100 ms 上电延时、按 8 页独立加解锁增量写出、8 ms 传输超时 + 早停 + 总线复位 + 连续失败退避、无应答一行 W 禁用、初始化失败回收设备句柄、关机断电与唤醒失败拉低 GPIO12 保护）。
- `main/def/OledDef.hpp` — `OledPage`、`OledStatus` 与无锁原子 `UiEventBus`（解耦 `PowerModule` 与 `OledModule`）。
- `main/modules/oled/OledUi.hpp`、`main/modules/oled/OledUi.cpp` — 纯逻辑（无 IDF/FreeRTOS 头）：只读视图模型、纯整数格式化与 ASCII 清洗截断、`PageSm` 状态机（含 `kCutPower` 精确剩余等待与唤醒失败有界退避重试）、`FrameTracker` 8 页 64-bit FNV-1a 脏页判定、WouoUI 页面渲染（开机/状态/设备信息/充电待机/关机；渲染结束清零 `p_cur_pen`）。
- `main/modules/oled/OledModule.hpp`、`main/modules/oled/OledModule.cpp` — OLED 模块与低优先级静态任务（Core 1、prio 1、栈 3072 B 与 1 KB 帧缓冲由 `BootArena` 仅在含 `Feature::kOled` 板型上外供、≤ 5 Hz、`WouoUI_GraphSetSendBuffFun` HAL 桥接）。
- `main/modules/oled/OledCmds.hpp`、`main/modules/oled/OledCmds.cpp` — DEV 档 `oled [status]` 诊断命令。
- `tools/main_host_check/OledUiCheck.cpp`、`tools/main_host_check/OledDriverCheck.cpp` — 主机单元测试（UI 格式化/状态机/脏页刷新/事件总线，以及共享 I2C0/SH1106 驱动替身与故障恢复测试）。
- `docs/reports/OL1-oled.md` — 本报告。

### 修改文件
- `main/board/BoardProfile.hpp`、`main/board/BoardProfile.cpp` — 增加 `OledPins`、`hasFeature()`，登记 P2 v1.41 OLED 引脚（I2C0 SDA21/SCL22、`0x3C`、电源 `GPIO12`、`rotated180=true`）。
- `main/board/Charger.cpp` — 改用共享 `board::i2c` 所有者与 `i2c::BusLock`，完整保留 B3 探测、REG02/REG03 校验、0-tick 非阻塞 `refreshInputStatus` 与告警文案。
- `main/data/AppData.hpp`、`main/data/AppData.cpp` — 增加 `oledStatus` 槽与 `uiEvents` 轻量事件总线。
- `main/modules/power/PowerModule.cpp` — 在 `KeyEvent::kShort` 通知 `uiEvents.notifyShortKey()`，在 `publishSnapshot()` 写完 `powerStatus` 后对 `shutdownStage` 变化发出 `uiEvents.notifyWake()`；不直接画屏，不改动关机时序。
- `main/app/BootReasons.hpp`、`main/app/BootBuffers.hpp`、`main/app/BootBuffers.cpp` — 按 ADR-0011 §7 将 `task.oled`（3072 B）与 `oled.fb`（1024 B）移入 `BootArena` 并仅在 `Feature::kOled` 板型上分配、绑定与回滚（`bind:oled`）。
- `main/modules/system/SystemCmds.cpp` — 在 `sys tasks` 输出中新增 `"oled"` 任务行（便于 HIL 实测 `stack_hwm`）。
- `main/AppConfig.hpp`、`main/AppContext.hpp`、`main/app/App.cpp`、`main/app/BootFlow.cpp`、`main/module_list.inc`、`main/CMakeLists.txt` — 注册 `OledModule`（`kAlways`）、`BootTask::kOled` 启动屏障与组件依赖。
- `main/app/ConsoleRegistration.hpp`、`main/app/ConsoleSetup.cpp` — 按 ADR-0016 在 DEV 档注册 `oled` 命令组（PRODUCTION 与 NONE 不登记）。
- `tools/ci/run_gates.sh`、`tools/doc_check/doc_check.py`、`tools/tidy_check/tidy_check.py` — 仅将显式第三方上游目录 `components/third_party/wououi/` 排除出格式/Doxygen/clang-tidy 门禁（保留 0-warning 构建与 G7b 运行时/堆门禁）。
- `tools/main_host_check/{CMakeLists.txt,BoardProfileCheck.cpp,BootAllocCheck.cpp,ChargerCheck.cpp,ConsoleProfileCheck.cpp,MainHostCheck.cpp,RadioModeCheck.cpp}` — 更新主机回归并接入新测试目标与跨审回归用例。

---

## 2. 设计取舍

1. **WouoUI 最小子集集成**：OL1 仅含只读状态页，无设置菜单与弹窗动画；因此只编译 `WouoUI_graph.c` 与 `WouoUI_font.c`（总体积 3,532 B），未引入 `WouoUI.c`/`WouoUI_page.c`/`WouoUI_win.c`（会拖入浮点 `sprintf` 与 ~1.6 KB 静态状态）。同时通过 `WouoUI_GraphSetSendBuffFun(&sendBufferHal)` + `WouoUI_BuffSend()` 完整接通 WouoUI HAL，后续 OL2 菜单卡可在同一目录下增量启用页面框架。
2. **第三方目录位置与冻结兼容**：根 `CMakeLists.txt` 为冻结文件，无法追加 `EXTRA_COMPONENT_DIRS`；通过 `components/third_party/CMakeLists.txt` + `components/third_party/wououi/` 实现既符合任务卡路径约定，又由 ESP-IDF 自动发现组件，且自动纳入 `g7b_check.py` 扫描验证零堆/零异常。
3. **单份 1 KB 帧缓冲 + 64-bit 页摘要**：WouoUI 内建 `HARDWARE_DYNAMIC_REFRESH` 需额外 1 KB 对比缓冲；本实现改为每页 64-bit FNV-1a 摘要（共 72 B 状态），静止画面 0 I2C 写出，局部更新仅写脏页（单页 I2C 传输失败时不提交该页摘要，下轮自动重试）。
4. **按页加解锁避免阻塞充电与按键**：SH1106 400 kHz 写单页 128 B 约 3 ms，每页独立获取/释放 `i2c::BusLock`（带优先级继承的静态互斥锁）；`Charger::refreshInputStatus` 保持 0-tick 非阻塞尝试加锁，确保 `kLink`（prio 10）与按键采样绝不被 `oled` 任务（Core 1, prio 1）阻塞。
5. **零延长关机时序**：`PowerModule` 的四阶段状态机与超时常量一字未改。进入 `kAnnounce` 立即唤醒 `oled` 绘出关机提示（约 25 ms）；进入 `kCutPower` 后在提示已展示 ≥ 200 ms 时关闭面板（`0xAE` / `0xAD 0x8A`）并拉低 `GPIO12`（远早于无 USB 首段 2000 ms 断电脉冲）；若 30 s 超时回退至 `kIdle`，OLED 自动重新上电恢复。
6. **SH1106 方向与硬件依据**：依据旧固件 `P2_V3.1_IDF/main/src/xyz_oled.c:13-25,384`（`OLED_DisplayTurn(1)`），P2 v1.41 面板倒装 180°（`0xA0`/`0xC0`），已通过 `BoardProfile::oled.rotated180 = true` 参数化配置。

---

## 3. MPL-2.0 合规清单

| 项 | 状态 / 内容 |
| --- | --- |
| 上游仓库与版本 | `https://github.com/Sheep118/WouoUI-PageVersion`，commit `82b985a` |
| 许可证 | Mozilla Public License Version 2.0，完整文本保存于 `components/third_party/wououi/LICENSE` |
| 独立隔离目录 | `components/third_party/wououi/`（与自有代码目录完全隔离） |
| 未修改上游文件 | `WouoUI_common.h`、`WouoUI_font.h`、`WouoUI_font.c`、`WouoUI_graph.h`、`WouoUI_graph.c`（原样保留） |
| 修改文件与标注 | 仅 `WouoUI_conf.h` 两处修改，均带 `/* RTK_main modification (MPL-2.0): ... */` 注释：① 禁用 `WOUOUI_LOG_ENABLE`（移除 `printf`）；② `SOFTWARE_DYNAMIC_REFRESH` 置 `0`（改由驱动页摘要去重） |
| 零堆核实 | `rg` 扫描上游 `Csource/` 0 处 `malloc/calloc/realloc/free`；`G7b-runtime` 扫描 `third_party` 目标文件确认 0 堆符号、0 异常、0 RTTI、0 `__cxa_guard` |

---

## 4. 跨审问题修复表（`docs/reports/OL1-review.md`）

| 编号 | 级别 | 根因摘要 | 修复方案 | 回归验证 |
| --- | --- | --- | --- | --- |
| `ROL1-01` | major | `wakeFromPowerOff()` 中 `configurePanel()` 失败未拉低 `GPIO12`，且 `PageSm` 仅发一次 `shouldWake` 导致永久黑屏耗电 | `Sh1106::wakeFromPowerOff()` 失败前强制 `drivePowerPin(false)`；`PageSm` 新增 `onWakeFailed(nowMs)` 与 1000 ms 有界退避重试，`OledModule::refreshOnce()` 失败时回调通知 | `oled_driver_wake_fail_recovers`、`OledUiCheck::testPageStateMachine`（normal + ASan） |
| `ROL1-02` | major | `writePage()` 双传输持锁上限 30 ms > 互斥等待 20 ms；单页失败继续写剩余页形成风暴；超时无总线复位 | `kI2cXferTimeoutMs` 降为 8 ms（`static_assert(2*8 < 20)`）；新增 `i2c::resetBus()` 并在释锁后对 `kTimeout` 触发复位；`flush()` 遇错立即早停并按连续失败次数指数退避（最多 4 轮） | `oled_driver_page_error_retry`、`oled_driver_timeout_abort_reset_backoff`（normal + ASan） |
| `ROL1-03` | major | `PowerModule` 在 `stepShutdown()` 内提前调用 `uiEvents.notifyWake()`，早于 `publishSnapshot()` 写入新 `shutdownStage` | 移除 `handleShutdownStageEntry/CutPower` 内 4 处提前唤醒，改为 `publishSnapshot()` 完成 `powerStatus.write(next)` 后对 `shutdownStage` 变化触发 `notifyWake()` | `main_{concurrent,preempt}_power_short_key_ui` 在唤醒回调瞬间断言 `powerStatus.shutdownStage` 已为 `kAnnounce`/`kCutPower` |
| `ROL1-04` | major | `s_oled_stack` + `s_oled_fb`（4 KB）无条件占 `.bss` 绕过 `BootArena`；`s_panel`/`s_bus` 因 `-1` 默认值落 `.dram0.data`（+336 B） | `task.oled`（3072 B）与 `oled.fb`（1024 B）移入 `allocateBootBuffers()` 并仅在 `Feature::kOled` 板型分配/绑定/回滚；`Sh1106`、`i2c::BusState`、`s_last_flush` 全部全零初值落 `.dram0.bss` | `boot_alloc_{on,gnss_off,trace_off,all_off}`（`testOledBootAllocationPlan` 验证 E1=0 分配、P2 正常/待机分配、故障注入与 `bind:oled` 回滚）+ ELF 符号/节核对（静态 DRAM 增量 `+4,672 B → +592 B`） |
| `ROL1-05` | minor | `kCutPower` 期间固定休眠 200 ms，可能错过 USB 快速关机重启前干净 `powerOff()` | `PageDecision` 新增 `nextWaitMs`，在 `kCutPower && !poweredOff_` 返回精确剩余毫秒数，`OledModule::run()` 按 `min(nextWaitMs, 200)` 等待 | `OledUiCheck::testPageStateMachine` 断言 `1060 ms → 140 ms`、`1150 ms → 50 ms`、`1200 ms → shouldPowerOff` |
| `ROL1-06` | minor | `Sh1106::init()` 配置失败未卸载 `dev_`；`renderPage()` 留下栈局部 `p_cur_pen` 悬垂指针；`sys tasks` 缺少 `"oled"` 行 | 新增 `i2c::removeDevice()` 并在 `init()` 配置失败时卸载 `dev_`；`renderPage()` 末尾调用 `WouoUI_GraphSetPen(nullptr)`；`cmdSysTasks()` 增加 `printTaskRow("oled", xTaskGetHandle("oled"))` | `oled_driver_init_cfg_fail_cleanup`、`OledUiCheck`（ASan）、`G3-idf`/`G5b-tidy` + HIL 步骤验证 `sys tasks` |
| `ROL1-07` | minor | `run_gates.sh`、`doc_check.py`、`tidy_check.py` 对 `third_party` 目录段排除过宽 | 统一收窄为显式上游前缀 `components/third_party/wououi/` | Python 单测断言 `wououi/` 豁免、`components/third_party/custom/` 与 `main/third_party/` 不豁免；`G5-format`/`G6-doc_check`/`G5b-tidy` 全部 PASS |

---

## 5. 测试映射

| 验收项 / 场景 | 测试用例 | 结果 |
| --- | --- | --- |
| 页面格式化（数字、单位、截断、ASCII 清洗、无 GNSS/无充电/无电池占位） | `OledUiCheck::testFormatting`（normal + ASan/UBSan） | PASS |
| 翻页与关机状态机（开机 1.5 s、短按跳过/循环、uint32 回绕、充电待机锁页、`kCutPower` 精确等待、30 s 回退与唤醒失败 1 s 退避重试） | `OledUiCheck::testPageStateMachine`（normal + ASan/UBSan） | PASS |
| WouoUI 渲染非空像素（含清零 `p_cur_pen`）与“内容未变不刷新”/局部脏页/失败页自动重试 | `OledUiCheck::testRenderAndDirtySkip`（normal + ASan/UBSan） | PASS |
| 轻量 UI 事件总线（短按计数、唤醒钩子注册与解绑） | `OledUiCheck::testUiEventBus`（normal + ASan/UBSan） | PASS |
| `PowerModule` 短按经 `UiEventBus` 通知 UI，且关机唤醒严格发生在 `powerStatus` 发布之后 | `main_{concurrent,preempt}_power_short_key_ui`（normal + ASan/UBSan） | PASS |
| `BootArena` 按 `Feature::kOled` 门控分配 `task.oled` + `oled.fb`（E1=0 分配、P2 正常/待机分配、故障注入与 `bind:oled` 回滚） | `boot_alloc_{on,gnss_off,trace_off,all_off}`（normal + ASan/UBSan） | PASS |
| 共享 I2C0 总线（仅建一次总线、Charger 100 kHz + SH1106 400 kHz 共存、列偏移 2、180° 旋转、按页加解锁、空闲 0 写、关机 `0xAE/0xAD/0x8A` + GPIO12 拉低、超时恢复） | `oled_driver_shared_both_online`（normal + ASan/UBSan） | PASS |
| OLED 无应答只报一行 W、拉低 GPIO12 并禁用显示，不影响 Charger | `oled_driver_oled_absent_charger_ok`（normal + ASan/UBSan） | PASS |
| Charger 离线（报一行 W），OLED 共用 I2C0 正常工作 | `oled_driver_charger_absent_oled_ok`（normal + ASan/UBSan） | PASS |
| 单页超时早停、总线复位、下次仅补写未提交页；连续超时退避零总线风暴 | `oled_driver_page_error_retry`、`oled_driver_timeout_abort_reset_backoff`（normal + ASan/UBSan） | PASS |
| `wakeFromPowerOff()` 失败拉低 GPIO12 保持断电态并可后续恢复；`init()` 配置失败卸载 `dev_` 无泄漏 | `oled_driver_wake_fail_recovers`、`oled_driver_init_cfg_fail_cleanup`（normal + ASan/UBSan） | PASS |
| 既有 B3 Charger（8 场景）与 Power（全部场景）回归 | `charger_*`、`main_*_power_*`、`power_logic_check`、`board_profile_check` | PASS |
| ADR-0016 控制台档位（DEV 含 `oled`，PRODUCTION/NONE 不含 `oled`） | `console_profile_check*`、`G7c-no-cli-deps` | PASS |
| 模块启用矩阵（Normal BT/Wi-Fi 与 ChargeStandby 均含 `oled`） | `radio_mode_check` | PASS |

---

## 6. 全量门禁结果（`ol2` 修复复验）

执行命令：`ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol2 --base 3bbd6d2`
日志目录：`builds/ol2-logs/`

| 门禁 | 结论 | 备注 |
| --- | --- | --- |
| `G1-frozen` | PASS | 全部冻结文件校验通过 |
| `G2-*` / `G2b-*-asan`（全部 14 个主机测试套件 + `gnss-all`） | PASS | `main_host_check` 132/132（normal + ASan）、`t13_host_check-asan` 45/45 均通过 |
| `G5-format` | PASS | `clang-format --style=file --dry-run --Werror` 0 差异（仅豁免 `components/third_party/wououi/`） |
| `G6-doc_check` | PASS | 改动函数 97 个，不合格 0 个 |
| `G3-idf`（default / trace-off / wifi-off / ota-off / production / console-none / none-wifi） | PASS | 7 变体全部 0 warning |
| `G3b-rtk_pure-esp32` / `esp32c3` | PASS | 双架构 0 warning |
| `G5b-tidy` | PASS | 156 文件，0 条发现 |
| `G7b-runtime` | PASS | 全部目标文件（含 `third_party`）0 堆、0 异常、0 RTTI、0 `__cxa_guard`；ELF 白名单 6/6 |
| `G7c-no-cli-deps` | PASS | NONE+Wi-Fi 构建零 CLI/REPL 符号残留 |
| 汇总 | **43 passed, 0 failed** | — |

---

## 7. 体积与内存对比（相对干净基线 `3bbd6d2`）

| 指标 | 基线 (`builds/ol1-base`) | OL1 初版 (`builds/ol1-idf`) | OL2 修复后 (`builds/ol2-idf`) | OL2 相对基线差值 |
| --- | ---: | ---: | ---: | ---: |
| `rtk_main.bin` | 1,528,272 B | 1,540,992 B | 1,541,744 B | **+13,472 B** (+13.2 KB，分区剩余 22%) |
| Flash Code (`.text`) | 1,094,648 B | 1,103,284 B | 1,104,252 B | +9,604 B |
| Flash Data (`.rodata` 等) | 265,324 B | 268,780 B | 268,892 B | +3,568 B |
| IRAM | 116,139 B (88.61%, 余 14,933 B) | 116,139 B | 116,139 B (88.61%, 余 14,933 B) | **+0 B** |
| DRAM 静态合计 | 96,763 B (77.67%, 余 27,817 B) | 101,435 B (81.42%) | **97,355 B (78.15%, 余 27,225 B)** | **+592 B**（`.bss` +304 B，`.data` +288 B；较 OL1 初版节省 **4,080 B** 静态 DRAM） |
| `BootArena` 动态外供（仅 `Feature::kOled`） | 0 B | 0 B | 4,096 B（`task.oled` 3,072 B + `oled.fb` 1,024 B） | E1 / console-only 降级为 **0 B**；P2 纳入 `sys mem` 台账 |
| 启动后最小空闲堆 | — | `NOT_RUN` | `NOT_RUN`（需实板运行 `sys mem` 实测） | P2 含 `BootArena` 4 KB + IDF `add_device` 一次性小块；E1 仅静态 +592 B |

### WouoUI 与 OLED 模块体积单列（`esp_idf_size --archives / --files`，`builds/ol2-idf`）

| 归档 / 目标文件 | Flash Code | Flash Data (`.rodata`) | DRAM (`.bss`/`.data`) | IRAM | 合计 |
| --- | ---: | ---: | ---: | ---: | ---: |
| **`libthird_party.a`（WouoUI 合计）** | **1,406 B** | **2,114 B** | **12 B (`.bss`)** | **0 B** | **3,532 B** |
| ├─ `WouoUI_graph.c.obj` | 1,406 B | 0 B | 12 B (`.bss`) | 0 B | 1,418 B |
| └─ `WouoUI_font.c.obj`（仅 `F6x8` 570 B + `F8X16` 1,520 B） | 0 B | 2,114 B | 0 B | 0 B | 2,114 B |
| `OledUi.cpp.obj` | 3,595 B | 0 B | 0 B | 0 B | 3,595 B |
| `OledModule.cpp.obj`（`s_panel` 240 B + `s_page_sm` 24 B 等，全落 `.bss`） | 1,519 B | 72 B | 274 B (`.bss` 274 / `.data` 0) | 0 B | 1,865 B |
| `Sh1106.cpp.obj` | 1,460 B | 4 B | 0 B | 0 B | 1,464 B |
| `I2cBus.cpp.obj`（`s_bus` 104 B，全落 `.bss`） | 853 B | 0 B | 104 B (`.bss` 104 / `.data` 0) | 0 B | 957 B |
| `OledCmds.cpp.obj` | 366 B | 40 B | 0 B | 0 B | 406 B |

ELF 符号核对（`xtensa-esp32-elf-objdump -t builds/ol2-idf/rtk_main.elf`）：`s_panel`（240 B）、`s_bus`（104 B）、`s_page_sm`（24 B）、`s_active_fb`（4 B）、`s_oled_task_handle`（4 B）、`s_last_flush`（1 B）全部位于 `.dram0.bss`；`.dram0.data` 仅增 +288 B（来自 `AppContext` 内 `OledModule`/`BootTask` TCB + `OledStatus` 槽 + `UiEventBus` + 控制台命令槽）。

---

## 8. HIL 验收步骤（`NOT_RUN`，待 Architect 在 P2 v1.41 实板执行）

1. **冷开机与开机画面**：在 P2 v1.41（无 USB）长按电源键开机；确认串口日志出现 `<oled> SH1106 ready at 0x3C (400 kHz, pwr GPIO12)`，屏幕正向显示 `P2` 与 `V3.1.0`（无上下颠倒、无列偏移 2 像素伪影），约 1.5 s 后自动切至状态页。
2. **状态页与实时更新**：观察顶栏射频（`BT:WAIT` → 手机连上变为 `BT:CONN`）与电量百分比；待 GNSS 定位后确认 `GNSS: RUN`、`FIX` 与 `SAT` 实时刷新。
3. **短按翻页与空闲不刷新**：
   - 短按电源键（< 1 s）：确认页面在状态页（`1/2`）与设备信息页（`2/2`，显示 `DEV`/`SN`/`HW: V1.41`/`FW: V3.1.0`）间切换。
   - 切至设备信息页静置 10 s 后执行控制台 `oled`：确认 `present=yes`、`page=device`、`frames skipped` 持续增长而 `frames flushed` 不再增长、`i2c_errors=0`。
4. **I2C 共享、栈水位与内存台账**：执行 `power status`、`power i2c scan`（确认同时扫出 `0x3C` 及板载充电芯片地址如 `0x1A`），若有 `0x1A` 执行 `power charger` 确认寄存器读出正常且按键响应无卡顿；执行 `sys tasks` 记录 `"oled"` 的 `stack_hwm`（评估 3,072 B 栈余量）；执行 `sys mem` 确认 `BootArena` 台账含 `task.oled`（3072 B）与 `oled.fb`（1024 B）且 `min` 空闲堆 ≥ 40 KB。
5. **Wi-Fi 模式显示**：执行 `radio wifi` 重启进入配置模式，确认状态页顶栏显示 `WIFI:AP`；执行 `radio bt` 返回蓝牙模式。
6. **关机提示与断电**：长按电源键 ≥ 2.5 s（或 `power off`，分别测无 USB 与插 USB 两种场景）；确认屏幕显示 `Shutting down...`，随后在整机断电/USB 关机重启前屏幕熄灭（测量或日志确认 `GPIO12` 拉低且现有关机总时长符合 ADR-0015）。
7. **充电待机显示**：关机状态下插 USB 启动进入 `charge_standby`；确认仅显示 `CHARGING`/`BATTERY FULL`/`USB STANDBY` 与电量/电压进度条，短按不切至状态/设备页，长按 2.5 s 正常重启开机。
8. **无屏 / E1 回归（可选）**：在 E1 DevKit 或断开 OLED 排线的 P2 板上开机，确认 E1 不触碰 GPIO12、`sys mem` 的 `BootArena` 中无 `task.oled`/`oled.fb`、`sys tasks` 显示 `oled: not running`、`oled` 显示 `enabled=no present=no`；断屏 P2 仅报一行 `<oled> no ack at 0x3C, display disabled`，其余功能与充电检测完全正常。
