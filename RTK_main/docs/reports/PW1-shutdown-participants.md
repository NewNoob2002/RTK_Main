# PW1 实施报告：关机收尾参与者表、低电/保存超时与 OL3 终帧边缘修复

- **分支 / 基线**：`feat/pw1-shutdown`，对比与 `doc_check` 基线 `main 6b54fc4`（任务卡提交 `2f97736`）
- **任务卡**：`docs/tasks/PW1-shutdown-participants.md`（完整实施 A / B / C / D 与低电 HIL 测试支持）
- **边界确认**：未修改 `docs/protocol/`、`docs/web/` 及任何冻结文件（`G1-frozen` PASS）；未改变产品默认值；未执行烧录、串口或硬件操作。

---

## 1. 改动清单

### A. 静态关机收尾参与者登记表与 OLED 进度派生
- `main/modules/power/PowerModule.hpp`、`main/modules/power/PowerModule.cpp`：
  - 新增静态参与者契约 `power::ShutdownParticipant{name, begin, isDone, userCtx, stage, milestone}` 与启动期一次性静态槽位注册 API（`registerShutdownParticipant` / `clearExtraShutdownParticipants` / `extraShutdownParticipants`，上限 `kMaxExtraShutdownParticipants = 4`，编译期 `static_assert` 约束总数 ≤ 16，无运行期堆）。
  - 将既有全部硬编码关机检查迁移为编译期内置参与者表 `PowerModule::ParticipantOps::kBuiltin`（11 项，落 `.rodata`）：
    - `kSave`：`storage_fence`、`gnss_fence`、`storage_flush`（待落盘队列 + NVS 在途写）、`bt_settings`、`web_settings`、`gnss_nvs`。
    - `kQuiesce`：`radio_pause`（BT/Wi-Fi 无线静默）、`safe_gpio`（外设安全输出电平）、`log_drain`（异步日志 + UART 排空）。
    - `kCutPower`：`oled_final_bat`（电池终帧停留）、`oled_final_usb`（USB RESTARTING 终帧停留）。
  - `PowerModule` 阶段入口（`beginStageParticipants`）、完成轮询（`pollStageParticipants`）、超时记录（`recordStageTimeouts`）与超时日志（`logStageTimeout`）统一遍历参与者表，超时日志按 `pending=[name1,name2,...]` 列出所有未完成参与者名。
  - `PowerModule::computeShutdownCounts` 与 `computeShutdownDoneMask` / `computeShutdownDegradedMask` 均由参与者表完成/超时状态掩码派生，不再维护分散硬编码标志。
- `main/def/SystemDef.hpp`、`main/modules/oled/OledUi.hpp`、`main/modules/oled/OledUi.cpp`、`main/modules/oled/OledModule.cpp`、`main/modules/oled/OledRender.cpp`：
  - `PowerSnapshot` 与 `oled::ViewModel` 新增 `shutdownDoneCount`、`shutdownTotalCount`（uint8_t × 2）。
  - `oled::shutdownTargetPercent` 改为优先按参与者表 `doneCount / totalCount` 在 `[15%, 92%]` 线性映射，终帧就绪（`kUsbOffReady` / `kBatteryOffReady`）直接到 `100%`；`totalCount == 0` 保留阶段回退兼容，界面表现不回退。

### B. 超时与低电阈值
- `main/board/PowerLogic.hpp`、`main/board/PowerLogic.cpp`：
  - `kShutdownSaveTimeoutMs` 由 `1000U` 调整为 `3000U`（注释说明 NVS 大 blob 多页写入及 SPIFFS 同步余量）；`kShutdownCutTimeoutMs = 30000U`（30 s 切电兜底）保持不变。
  - 导出 `kLowBatteryPct = 10U`、`kLowBatteryHoldMs = 120000U`（由 30 s 调整为 120 s），`LowBatteryTimer::isDue` 支持传入自定义 `holdMs`（默认 `kLowBatteryHoldMs`）；预留硬件过放保护电压即时关机 `TODO`（明确标注需先实测 CM680 / F9P 欠压阈值，当前保持关闭）。
- `main/Kconfig.projbuild`、`main/modules/power/PowerCmds.cpp`、`main/modules/power/PowerModule.cpp`：
  - HIL 低电测试支持（不改变产品默认值）：
    - Kconfig 新增 `CONFIG_RTK_LOW_BATTERY_HOLD_MS`（默认 `120000`，范围 `1000..300000`），允许测试构建缩短台架等待时长。
    - DEV 档控制台新增 `power simbat <0-100|off>`，经无锁原子 `power::setSimulatedBatteryPct` 注入模拟电量；清除后立即复位低电计时并恢复真实 ADC 采集。

