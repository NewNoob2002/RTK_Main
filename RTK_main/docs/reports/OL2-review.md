# OL2 独立代码审查报告

- **审查对象**：工作区 `feat/ol2-oled-ui` 未提交改动（相对基线 `78e5801`，19 个修改文件 + 6 个新增文件）
- **对照基线**：`RTK_main/AGENTS.md`、`RTK_main/docs/tasks/OL2-oled-ui.md`、`RTK_main/docs/reports/OL2-oled-ui.md`
- **Profile 与依据**：`esp-idf`（`sdkconfig.defaults`、`idf_component_register`），C++ 等级 T2
- **审查结论**：**FAIL**（`0 blocker / 3 major / 4 minor`；USB 关机终帧等待为永真死代码、对应主机测试存在假阳性、I2C 写失败仍标记终帧已刷出，须修复后复审）

---

## 1. 专项核查摘要

| 审查维度 | 结论 | 关键证据与风险 |
| --- | --- | --- |
| 1. 关机路径与 PowerModule 语义 | **FAIL** | `PowerLogic.cpp` 零改动，`ShutdownSm` 阶段与 30 s 超时回退语义完整保留；无 USB 路径不再调用 `s_panel.powerOff()` 且 GPIO12 不在 `kP2SafeOutputs` 中，保持亮屏至硬件断电；USB 路径在 `kSave`（`linkFence_`/`ioFence_` + 待落盘清空）与 `kQuiesce` 完成后才进入 `kCutPower`，未新增固定延时。**缺陷**：`ROL2-01`（`kOledFinalFlushMaxMs == kQuiesceMinWaitMs == 30U` 使 `waitOled`/`oledDone` 沦为永真死代码）、`ROL2-03`（I2C 写失败仍置位终帧已刷出）。 |
| 2. 按键方案 A 与关机中处理 | **PASS** | `PowerKeySm`（30 ms 消抖、<1000 ms 短按释放触发、≥2500 ms 长按触发且松开不发短按）零改动，长按绝不误触短按/双击；`ClickDetector`（300 ms 窗口、回绕安全、窗口到期同轮按键处理）边界正确；`UiController::step()` 在关机中与 `kChargeStandby` 下消费丢弃 `shortDelta` 并 `click_.reset()`，30 s 回退不会残留关机前/中按键。 |
| 3. 开机自检 | **PASS（含 minor）** | `collectViewModel` 仅读 `DataCenter`/`AppContext` 快照，零驱动调用、零阻塞；充电芯片缺失（`!chargerPresent && !chargerOk`）与 GNSS 未启用返回 `kAbsent`（显示 `-`，不挂告警）；GNSS 失败不阻塞并挂状态栏告警图标；每项最短展示 `200 ms <= 300 ms`。次要问题见 `ROL2-05`、`ROL2-06`。 |
| 4. I2C 共享、帧调度与任务栈 | **PASS** | `Sh1106::writePage` 仍按页独立加解锁（400 kHz 下单页 ~3.4–3.7 ms ≤ 8 ms，静态断言 `2*8 < 20`），`Charger::refreshInputStatus` 0-tick `BusLock(0U)` 未受改动；动画期 33 ms 扣减本帧耗时且至少让出 1 tick（`CONFIG_FREERTOS_HZ=1000`），静止期 200 ms 轮询由 `FrameTracker` 跳过实现 0 写 I2C。经 `xtensa-esp32-elf-objdump` 实核 ELF 栈帧：采集链 `taskEntry(32)+run(32)+refreshOnce(352)+collectViewModel(624)+collectSatSummary(544) ≈ 1,584 B`、渲染链含 picolibc `snprintf(112)+__d_vfprintf(128) ≈ 1,296 B`，均远低于 `3,072 B` 栈（实板 HWM 为 `NOT_RUN`）。 |
| 5. 定点动画正确性 | **PASS（含 minor）** | `easeOutCubic`（`uint64_t` 乘法 + `inv^3 <= 2^30`）、`lerpFixed`、`formatCoordE9`/`formatAltMm` 无溢出、端点精确、严格单调；`FixedSpring` 在常规 33 ms 与 32/64/96 px 跳变下 14–16 帧单调收敛、无过冲。**边界缺陷**：`ROL2-04`（`dtMs = 1 ms` 且位移 ≤10 px 时向零截断导致永不收敛）。 |
| 6. 内存（DRAM / 栈 / 无堆 / IRAM） | **PASS（含 minor）** | `sha256sum -c docs/frozen.sha256` PASS；`G7b` PASS（0 堆符号、0 异常/RTTI 违规、0 浮点 `printf`）；`nm` 核实 `Oled*` 静态 DRAM 为 `s_panel`(240 B `.bss`) + `s_ui_ctrl`(128 B `.data`) + 指针 8 B，合计增量远小于 1 KB；IRAM 增量 0 B；`BootArena` 4 KB 外供仅在 `Feature::kOled` 分配。注释偏差见 `ROL2-07`。 |
| 7. 测试有效性 | **FAIL** | 见 `ROL2-02`：USB 终帧握手测试与双击窗口缩短测试在对应实现被删除时仍会通过；`renderSystemCheckRow` 与 `drawRollingValue` 活跃滚动分支在主机测试与 ASan/UBSan 下 0 覆盖。 |

