# PW1 实施报告：关机收尾参与者表、低电/保存超时与 OL3 终帧边缘修复

- **分支 / 基线**：`feat/pw1-shutdown`，对比与 `doc_check` 基线 `main 6b54fc4`（任务卡提交 `2f97736`）
- **任务卡**：`docs/tasks/PW1-shutdown-participants.md`（完整实施 A / B / C / D 与低电 HIL 测试支持）
- **边界确认**：未修改 `docs/protocol/`、`docs/web/` 及任何冻结文件（`G1-frozen` PASS）；未改变产品默认值；未执行烧录、串口或硬件操作。

---

## 1. 改动清单

### A. 静态关机收尾参与者登记表与 OLED 进度派生
- `main/modules/power/PowerModule.hpp`、`main/modules/power/PowerModule.cpp`：
  - 新增静态参与者契约 `power::ShutdownParticipant{name, begin, isDone, userCtx, stage, milestone}` 与启动期一次性静态槽位注册 API（`registerShutdownParticipant` 返回 `fw::Status`，另有 `clearExtraShutdownParticipants` / `extraShutdownParticipants`，上限 `kMaxExtraShutdownParticipants = 4`，编译期 `static_assert` 约束总数 ≤ 16，无运行期堆）。外部登记只接受 `kSave` / `kQuiesce`；`kCutPower` 由内部 OLED 双终帧有界流程独占。名字限长 `kMaxShutdownParticipantNameLen = 32` 且不得与内置名重名。
  - 将既有全部硬编码关机检查迁移为编译期内置参与者表 `PowerModule::ParticipantOps::kBuiltin`（11 项，落 `.rodata`）：
    - `kSave`：`storage_fence`、`gnss_fence`、`storage_flush`（待落盘队列 + NVS 在途写）、`bt_settings`、`web_settings`、`gnss_nvs`。
    - `kQuiesce`：`radio_pause`（BT/Wi-Fi 无线静默）、`safe_gpio`（外设安全输出电平）、`log_drain`（异步日志 + UART 排空）。
    - `kCutPower`：`oled_final_bat`（电池终帧停留）、`oled_final_usb`（USB RESTARTING 终帧停留）。
  - `PowerModule` 阶段入口（`beginStageParticipants`）、完成轮询（`pollStageParticipants`）、超时记录（`recordStageTimeouts`）与超时日志（`logStageTimeout`）统一遍历参与者表，超时日志按参与者逐项输出：`power save|quiesce timeout pending=<完整名字>`（kSave 另有存储汇总行），不再拼接定长缓冲。
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
    - DEV 档控制台新增 `power simbat <0-100|off>`，经无锁原子 `power::setSimulatedBatteryPct` 注入模拟电量；每次实际状态变更（启用、改值、退出、重注入）递增会话代号并与百分比一同原子发布，模拟已关闭时重复 `off` 为幂等空操作、不递增代号；会话变化即复位低电计时、丢弃模拟读数并恢复真实 ADC 采集，真实 ADC 成功读到 0 mV 不再被误判为模拟。

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
  - 新增并保留 `power_participant_blocks_and_timeout_names`（阻塞断电直到超时、超时日志列名、非法/重名/超长/内置同名/非 kSave·kQuiesce 阶段注册按 `fw::Status` 拒绝、全部 5 种 `ShutdownReason` trace 格式化）、`power_oled_battery_to_usb_final_frame`（电池终帧停留期间插 USB 仍完整等待 RESTARTING 终帧与 300 ms 停留）、`power_sim_battery_low_shutdown`（模拟低电注入、清除恢复真实 ADC 与 120 s 触发关机），并在 `OledUiCheck` 中补齐参与者计数进度派生与 `kWeb` OLED 渲染断言。
  - 评审修复新增 6 组：`power_oled_usb_final_frame_stale_clock`、`power_oled_battery_final_frame_stale_clock`、`power_participant_four_slot_names`、`power_sim_battery_boundary`、`power_low_battery_real_zero_mv`、`power_sim_battery_reinject`，复审新增 `power_oled_usb_late_u3_final_frame_hold`、`power_sim_battery_redundant_off`，以及 `OledUiCheck` 的 kCutPower 92% 断言（详见第 5 节）。

---

## 2. DRAM / IRAM / Flash 与基线对比

对比条件：同一 `sdkconfig.defaults`（ESP-IDF v6.1，`esp32` 默认构建），基线为 `git archive 6b54fc4` 干净构建，目标为 `builds/pw1-idf`。

### 整机镜像对比