### C. 关机来源扩展与文案补齐
- `main/board/PowerLogic.hpp`、`main/board/PowerLogic.cpp`：新增预留枚举项 `ShutdownReason::kWeb`（无调用点，`shutdownReasonName` 返回 `"web"`）。
- `main/modules/debug/TraceText.hpp`、`main/modules/debug/TraceText.cpp`、`main/modules/debug/TraceTextValues.cpp`：新增 `TraceValue::kShutdownStageA`、`kShutdownReasonA`、`kShutdownReasonB`，使 `kShutdownStage` 与 `kShutdownAbort` trace 句子按名称覆盖全部 5 种 `ShutdownReason`（`key` / `console` / `low_battery` / `phone` / `web`）。
- `main/modules/oled/OledUi.cpp`：`shutdownReasonText` 覆盖 `ShutdownReason::kWeb`（显示 `"WEB"`）。

### D. OL3 终帧边缘修复与 OL4 保持
- `main/modules/power/PowerModule.hpp`、`main/modules/power/PowerModule.cpp`：
  - 将原共用 `finalFrameReady_` / `finalHoldDeadlineMs_` 拆分为独立的 `FinalFrameState batteryFinalFrame_` 与 `usbFinalFrame_`；电池断电终帧停留期间插入 USB 转入重启时，不再复用电池终帧标志跳过 USB `RESTARTING` 终帧与 300 ms 停留。
  - OL4 2 项长按确认弹窗与 8 s 强制关机兜底（`kForcePowerOffHoldMs = 8000U`）保持不变。

### 文档与主机测试
- `docs/adr/ADR-0015-board-profiles.md`（§2.4）、`docs/adr/ADR-0020-oled-display.md`（§5.3）：同步 3 s 保存超时、120 s 低电保持、过放保护待实测说明、静态参与者登记表、`ShutdownReason::kWeb`、HIL `power simbat` / `RTK_LOW_BATTERY_HOLD_MS` 与电池/USB 终帧拆分状态。
- `tools/main_host_check/PowerLogicCheck.cpp`、`OledUiCheck.cpp`、`MainHostCheck.cpp`、`CMakeLists.txt`：
  - 更新既有测试以匹配 3 s 保存超时与 120 s 低电保持。
  - 新增 `power_participant_blocks_and_timeout_names`（参与者阻塞断电直到超时、kSave/kQuiesce 超时日志列出参与者名、非法/重名注册拒绝、全部 5 种 `ShutdownReason` trace 格式化）、`power_oled_battery_to_usb_final_frame`（电池终帧停留期间插 USB 仍完整等待 RESTARTING 终帧与 300 ms 停留）、`power_sim_battery_low_shutdown`（模拟低电注入、清除恢复真实 ADC 与 120 s 触发关机），并在 `OledUiCheck` 中补齐参与者计数进度派生与 `kWeb` OLED 渲染断言。

---

## 2. DRAM / IRAM / Flash 与基线对比

对比条件：同一 `sdkconfig.defaults`（ESP-IDF v6.1，`esp32` 默认构建），基线为 `git archive 6b54fc4` 干净构建，目标为 `builds/pw1-idf`。

### 整机镜像对比

| 指标 | 基线 `6b54fc4` | PW1 `builds/pw1-idf` | 增量 | 约束 |
| --- | ---: | ---: | ---: | --- |
| `rtk_main.bin` | 1,582,304 B | 1,585,248 B | **+2,944 B** | 应用分区余 19%（`0x5cfa0` B） |
| Flash Code (`.text`) | 1,138,556 B | 1,140,856 B | **+2,300 B** | — |
| Flash Data (`.rodata`/`.appdesc`/`.tdata`) | 274,556 B | 275,100 B | **+544 B** | 内置参与者表入 `.rodata` |
| IRAM | 116,139 B | 116,139 B | **0 B** | **0 B（满足）** |
| 静态 DRAM 合计 | 98,475 B | 98,595 B | **+120 B** | **≤ 256 B（满足）** |
| — DRAM `.data` | 52,891 B | 53,003 B | +112 B | 4 槽额外参与者数组 + 模拟电量原子 + 新增快照/模块状态 |
| — DRAM `.bss` | 45,584 B | 45,592 B | +8 B | 额外参与者计数等 |
| RTC SLOW | 68 B | 68 B | **0 B** | — |
| 运行期堆分配 | 0 B | 0 B | **0 B** | **零运行期堆（满足）** |

### 主要改动目标文件明细（`builds/pw1-idf`）

| 目标文件 | Flash Code | Flash Data | 静态 DRAM | IRAM |
| --- | ---: | ---: | ---: | ---: |
| `PowerModule.cpp.obj` | 8,711 B | 220 B | 89 B（`.data` 8 B / `.bss` 81 B） | 0 B |
| `PowerLogic.cpp.obj` | 1,678 B | 44 B | 0 B | 0 B |
| `PowerCmds.cpp.obj` | 1,068 B | 120 B | 0 B | 0 B |
| `OledRender.cpp.obj` | 13,457 B | 499 B | 0 B | 0 B |
| `OledUi.cpp.obj` | 7,774 B | 32 B | 0 B | 0 B |
| `OledModule.cpp.obj` | 6,950 B | 84 B | 762 B | 0 B |
| `TraceText.cpp.obj` | 664 B | 2,448 B | 0 B | 0 B |
| `TraceTextValues.cpp.obj` | 930 B | 0 B | 0 B | 0 B |