---

## 2. 审查发现清单

### Blocker

无。

---

### Major

#### ROL2-01（Major）— USB 关机终帧等待因超时常量与日志最小等待相等而沦为永真死代码
- **依据**：任务卡 §B（USB 路径允许的唯一额外等待是最后一帧 I2C 刷出 ≤30 ms，全部收尾完成后画“已关机/充电中”再 `esp_restart`）
- **位置**：`RTK_main/main/modules/power/PowerModule.cpp:35,38-39,408-412,736-744`
- **失败场景**：
  - `kQuiesceMinWaitMs = 30U`（L35），`isLogDrained(elapsed)` 在 `elapsed < 30` 时恒返回 `false`（L410）；`kRestartWaitMs = 150U`（L38）。因此 L741 右半式 `(elapsed >= kRestartWaitMs || isLogDrained(elapsed))` 为真必然蕴含 `elapsed >= 30`。
  - 同时 `kOledFinalFlushMaxMs = 30U`（L39），导致 L740 `oledDone = !waitOled || (elapsed >= 30U)` 在 `elapsed >= 30` 时恒为 `true`。布尔代数上 L741 与删除 `oledDone` 前的原代码完全等价，`UiBusFlag::kShutdownFinalFlushed` 与 `waitOled` 在任意运行场景下都无法推迟 `esp_restart()` 哪怕 1 ms。
  - **实板后果**：有 USB 关机且 U3（GPIO25）拉低瞬间，若低优先级 `oled` 任务（Core 1, prio 1）正处于前一帧（92% `CUTTING PWR`）的 I2C 刷写中（约 15–28 ms），随后才唤醒渲染并刷写 100% 终帧（约 18–25 ms），总耗时将超过 30 ms。在 `elapsed == 30 ms` 日志排空时，`PowerModule` 直接调用 `esp_restart()`，在 I2C 写页中途复位芯片——用户看不到“已关机/充电中”终帧，且存在从机持低 SDA 跨软复位的总线风险。
- **建议修复**：
  - 将 OLED 终帧额外等待窗口与日志排空最小窗口解耦：首次满足 `(elapsed >= kRestartWaitMs || isLogDrained(elapsed))` 时记录 `oledWaitStartMs_`（或在该分支内按 `nowMs - oledWaitStartMs_ >= kOledFinalFlushMaxMs` 判定终帧超时上限 30 ms）。
  - 当 OLED 在日志排空前已刷出终帧（`shutdownFinalFrameFlushed() == true`）或无 OLED（`oledActive() == false`）时，额外等待仍为 **0 ms**；当 OLED 尚未刷完终帧时，真正给予最多 `kOledFinalFlushMaxMs = 30 ms` 的额外刷出窗口，超时强制重启防挂死。