| 指标 | 基线 `6b54fc4` | PW1 `builds/pw1-idf` | 增量 | 约束 |
| --- | ---: | ---: | ---: | --- |
| `rtk_main.bin` | 1,582,304 B | 1,585,232 B | **+2,928 B** | 应用分区余 19%（`0x5cfd0` B） |
| Flash Code (`.text`) | 1,138,556 B | 1,140,848 B | **+2,292 B** | — |
| Flash Data (`.rodata`/`.appdesc`/`.tdata`) | 274,556 B | 275,068 B | **+512 B** | 内置参与者表入 `.rodata` |
| IRAM | 116,139 B | 116,139 B | **0 B** | **0 B（满足）** |
| 静态 DRAM 合计 | 98,475 B | 98,611 B | **+136 B** | **≤ 256 B（满足）** |
| — DRAM `.data` | 52,891 B | 53,019 B | +128 B | 4 槽额外参与者数组 + 模拟电量会话原子字 + 新增快照/模块状态 |
| — DRAM `.bss` | 45,584 B | 45,592 B | +8 B | 额外参与者计数等 |
| RTC SLOW | 68 B | 68 B | **0 B** | — |
| 运行期堆分配 | 0 B | 0 B | **0 B** | **零运行期堆（满足）** |

### 主要改动目标文件明细（`builds/pw1-idf`，评审修复前首轮实现的数据，修复后整机增量见上表）

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

在 `RTK_main` 下执行 `IDF_PATH=/home/gtc/esp/esp-idf-v6.1 PATH="/home/gtc/.espressif/python_env/idf6.1_py3.13_env/bin:$PATH" bash tools/ci/run_gates.sh --prefix pw1 --base 6b54fc4`，**43 passed, 0 failed**（复审修复后重跑；日志目录 `RTK_main/builds/pw1-logs/`）：

| 门禁 | 结果 | 详情 |
| --- | --- | --- |
| `G1-frozen` | **PASS** | `sha256sum -c docs/frozen.sha256` 全部匹配 |
| `G2-main_host_check` / `G2b-main_host_check-asan` | **PASS** | 205/205 通过（含首轮 3 组、评审修复新增 6 组、复审新增 2 组 concurrent+preempt 场景及 OledUi/PowerLogic 新断言） |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | **PASS** | 46/46 通过 |
| 其余 12 组 `G2-*` / `G2b-*-asan`（host / app_fw / services / stream_demux / bt_reconnect / cli / gnss / gnss-all / nvs_store / web_proto / settings / ota） | **PASS** | 全部普通 + ASan/UBSan 通过 |
| `G5-format` | **PASS** | `clang-format --style=file --dry-run --Werror` 零差异 |
| `G6-doc_check` | **PASS** | `改动函数 57 个，不合格 0 个`（基线 `6b54fc4`） |
| `G3-idf`（default / trace-off / wifi-off / ota-off / production / console-none / none-wifi）与 `G3b-rtk_pure`（esp32 / esp32c3） | **PASS** | 9 档全过，0 warning |
| `G5b-tidy` | **PASS** | 159 个文件，0 条告警 |
| `G7b-runtime` | **PASS** | 无禁止异常/RTTI 符号 |
| `G7c-no-cli-deps` | **PASS** | `builds/pw1-none-wifi` 零 CLI/REPL 依赖 |
| 实板 HIL（P2 v1.41） | **NOT_RUN** | 未连接/操作实板，待按 §4 执行 |

---

## 4. HIL 待测步骤与 `NOT_RUN` 项

以下项目当前为 **`NOT_RUN`**，待实板（P2 v1.41，DEV 档固件 `builds/pw1-idf`）按步骤验收：

1. **长按 / 控制台 / 低电关机与 OLED 进度条（PW1 A/D）**：
   - 无 USB 下长按电源键弹出 OL4 2 项确认弹窗（取消 / 关机），确认关机；观察 OLED 进度环与里程碑从参与者完成度平滑推进至 `POWER OFF` 终帧并稳定停留 ≥ 300 ms 后断电。
   - 按住电源键 8 s 验证强制关机兜底（`kForcePowerOffHoldMs`）仍生效。
   - 长按电源键（`reason=key`）、串口执行 `power off`（`reason=console`）与低电关机（`reason=low_battery`，见第 4 项）三条路径，核对日志 `power off reason=...` 与 trace 句子按名称输出。
2. **保存超时 3 s 与超时参与者清单（PW1 A/B）**：
   - 触发大配置保存同时发起关机，确认在 3 s 窗口内等待 NVS/GNSS/BT/Web 全部落盘；若人为阻塞超过 3 s，核对日志输出 `power save timeout after 3000 ms; ...` 摘要行及随后逐项 `power save timeout pending=<名字>`，列出具体未完成参与者名，随后降级进入 `kQuiesce` 并完成断电。