---

## 3. 门禁与测试结果

在 `RTK_main` 下执行 `IDF_PATH=/home/gtc/esp/esp-idf-v6.1 PATH="/home/gtc/.espressif/python_env/idf6.1_py3.13_env/bin:$PATH" bash tools/ci/run_gates.sh --prefix pw1 --base 6b54fc4`，**43 passed, 0 failed**（日志目录 `RTK_main/builds/pw1-logs/`）：

| 门禁 | 结果 | 详情 |
| --- | --- | --- |
| `G1-frozen` | **PASS** | `sha256sum -c docs/frozen.sha256` 全部匹配 |
| `G2-main_host_check` / `G2b-main_host_check-asan` | **PASS** | 189/189 通过（含新增 3 组 concurrent+preempt 场景与 OledUi/PowerLogic 新断言） |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | **PASS** | 46/46 通过 |
| 其余 12 组 `G2-*` / `G2b-*-asan`（host / app_fw / services / stream_demux / bt_reconnect / cli / gnss / gnss-all / nvs_store / web_proto / settings / ota） | **PASS** | 全部普通 + ASan/UBSan 通过 |
| `G5-format` | **PASS** | `clang-format --style=file --dry-run --Werror` 零差异 |
| `G6-doc_check` | **PASS** | `改动函数 56 个，不合格 0 个`（基线 `6b54fc4`） |
| `G3-idf`（default / trace-off / wifi-off / ota-off / production / console-none / none-wifi）与 `G3b-rtk_pure`（esp32 / esp32c3） | **PASS** | 9 档全过，0 warning |
| `G5b-tidy` | **PASS** | 159 个文件，0 条告警 |
| `G7b-runtime` | **PASS** | 无禁止异常/RTTI 符号 |
| `G7c-no-cli-deps` | **PASS** | `builds/pw1-none-wifi` 零 CLI/REPL 依赖 |
| 实板 HIL（P2 v1.41） | **NOT_RUN** | 未连接/操作实板，待按 §4 执行 |

---

## 4. HIL 待测步骤与 `NOT_RUN` 项

以下项目当前为 **`NOT_RUN`**，待实板（P2 v1.41，DEV 档固件 `builds/pw1-idf`）按步骤验收：

1. **常规按键 / 控制台 / 手机关机与 OLED 进度条（PW1 A/D）**：
   - 无 USB 下长按电源键弹出 OL4 2 项确认弹窗（取消 / 关机），确认关机；观察 OLED 进度环与里程碑从参与者完成度平滑推进至 `POWER OFF` 终帧并稳定停留 ≥ 300 ms 后断电。
   - 按住电源键 8 s 验证强制关机兜底（`kForcePowerOffHoldMs`）仍生效。
   - 串口执行 `power off`、手机 App 发起关机，核对日志 `power off reason=console|phone` 与 trace 句子按名称输出。
2. **保存超时 3 s 与超时参与者清单（PW1 A/B）**：
   - 触发大配置保存同时发起关机，确认在 3 s 窗口内等待 NVS/GNSS/BT/Web 全部落盘；若人为阻塞超过 3 s，核对日志输出 `power save timeout after 3000 ms; pending=[...]` 且列出具体未完成参与者名，随后降级进入 `kQuiesce` 并完成断电。
3. **电池终帧停留期插 USB 边缘（PW1 D）**：
   - 电池供电下发起关机，在 OLED 刚切出 `POWER OFF` 终帧的 300 ms 停留期内插入 USB，确认屏幕切到 `RESTARTING` 并完整显示 ≥ 300 ms 后重启进入充电待机，不跳过 `RESTARTING` 帧。
4. **低电 <10% 持续 120 s 关机（PW1 B + HIL 手段）**：
   - 方案 1（默认固件 + 控制台注入）：拔掉 USB 后串口执行 `power simbat 5`，确认 119 s 内不关机；中途执行 `power simbat off` 或插入 USB 取消计时并恢复真实 ADC；再次 `power simbat 5` 持续满 120 s 后触发 `low_battery` 关机。
   - 方案 2（加速测试构建，不改默认配置）：使用临时覆写 `CONFIG_RTK_LOW_BATTERY_HOLD_MS=10000` 编译台架测试镜像，配可调电源调至 <10% 验证真实 ADC 链路。
5. **硬件过放即时关机阈值测量（PW1 B 可选项，`NOT_RUN`）**：
   - 当前过放即时关机保持关闭（`PowerLogic.hpp` 留有 `TODO`）；需先在 P2 v1.41 实测 CM680 / F9P 欠压复位点与稳压跌落余量后，再决定是否启用即时关机阈值。