#### ROL2-02（Major）— 主机测试对 USB 终帧握手与双击缩短休眠存在假阳性，且遗漏关键渲染路径覆盖
- **依据**：任务卡“测试”要求（主机测试须能拦截坏实现）
- **位置**：
  - `RTK_main/tools/main_host_check/MainHostCheck.cpp:437-455`
  - `RTK_main/tools/main_host_check/OledUiCheck.cpp:320-324,437-498`
- **失败场景**：
  1. **`MainHostCheck.cpp:447-453`**：`pollPower(2U)` 仅推进到 `elapsed = 20 ms`，此时 `s_restarts == 0U` 仅因 `elapsed < kQuiesceMinWaitMs (30 ms)`；紧接着调用 `markShutdownFinalFrameFlushed()` 并 `pollPower(2U)` 到 `elapsed = 30 ms` 断言 `s_restarts == 1U`。**即使删掉 `PowerModule.cpp:738-740` 的全部 OLED 等待逻辑或删掉测试里的 `markShutdownFinalFrameFlushed()`，该测试仍 100% 通过**。且未测试“终帧未置位在日志排空后被拦住、满 30 ms 额外上限才超时回退重启”以及“无 OLED 板型 0 额外等待”。
  2. **`OledUiCheck.cpp:320-321`**：短按后在 `elapsed = 0 ms`（`remainingMs == 300 ms`）断言 `dec.nextWaitMs <= kIdlePollPeriodMs (200 ms)`，因 `min(200, 300) == 200`，**即使完全删掉 `OledUi.cpp:439-442` 的窗口缩短调度代码，断言依然通过**。
  3. **`OledUiCheck.cpp:446-498`**：`renderPage(OledPage::kSystemCheck, ...)` 传入 `checkSm == nullptr`，仅覆盖了 Logo 阶段；整份测试从未在 `checkSm` 处于 `kChecking`/`kSummary` 或 `satsRoll`/`batRoll` 处于活跃滚动期时调用 `renderUi`，导致 `renderSystemCheckRow`（`OledRender.cpp:400-449`）与 `drawRollingValue` 滚动剪裁分支（`OledRender.cpp:266-277`）在普通与 ASan/UBSan 下均为 **0 覆盖**。
- **建议修复**：
  - 在 `MainHostCheck.cpp` 中拆分验证：① `oledActive=true` 且未标终帧时，推进至日志排空点（30 ms）后继续推进 20 ms 仍 `s_restarts == 0U`，标终帧后下一轮立刻 `s_restarts == 1U`；② 始终不标终帧时，超过 30 ms 额外上限后兜底触发 `s_restarts == 1U`；③ `oledActive=false` 时在 30 ms 日志排空点立即重启。
  - 在 `OledUiCheck.cpp` 中于短按 250 ms 后（剩余窗口 50 ms）断言 `dec.nextWaitMs == 50U`；补充以活跃 `SystemCheckSm`（覆盖 `kOk`/`kFail`/`kAbsent`/`kPending` 及等待 ≥1 s + GNSS 型号固件显示）与活跃 `NumberRoll`（增/减方向中点帧）调用 `renderUi` 的用例。

#### ROL2-03（Major）— USB 关机终帧 I2C 写出失败时仍无条件标记终帧已刷出
- **依据**：任务卡 §B（确保“已关机/充电中”终帧实际刷出后再重启）
- **位置**：`RTK_main/main/modules/oled/OledModule.cpp:307-312`
- **失败场景**：
  - L307 执行 `WouoUI_BuffSend()` 后，L310-312 仅判断 `if (dec.markShutdownFinalFrame)` 即置位 `markShutdownFinalFrameFlushed()`，未校验 `s_last_flush`。
  - 若此时 I2C 总线因共享器件瞬时占锁返回 `kBusy` 或写某页报错（`s_last_flush == board::Sh1106FlushResult::kError`），SH1106 仅刷出半帧或停留在 92% 画面，但 OLED 立即置位完成标志促使 `PowerModule` 立刻重启，放弃了 30 ms 窗口内的重试机会。