3. **电池终帧停留期插 USB 边缘（PW1 D）**：
   - 电池供电下发起关机，在 OLED 刚切出 `POWER OFF` 终帧的 300 ms 停留期内插入 USB，确认屏幕切到 `RESTARTING` 并完整显示 ≥ 300 ms 后重启进入充电待机，不跳过 `RESTARTING` 帧。
4. **低电 <10% 持续 120 s 关机（PW1 B + HIL 手段）**：
   - 方案 1（默认固件 + 控制台注入）：拔掉 USB 后串口执行 `power simbat 5`，确认 119 s 内不关机；中途执行 `power simbat off` 或插入 USB 取消计时并恢复真实 ADC；再次 `power simbat 5` 持续满 120 s 后触发 `low_battery` 关机。
   - 方案 2（加速测试构建，不改默认配置）：使用临时覆写 `CONFIG_RTK_LOW_BATTERY_HOLD_MS=10000` 编译台架测试镜像，配可调电源调至 <10% 验证真实 ADC 链路。
5. **硬件过放即时关机阈值测量（PW1 B 可选项，`NOT_RUN`）**：
   - 当前过放即时关机保持关闭（`PowerLogic.hpp` 留有 `TODO`）；需先在 P2 v1.41 实测 CM680 / F9P 欠压复位点与稳压跌落余量后，再决定是否启用即时关机阈值。


### 4.1 实板结果（2026-11-25，P2 v1.41，`builds/pw1v-idf` @ `21fbabc`，DEV 档，电池供电 96%）

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 长按关机：OL4 确认弹窗、进度环、`POWER OFF` 终帧 ≥ 300 ms | PASS（用户实测） | 目视 |
| 8 s 强制关机兜底 | PASS（用户实测） | 目视 |
| 电池终帧停留期插 USB → `RESTARTING` ≥ 300 ms → 充电待机 | PASS（用户实测） | 目视 |
| USB 在位时关机 → `RESTARTING` → 重启不挂起 | PASS（用户实测） | 目视 |
| 控制台 `power off` | PASS（自动） | `power off reason=console usb=0`，0.55 s 后 `device powered down` |
| 低电 120 s：`simbat 5` 63 s 后 `off` 取消；重复 `off` 无副作用；再 `simbat 5` | PASS（自动） | 第二轮 120.03 s 触发 `low battery shutdown` / `reason=low_battery usb=0`；111 s 时仍 `stage=idle` |
| 断电约 9 s 后串口出现 `E BOD: Brownout detector was triggered` | 正常（用户确认） | 电池通路关断后的欠压，非缺陷 |
| 模拟低电期间 OLED 显示 | PASS（用户目视） | 串口打开复位一次后 `simbat 5` 显示 5%，100 s 后 `simbat off` 恢复 96%，期间未关机 |
| 3 s 保存超时与逐项参与者名 | NOT_RUN | 控制台无阻塞保存手段，由主机测试覆盖 |
---

## 5. 评审修复对照（`feat/pw1-shutdown`，评审结论 FAIL → 修复后重跑门禁）