- **建议修复**：
  - 仅在 `dec.markShutdownFinalFrame && (s_last_flush == board::Sh1106FlushResult::kWritten || s_last_flush == board::Sh1106FlushResult::kSkipped)` 时置位 `markShutdownFinalFrameFlushed()`；当 `dec.markShutdownFinalFrame && s_last_flush == kError` 时跳过退避并缩短下轮等待（如 1–5 ms），争取在 30 ms 上限内补刷成功。

---

### Minor

#### ROL2-04（Minor）— `FixedSpring::step()` 在 `dtMs = 1 ms` 且小位移（≤10 px）时因整数向零截断陷入死区
- **依据**：定点动画终值收敛正确性要求
- **位置**：`RTK_main/main/modules/oled/OledAnim.cpp:121-149`
- **失败场景**：
  - 在 1 ms 步长（例如 `CONFIG_FREERTOS_HZ=1000` 下频繁被外部事件唤醒，或复用 `FixedSpring` 于 1 ms 节拍）且剩余位移 `<= 10 px` 时，积分会收敛至 `errQ8 = 80, velQ8 = 715` 的伪平衡点：`accQ8 = 80*220 - 715*26 = -990`，`(accQ8 * 1) / 1000 == 0` 且 `(715 * 1) / 1000 == 0`，位置与速度均不再变化，但 `|velQ8| = 715 > kSpringSettleVelQ8 (512)`，导致 `settled_` 永不为 `true`、`animating` 无法退出。
  - 注：常规 33 ms 帧周期及 32/64/96 px 表盘跳变经仿真验证均在 14–16 帧内严格单调收敛且无过冲；但现有测试 `OledUiCheck.cpp:182-198` 仅测 33 ms + 96 px 终值，未测小 `dt` 与过程单调性。
- **建议修复**：
  - 在 `FixedSpring::step()` 中检测整步更新后 `posQ8_` 与 `velQ8_` 均未发生变化且 `absErr <= kScale` 时强制吸附至 `targetQ8_` 并置 `settled_ = true`（或改用 Q16 速度 / 非零最小步进）；并在 `OledUiCheck` 增补 `dt = 1/2/16/33 ms` 与逐帧单调性校验。

#### ROL2-05（Minor）— GNSS 处于 `kOutput`/`kReadback`/`kMaintenance`/`kPowerUp` 时表盘误报 `"OFF"` 与 `"GNSS WAIT"`
- **依据**：任务卡 §C（表盘实时准确展示 GNSS 解状态与卫星数）
- **位置**：`RTK_main/main/modules/oled/OledUi.cpp:477,696-712`、`RTK_main/main/modules/oled/OledRender.cpp:319,513`
- **失败场景**：
  - `dialHeroBadge` 仅识别 `kProbing`/`kConfiguring`/`kFailed`/`kRunning`，对 `gnss::State` 中的 `kPowerUp`/`kOutput`/`kReadback`/`kMaintenance` 统一返回 `"OFF"`；`drawStatusBar` 与 `renderDialplate` 也只在 `kRunning` 下渲染卫星数与精度。
  - 运行期通过 App/控制台调整输出语句（进入 `kOutput`）或读回配置（进入 `kReadback`）时，表盘会短暂闪现 `"OFF"` / `"GNSS WAIT"` / `"--"`，误导操作者以为 GNSS 已关闭。
- **建议修复**：
  - 区分各枚举状态：`kPowerUp` 显示 `"PROBE"`/`"BOOT"`，`kOutput`/`kMaintenance` 显示 `"CFG"`，`kReadback` 沿用运行态定位显示（提取 `isGnssRunningLike(gnss::State)` 辅助函数供状态栏与表盘共用）。

#### ROL2-06（Minor）— 双击跳过自检时若 GNSS 仍在探测且随后失败，状态栏不会补挂告警图标
- **依据**：任务卡 §A（自检失败项不阻塞，进表盘后状态栏显示告警图标）
- **位置**：`RTK_main/main/modules/oled/OledUi.cpp:184-195,359-368`
- **失败场景**：
  - 用户在 Logo 或前 4 项期间双击跳过自检，`SystemCheckSm::skip(vm)` 以 `elapsedMs = 0U` 评估 GNSS，此时 `gnssState == kProbing` 返回 `kPending`，`hasWarning_` 为 `false`。随后控制器留在 `kDialplate` 不再重估 GNSS，即使数秒后 GNSS 探测超时进入 `gnss::State::kFailed`，状态栏告警三角图标也不会亮起。
- **建议修复**：
  - 在 `UiController::renderState()` 或 `step()` 中将 `hasWarning_` 与 `(vm.gnssEnabled && vm.gnssState == gnss::State::kFailed)` 取或（或追踪跳过时未决的 GNSS 项直至其落地）。

#### ROL2-07（Minor）— OL1 遗留不可达断电/唤醒死代码与 `s_ui_ctrl` 落 `.dram0.bss` 注释失实
- **依据**：`AGENTS.md`（清理死代码、保持注释与实测一致）
- **位置**：`RTK_main/main/modules/oled/OledUi.hpp:255,278,311,411,413-414`、`RTK_main/main/modules/oled/OledUi.cpp:319-324,403-406,468-473`、`RTK_main/main/modules/oled/OledModule.cpp:35,38,291-300`、`RTK_main/docs/adr/ADR-0020-oled-display.md:112`
- **失败场景**：
  - OL2 移除关机主动断屏后，`UiController::poweredOff_` 仅在 `onWakeFailed()` 中被置为 `true`，而 `onWakeFailed()` 仅在 `dec.shouldWake == true`（前提是 `poweredOff_ == true`）时触发，相关状态与 `wakeFromPowerOff()` 调用点形成不可达死分支。
  - `OledUi.hpp:278`、`OledModule.cpp:35` 及 `ADR-0020` §5.1 声称 `s_ui_ctrl` 全零落 `.dram0.bss`，但 `nm` 实测 `s_ui_ctrl` 因 `FixedSpring::settled_ = true` 和 `PageTransition::durationMs = 300U` 位于 `.data`（128 B，`3ffc4a68 d`）。
- **建议修复**：
  - 删除 `UiController` 中不可达的 `poweredOff_`/`wakeRetryActive_`/`wakeFailedAtMs_`/`onWakeFailed`/`shouldWake` 残留逻辑；同步修正源码与 ADR 中的 `.bss` 注释（或将成员初值改为全零语义使其真正落入 `.bss`）。

---

## 3. 审查期间独立执行的核查命令与结果

| 检查项 | 执行命令 / 范围 | 结果 |
| --- | --- | --- |
| 冻结文件校验 | `cd RTK_main && sha256sum -c docs/frozen.sha256` | **PASS**（全部 OK） |
| 主机测试（OLED / Power） | `ctest --test-dir builds/ol3-main_host_check -R "oled\|power"` | **PASS**（43/43） |
| 门禁日志核验 | 检查 `builds/ol3-logs/G7b-runtime.log`、`G5b-tidy.log`、`G6-doc_check.log`、`G7c-no-cli-deps.log` | **PASS**（0 堆/0 tidy/0 doc 告警） |
| ELF 栈帧与符号核查 | `xtensa-esp32-elf-objdump -d` / `xtensa-esp32-elf-nm -S` 检查 `builds/ol3-idf/rtk_main.elf` | **PASS**（证实 `noinline` 栈分离、0 堆/0 浮点、发现 `s_ui_ctrl` 落 `.data`） |
| 定点弹簧数值仿真 | Python 精确复现 `FixedSpring::step` 扫描 `dt=1..64 ms` 与多组位移 | **发现 `ROL2-04`**（`dt=1 ms, delta<=10` 死区） |
| 实板 HIL | 硬件禁止访问 | **NOT_RUN** |

---

## 4. 最终结论

**FAIL**

请优先修复 `ROL2-01`（USB 终帧等待死代码）、`ROL2-02`（补齐能拦截坏实现的主机测试与渲染覆盖）与 `ROL2-03`（I2C 失败不误报终帧已刷出），建议一并处理 `ROL2-04`～`ROL2-07`，重新通过全量门禁后再行复审。