| 评审号 | 修复 | 回归测试 / 证据 |
| --- | --- | --- |
| Important 1 | `registerShutdownParticipant` 改返回 `fw::Status`；外部仅接受 `kSave` / `kQuiesce`，其余阶段 `kInvalidArg`；`kCutPower` 仅由内部有界 OLED 双终帧流程处理；USB 重启路径不再绕过 30 s 回退的原因是外部项已无法进入 kCutPower，而内置 OLED 终帧等待有界（约 30 ms 日志等待 + 终帧确认 + 300 ms 停留）。**复审修订**：首轮在 `pollRestart` 新增的“自进入断电阶段 30 s”兜底已删除——ADR 只规定 U3 未关断时的 30 s 回退（`ShutdownSm::stepCutPower`），U3 关断后的终帧确认与 300 ms 停留不得被提前结束。 | `power_participant_blocks_and_timeout_names`：`kIdle/kAnnounce/kCutPower` 阶段登记返回 `kInvalidArg`。复审回归：`power_oled_usb_late_u3_final_frame_hold`（29,900 ms 时 U3 关断，再确认 USB 终帧，停留满 300 ms 才重启，299 ms 不重启）。该用例与 `power_sim_battery_redundant_off` 在复审前提交 `5f70695` 上均失败（已在临时副本验证）。 |
| Important 2 | `quiesceLimitMs` / `beginStageParticipants` 增加 `builtin` 判定：`kSafeGpio` 零等待与失败标记只绑定内置 `safe_gpio`，额外项按通用 150 ms 超时。 | `power_participant_four_slot_names`：内置 safe_gpio 失败（`kIoError`）时，额外 `kSafeGpio` 项未完成不被提前放行，quiesce 等满超时后才进入 `kCutPower`。 |
| Important 3 + Nit 8 | 模拟电量改为单个原子字（低 8 位百分比 + 高 24 位会话代号，启用 / 改值 / 退出 / 重注入时递增，模拟已关闭时重复 `off` 不递增）；`pollBattery` 以会话变化复位低电计时并丢弃模拟读数；删除 `batteryMv_==0` 哨兵，成功的真实 0 mV 不再重置计时；清除后立即重注入不再合并两轮。 | `power_sim_battery_boundary`（119999 ms 不触发 / 120000 ms 触发）、`power_low_battery_real_zero_mv`、`power_sim_battery_reinject`。在首轮提交 `4d80bd0` 上验证：后两项失败、boundary 通过。 |
| Important 4 | `finalFrameReady` 首次观察到终帧确认后重新读取 `rtos::nowMs()`，以该时间计算 300 ms 停留。 | `power_oled_usb_final_frame_stale_clock`（确认前 100 ms 的旧时间 poll，+249/+299 ms 不重启，+300 ms 重启）与 `power_oled_battery_final_frame_stale_clock`。 |
| Important 5 | 保留原行为场景，新增上述 stale-clock 用例，并按真实先后（GPIO4 动作 → U3 关断 → 终帧确认）驱动。 | **基线 `2f97736` 临时目录验证**：`git archive 2f97736` 解出副本，接入同一测试后，`power_oled_usb_final_frame_stale_clock` **失败**（`s_restarts == 0U` 两处），电池版在基线通过（基线电池路径本就重读时间，非回归点）。**故障注入**：将 `finalFrameReady` 中 USB 状态别名到电池状态后，`power_oled_battery_to_usb_final_frame` **失败**；stale-clock USB 用例未检出该注入（诚实记录：USB 状态独立性由原用例覆盖）。注入后已恢复源码。 |
| Nit 6 | 名字上限 `kMaxShutdownParticipantNameLen = 32`；拒绝与内置同名；超时日志逐项输出完整名字，移除定长拼接缓冲。 | `power_participant_four_slot_names`：4 个 32 字符名 + 第 5 个 `kNoSpace`，日志断言 pending 项完整名字可见、已完成项不出现；`power_participant_blocks_and_timeout_names` 覆盖 33 字符与内置同名拒绝。 |
| Nit 7 | `shutdownTargetPercent` 在 `kCutPower` 未到终态时保持 92%。 | `OledUiCheck.cpp`：`kCutPower` 在 7/8、0/8、0/0 下均为 92，终态仍为 100。 |
| Nit 9 | 第 4 节 HIL 第 1 项删除手机关机步骤，改为长按 / 控制台 / 低电三条路径。 | 文档变更。 |

### 复审（PW1-rereview）追加修复

| 复审号 | 修复 | 回归测试 / 证据 |
| --- | --- | --- |
| Important 1 | 删除 `pollRestart` 中自增的 U3 已关断后 30 s 兜底；仅保留 ADR 规定的 U3 未关断 30 s 回退，U3 关断后沿用有界日志等待、终帧确认与 300 ms 停留。 | `power_oled_usb_late_u3_final_frame_hold`；在 `5f70695` 失败。 |
| Important 2 | `setSimulatedBatteryPct(kBatteryPctUnknown)` 在模拟本已关闭时幂等：CAS 循环内直接返回，不递增会话代号，不重置真实低电计时；真正的启用 / 退出 / 重注入仍各自递增会话。 | `power_sim_battery_redundant_off`：真实 5% 低电 119 s 时执行一次 `off`，超过 120 s 仍受理低电关机；在 `5f70695` 失败。 |
| Nit | 登记名字拒绝控制字符（`< 0x20`，含 CR/LF，以及 `0x7F`），返回 `kInvalidArg`。 | `power_participant_blocks_and_timeout_names` 新增含 `\n` / `\r` / `\t` / `0x01` / `0x7F` 的名字拒绝断言；在 `5f70695` 失败（临时副本中该用例崩溃/失败）。 |

约束复核：DRAM 增量相对 `6b54fc4` 为 **+136 B（≤ 256 B）**，IRAM **0 B**，无运行期堆；未修改冻结文件（`G1-frozen` PASS）。实板 HIL 仍为 `NOT_RUN`。
