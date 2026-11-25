# OL2 实施报告：OLED 表盘 UI、开机自检与关机进度、流畅动画

- **任务卡**：`docs/tasks/OL2-oled-ui.md`
- **基线提交**：`78e5801`（OL1 已合入 `main`）
- **Profile 与依据**：`esp-idf`（`sdkconfig.defaults`、`idf_component_register`），C++ 等级 T2
- **并行边界确认**：未修改 `components/gnss/`、`main/modules/gnss/`、`main/settings/`、`main/app/SettingsBackend.*` 及任何冻结文件（`G1-frozen` PASS）。
- **未执行动作确认**：未执行 `git commit`/`push`、未烧录、未碰串口。

---

## 1. 改动清单

### 新增文件
- `main/modules/oled/OledAnim.hpp`、`main/modules/oled/OledAnim.cpp` — 纯定点动画引擎与按键方案 A 单键消歧器：`ClickDetector`（`kDoubleClickWindowMs = 300 ms` 短按/双击判定，支持 `uint32_t` 回绕）、Q10 三次缓出 `easeOutCubic` / `lerpFixed`、Q8 阻尼弹簧 `FixedSpring`（16 ms 子步半隐式欧拉 + 终值吸附）、竖向数字滚动 `NumberRoll`（180 ms）、页面过渡 `PageTransition`（300 ms）。无浮点、无堆、无 IDF/FreeRTOS 头。
- `main/modules/oled/OledIcons.hpp` — 全自绘 XBM 风格单色位图（列主序垂直字节，零版权素材）：`8×8` 卫星/蓝牙待机/蓝牙已连/Wi-Fi、`7×8` 告警/勾选/叉号/横杠、`5×8` 闪电、`4×14×14` 底部入口图标（卫星/位置/设备/电源），以及 32 点定点关机进度环坐标表（全 `constexpr` 落 `.rodata`，0 DRAM / 0 IRAM）。
- `main/modules/oled/OledRender.cpp` — WouoUI `graph/font` 图形渲染器：单色 Logo 逐列展开、5 项自检实时列表与底部进度条、主表盘（顶栏 + `Font_12_24` 大号解状态/基站徽章 + 4 入口图标弹簧框）、4 个子菜单（卫星星座柱状图 + Top-8 CN0 载噪比条、纯定点经纬高/HDOP/差分龄期、设备信息、电源详情）、充电待机页、32 点定点进度环关机页，以及滑动视差（`kSlideLeft`）和图标展开/收回（`kExpandFromIcon`/`kCollapseToIcon`）过渡动画。渲染末尾保持 `WouoUI_GraphSetPen(nullptr)` 防悬垂。
- `tools/main_host_check/OledModuleCheck.cpp` — 真实 `OledModule.cpp` + `Sh1106.cpp` + `I2cBus.cpp` + `OledUi/Anim/Render` 集成主机测试（经 I2C 与虚拟时钟替身），覆盖终帧编排（连续错误→退避→清退避→部分页失败重试→严格先刷屏后置终帧位→0 脏页跳过）与 OLED 正忙时的 299/300 ms 生产时间双击及绝对截止休眠重算。
- `docs/reports/OL2-oled-ui.md` — 本报告。

### 修改文件
- `main/board/PowerLogic.hpp` — 新增 `ShutdownDoneBit` 与 `ShutdownDegradedBit` 枚举及辅助函数，分别只读表达真实收尾完成位与超时/失败降级位（`kSaveTimeout`、`kPeripheralsFailed`、`kRadioTimeout`）；`ShutdownSm` 状态机与超时常量语义零改动。
- `main/def/SystemDef.hpp` — `PowerSnapshot` 新增 4 个只读字段：`batteryAdcReady`、`chargerPresent`、`shutdownDoneMask`、`shutdownDegradedMask`（供自检页与关机进度页只读消费，UI 不直接调驱动）。
- `main/data/AppData.hpp` — `DataCenter` 新增无锁原子 `std::atomic<bool> wifiReady{false}`（`WebModule` 写，OLED 自检读）。
- `main/modules/web/WebServer.cpp` — `WebModule::init()` 成功后置位 `data.wifiReady = true`，`stopBoot()` 时复位 `false`。
- `main/modules/power/PowerModule.hpp`、`main/modules/power/PowerModule.cpp` — 保存、外设与无线收尾按各里程碑报告，超时或失败写入独立的 `shutdownDegradedMask`。USB 日志放行后锁存 30 ms 绝对截止，每次相对等待最多一 tick，等待前后核对单调时钟，终帧通知可提前唤醒。恢复 `kCutPower` 起始时间的记录。消抖短按继续投递生产时间戳。
- `main/def/OledDef.hpp` — `OledPage` 扩展为 8 页；`OledStatus` 新增 `fpsX10`/`lastDirtyPages`/`totalDirtyPages`/`lastI2cUs`/`maxI2cUs`/`lastFrameUs`/`maxFrameUs`；`UiEventBus` 新增无锁 `UiBusFlag`（`kOledActive`/`kShutdownFinalFlushed`）、重启唤醒钩子（`setRestartWakeHook()`）以及无锁 SPSC 消抖短按时间戳队列（`kShortKeyQueueCap = 8`）。
- `main/board/Sh1106.hpp`、`main/board/Sh1106.cpp` — 每页超时传参预算 8 ms，页地址命令和数据段共用。驱动分段等待与错误恢复可能超出，实板测最大持锁。释锁后对 `ESP_ERR_TIMEOUT` 和 `ESP_ERR_INVALID_RESPONSE` 尝试总线复位，失败页摘要失效。待判单击到期时中断剩余脏页并回到 UI 状态机。
- `main/modules/oled/OledUi.hpp`、`main/modules/oled/OledUi.cpp` — 升级为 `SystemCheckSm` + `UiController` 控制器；新增纯整数定点格式化、`dialHeroBadge`（覆盖全 9 态）、`dialGnssDetail`（按 GNSS 非运行状态输出 `DISABLED`/`POWER UP`/`PROBING`/`CONFIG`/`SET OUT`/`MAINT`/`FAULT` 替代笼统等待文案）、`settleFrameWait`（按 `FlushOutcome::kBackoff` 重试并按 `clickDeadlineMs - postFlushNowMs` 重算精确休眠）、`shutdownStageText`（独立呈现 `SAVE TIMEOUT`/`RF TIMEOUT`/`SAFE IO ERR` 降级文案）；清理 OL1 不可达唤醒死代码，使 `s_ui_ctrl` 全零落 `.dram0.bss`。
- `main/modules/oled/OledModule.hpp`、`main/modules/oled/OledModule.cpp` — 接入 `UiController`、`renderUi` 与 `settleFrameWait`；动画期按 33 ms 扣除本帧耗时调度（~30 fps），静止期 200 ms 轮询且脏页为 0 时 0 写 I2C；通过 `[[gnu::noinline]]` 隔离 `collectViewModel` 与 `collectSatSummary`（避免 512 B `SatTable` 内联进 `refreshOnce` 撑大渲染与 I2C 调用栈）；记录 1 s 滑动窗口 FPS 与微秒级 I2C/帧耗时；USB 关机终帧刷屏前 `clearBackoff()`，仅在 `kWritten`/`kSkipped` 时置位 `markShutdownFinalFrameFlushed()`，写失败则缩短至 2 ms 快速补刷。
- `main/modules/oled/OledCmds.cpp` — DEV 档 `oled [status]` 新增 `fps`、`dirty_pages`（`last`/`total`）、`i2c_us`（`last`/`max`）、`frame_us`（`last`/`max`）输出。
- `tools/main_host_check/CMakeLists.txt`、`MainHostCheck.cpp`、`OledDriverCheck.cpp`、`OledUiCheck.cpp`、`WebModuleCheck.cpp` — 接入 `OledAnim.cpp`/`OledRender.cpp`/`OledModuleCheck.cpp` 与 `gnss` 库；新增日志回忙（`power_oled_usb_log_rebusy`）、非整除 11 ms 轮询+真实任务唤醒（`power_oled_usb_wake_on_flush`/`deadline`）、关机降级掩码（`power_shutdown_degraded_masks`）、单页 8 ms 预算与锁争用注入（`oled_driver_page_lock_budget`）、真实 `OledModule` 终帧编排与 299/300 ms 正忙双击/休眠重算（`oled_module_*`）、分区域滚动数字断言与状态文案渲染校验，并移除 USB fixture 重启计数清零而在 U3 翻转前断言 0 次。
- `docs/adr/ADR-0020-oled-display.md` — 描述 OL2 UI、按页检查单击截止、无线收尾语义、降级后的强制下电/USB 重启及 8 ms 每页超时传参预算。

### 最小只读快照增补说明
1. `PowerSnapshot::{batteryAdcReady, chargerPresent, shutdownDoneMask, shutdownDegradedMask}`：让自检页与关机进度页在不直接调用 ADC/SGM41513 驱动的前提下读到真实状态、真实完成里程碑与降级结果。
2. `DataCenter::wifiReady`（无锁 `std::atomic<bool>`）：让自检页在 Wi-Fi 配置模式下按真实 SoftAP 启动状态判定射频就绪。
3. `UiEventBus::{setOledActive, markShutdownFinalFrameFlushed, resetShutdownFinalFrame, setRestartWakeHook, notifyShortKey(ms), popShortKey}`：提供 USB 关机终帧完成唤醒与消抖短按生产时间戳无锁 SPSC 队列。

---

## 2. 验证与门禁结果

执行命令：
```sh
export IDF_PATH=~/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.1_py3.13_env
export PATH="$PWD/builds/ol5-review-probes:$PATH" UV_OFFLINE=1
ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol8 --base 78e5801
```
第三轮复跑 **43 PASS / 0 FAIL**。首跑 G6 未识别 `flush()` 的声明注释，G5b 报 `flush()` 分支复杂度超限。将注释移到定义处，并让写入成功分支提前进入下一页后，两项均通过。 [复跑日志](../../builds/ol8-logs/fix3-gates.log) · [首跑日志](../../builds/ol8-logs/fix3-gates-initial.log)

| 门禁 / 检查项 | 结果 | 关键证据 / 备注 |
| --- | --- | --- |
| `G1-frozen`（`sha256sum -c docs/frozen.sha256`） | **PASS** | 全部冻结文件零改动 |
| `G2-main_host_check` / `G2b-main_host_check-asan` | **PASS** | 普通和 ASan/UBSan 各 159/159 |
| 其余 13 组主机套件（normal + ASan/UBSan，共 26 项） | **PASS** | `host`/`app_fw`/`services`/`stream_demux`/`bt_reconnect`/`cli`/`gnss`/`gnss-all`/`nvs_store`/`web_proto`/`settings`/`ota`/`t13_host_check` 全过 |
| `G5-format`（`clang-format --style=file --dry-run --Werror`） | **PASS** | 0 格式违规 |
| `G6-doc_check`（`tools/doc_check/doc_check.py --base 78e5801`） | **PASS** | 改动函数 138 个，不合格 0 个 |
| `G3-idf`（默认配置，0 warning） | **PASS** | `rtk_main.bin` 1,560,144 B，分区剩余 21%（405,936 B） |
| `G3-idf-trace-off` / `wifi-off` / `ota-off` / `production` / `console-none` / `none-wifi` | **PASS** | 6 个条件编译变体全部 0 warning |
| `G3b-rtk_pure-esp32` / `G3b-rtk_pure-esp32c3` | **PASS** | Xtensa 与 RISC-V 纯逻辑测试应用均 0 warning |
| `G5b-tidy`（`tools/tidy_check/tidy_check.py`） | **PASS** | 文件 158 个，发现 0 条（`WarningsAsErrors: '*'`） |
| `G7b-runtime`（`tools/g7b_check/g7b_check.py`） | **PASS** | 零运行期堆、零异常/RTTI、零浮点 `printf` |
| `G7c-no-cli-deps` | **PASS** | NONE 档未链入 `oled` 等 CLI 命令 |
| HIL 实板验收（P2 v1.41） | **NOT_RUN** | 任务卡禁止烧录/碰串口，待实板按 §5 验收 |

---

## 3. 体积、DRAM、IRAM 与任务栈分析

### 整机镜像对比（同一 `sdkconfig`）

| 指标 | OL1 基线 `78e5801` | 第三轮 `builds/ol8-idf` | 增量 |
| --- | ---: | ---: | ---: |
| `rtk_main.bin` | 1,544,384 B | 1,560,144 B | **+15,760 B** |
| Flash Code | 1,106,716 B | 1,118,072 B | +11,356 B |
| Flash Data | 269,068 B | 273,324 B | +4,256 B |
| IRAM | 116,139 B | 116,139 B | **0 B** |
| DRAM 静态合计 | 97,379 B | 97,651 B | **+272 B**（预算 ≤1 KiB） |
| OLED BootArena 外供 | 4,096 B | 4,096 B | 0 B |

应用分区余 21%。OLED 外供仍为 3,072 B 任务栈和 1,024 B 帧缓冲。 [整机尺寸](../../builds/ol8-logs/elf-size-comparison.log)

### OLED / WouoUI 目标文件明细

| 目标文件 | Flash Code | Flash Data | 静态 DRAM | IRAM |
| --- | ---: | ---: | ---: | ---: |
| `OledRender.cpp.obj` | 6,450 B | 281 B | 0 B | 0 B |
| `WouoUI_font.c.obj` | 0 B | 5,546 B | 0 B | 0 B |
| `OledUi.cpp.obj` | 4,341 B | 32 B | 0 B | 0 B |
| `OledModule.cpp.obj` | 2,718 B | 72 B | 378 B | 0 B |
| `WouoUI_graph.c.obj` | 2,181 B | 0 B | 12 B | 0 B |
| `Sh1106.cpp.obj` | 1,425 B | 0 B | 0 B | 0 B |
| `OledAnim.cpp.obj` | 998 B | 0 B | 0 B | 0 B |
| `OledCmds.cpp.obj` | 474 B | 40 B | 0 B | 0 B |

`s_ui_ctrl` 120 B、`s_restart_task_handle` 4 B、`s_flush_deadline_ms` 8 B，均在 `.dram0.bss`。 [目标文件尺寸](../../builds/ol8-logs/elf-file-sizes.log)

### OLED 任务栈

`ol8-idf` 的 `refreshOnce()` 可见入口帧为 384 B。采集链含卫星汇总的入口帧总计 1,648 B，表盘渲染链到 `renderDialplate` 为 992 B。采集先返回，再进入渲染与 I2C 发送。保持 3,072 B 栈配置，目标板 `stack_hwm` 仍为 `NOT_RUN`。 [反汇编栈帧](../../builds/ol8-logs/elf-stack-frames.log)

---

## 4. I2C 帧耗时估算（400 kHz SH1106）

- 总线参数：`I2C_NUM_0` @ 400 kHz（每字节 9 clock = 22.5 us）。
- 单页传输（`Sh1106::writePage`）：
  - 页地址命令事务：总线 5 B（地址 1 + 控制字节 1 + 命令字节 3）≈ 0.11 ms，加软件开销估算约 **0.25 ms**。
  - 页数据事务：130 B（地址 1 + 控字节 `0x40` + 128 B 像素）≈ 2.93 ms + 驱动开销 ≈ **3.1–3.4 ms**。
  - 健康总线单页持锁估算 **~3.4–3.7 ms**。每页超时传参预算 **8 ms**。数据段使用命令执行后的剩余整毫秒，剩余不足 1 ms 时跳过并恢复总线。驱动分段等待与错误恢复可能超出该预算，最大持锁时长需实板测量。Charger 继续用 0 tick 尝试拿锁。
- 整帧耗时估算：
  - **静止画面**（0 脏页）：**0 ms**（`FrameTracker` 跳过，0 写 I2C）。
  - **局部动画**（数字滚动 / 底部弹簧选中框，2–3 脏页）：**~7–11 ms**，在 33 ms 帧周期内轻松稳定达到 **30 fps**。
  - **全屏过渡**（300 ms 滑动视差 / 图标展开全 8 脏页）：**~27–30 ms**，配合扣除帧耗时的 33 ms 调度可达 **~28–30 fps**（约 9–10 帧完成 300 ms 过渡）。
- 上述为基于 400 kHz 协议时序的理论估算，实板以 `oled` 命令输出的 `fps`、`dirty_pages`、`i2c_us`、`frame_us` 为准（`NOT_RUN`）。

---

## 5. HIL 验收步骤（`NOT_RUN`，待实板执行）

1. **开机 Logo 与自检页**：P2 v1.41（无 USB）长按开机；确认单色 Logo 逐列平滑展开 → `SYSTEM CHECK` 逐项推进（`PWR/BAT`、`NVS`、`BT SPP`、`CHARGER`、`GNSS` 含型号/固件短串）→ 300 ms 滑动视差切入表盘；确认无充电芯片板子 `CHARGER` 显示 `-` 且状态栏不挂告警图标；拔掉或禁用 GNSS 时不阻塞开机，进入表盘后顶栏出现三角告警图标。
2. **按键方案 A 与表盘/子菜单动效**：
   - 短按产生后开启 300 ms 窗口，窗口到期时验证选中框推进。若当前 I2C 页跨过截止，验证完成该页后立即判定并重新渲染。
   - 双击（间隔 < 300 ms）：从选中图标矩形平滑展开进入对应子菜单；在卫星子菜单内短按切换 `SATS SYS`（星座柱状图）与 `SATS CN0`（Top-8 载噪比条）；在任意子菜单双击平滑收回返回表盘。
   - 确认 2.5 s 长按直接触发关机，松开绝不误判为短按/双击。
3. **实时数字滚动与静止 0 写**：
   - 定位后观察顶栏卫星数与电量百分比变化时具备竖向滚动动效，表盘大号解状态按 `NONE/SPS/DGPS/FLOAT/FIX` 或 `BASE/SVY` 实时刷新；位置页经纬高/HDOP/差分龄期与串口 GNSS 输出一致。
   - 静置表盘 10 s 后执行 `oled`：确认 `fps=0.0`、`frames skipped` 持续增长而 `frames flushed` 与 `totalDirtyPages` 不再增长、`i2c_errors=0`。
   - 动画交互期间用 `oled` 记录 `fps`、`dirty_pages`、`i2c_us`、`frame_us`，用 `sys tasks` 记录 `"oled"` 的 `stack_hwm`，用 `power charger` 确认充电采样可用。
   - 单独测量共享锁从获取到释放的时间，记录健康总线、NACK 和传输超时下的最大单页持锁时间及恢复持锁时间。`oled` 的 `i2c_us` 统计整帧。
4. **关机进度（无 USB 与有 USB）**：
   - **无 USB**。长按 ≥ 2.5 s 或 `power off`，确认保存、无线收尾和断电文案按阶段显示，屏幕保持至硬件断电。蓝牙按 `CLOSING SPP` / `SCAN REQ OK` 检查，SoftAP 按 `CLOSING AP` / `AP MODE OFF` 检查。
   - **有 USB**：长按 ≥ 2.5 s 或 `power off`，确认进度推进至 U3 关断后清晰显示 `POWERED OFF` / `CHARGING...` / `100%`，随后重启进入 `charge_standby` 充电待机页（短按无效，长按 2.5 s 正常开机）。
   - **30 s 超时回退（故障注入）**：模拟 U3 无法拉低触发 30 s 超时，确认屏幕自动恢复表盘。
5. **无 OLED 板型回归（E1）**：确认 E1 启动不触碰 GPIO12、`oled` 显示 `enabled=no present=no`、USB 关机重启增加 0 ms 等待。

---

## 6. 与契约的偏离及未验证风险

- **既定策略例外与边界说明**：
  1. 保存超时（1 s）、无线收尾超时（BT 150 ms / SoftAP 5.5 s）或安全输出错误时，沿用已授权的强制下电策略。若 USB 在位且 U3 已关断，则走 USB 重启。OLED 以独立降级位展示 `SAVE TIMEOUT` / `RF TIMEOUT` / `SAFE IO ERR` 与 `!`。蓝牙 `R:+` 指 SPP 已断开且扫描关闭请求已提交，OLED 对应提示为 `SCAN REQ OK`。
  2. 每页超时传参预算为 8 ms。同步 I2C 驱动分段等待与错误恢复可超出，最大单页持锁须在实板测量（`NOT_RUN`）。
  3. USB 终帧绝对截止是日志放行后 30 ms。等待前后检查截止，代码每次只挂一 tick，相对等待引入的调度误差 ≤1 tick（当前 1 ms）。若任务被抢占至截止之后，恢复运行后在重新检查截止时立即重启。实板端到端时序为 `NOT_RUN`。
- **未验证风险**：
  1. 实板视觉效果（14×14 图标清晰度、Logo 展开节奏、32 点进度环可读性、30 fps 实测帧率、最大单页持锁时长与 `i2c_us` 最大值）未经硬件观测（`NOT_RUN`）。
  2. `"oled"` 任务运行期 `stack_hwm` 仅完成静态入口帧分析（见 §3），需实板经 `sys tasks` 实测确认 newlib `snprintf` 与 IDF I2C 驱动叠加后的最终余量（`NOT_RUN`）。
  3. USB 关机终帧刷出到 `esp_restart()` 的视觉停留时长取决于 `esp_restart()` 复位后 bootloader 重新初始化前的屏幕保持时间（SH1106 在 GPIO12 未拉低且复位后未重配前保持末帧内容），需实板确认体感清晰度（`NOT_RUN`）。

---

## 7. 独立审查修复记录（`OL2-review.md`：`ROL2-01`～`ROL2-07`）

| 编号 | 级别 | 修复内容与位置 | 对应测试与变异核验（删/破坏实现后实测失败证据） |
| --- | --- | --- | --- |
| `ROL2-01` | Major | `PowerModule.hpp` 新增 `oledWaitStarted_`/`oledWaitStartMs_`；`PowerModule.cpp` 在 `restartPending_` 满足 `(elapsed >= kRestartWaitMs \|\| isLogDrained(elapsed))` 后独立计时 `kOledFinalFlushMaxMs = 30 ms` 上限；`!oledActive()` 或 `shutdownFinalFrameFlushed()` 时额外等待 0 ms 直接 `esp_restart()`。 | 新增 4 组场景 `power_oled_usb_final_flush`、`power_oled_usb_final_timeout`、`power_oled_usb_absent_no_wait`、`power_oled_usb_already_flushed` 并收紧 `power_short_key_ui`。<br>**变异 1**（删除 `waitOled`/`oledWaitStartMs_` 逻辑）：`power_short_key_ui`（L448/450）、`power_oled_usb_final_flush`（L487/489）、`power_oled_usb_final_timeout`（L501/503）全部 FAIL。<br>**变异 2**（忽略 `oledActive`/`shutdownFinalFrameFlushed` 强等 30 ms）：`power_short_key_ui`（L453）、`power_oled_usb_final_flush`（L492）、`power_oled_usb_absent_no_wait`（L516）、`power_oled_usb_already_flushed`（L530）全部 FAIL。 |
| `ROL2-02` | Major | 按审查意见补齐主机测试与渲染分支覆盖：① 4 组 USB 终帧握手边界场景（见 `ROL2-01`）；② `OledUiCheck` 短按 250 ms 后断言 `dec.nextWaitMs == 50U`；③ 以活跃 `SystemCheckSm`（覆盖 `kOk`/`kFail`/`kAbsent`/`kPending` + `waitS >= 1 s` + GNSS 型号固件）和活跃 `NumberRoll`（增/减方向中点帧）调用 `renderUi()`，覆盖普通与 ASan/UBSan。 | **变异（删除 `OledUi.cpp` `if (click_.pending())` 窗口缩短）**：`OledUiCheck:334`（`dec.nextWaitMs == 50U`）FAIL。<br>**变异（禁用 `OledRender.cpp` `waitS>=1`、GNSS 固件拼接及 `drawRollingValue` 滚动分支）**：`OledUiCheck:597`、`610`、`630` 三处 `memcmp != 0` 全部 FAIL。 |
| `ROL2-03` | Major | `Sh1106.hpp` 新增 `clearBackoff()`；`OledUi.hpp/.cpp` 新增 `FlushOutcome`、`PostFlushDecision` 与 `settleFrameWait()`；`OledModule.cpp` 在 `dec.markShutdownFinalFrame` 时先 `clearBackoff()` 再刷屏，仅 `kWritten`/`kSkipped` 置位 `markShutdownFinalFrameFlushed()`，`kError` 不置位并缩短等待至 `kShutdownFinalRetryMs = 2 ms` 快速补刷。 | `OledUiCheck` 校验 `settleFrameWait` 全部 4 种 `FlushOutcome` 与动画耗时扣减；`OledDriverCheck::runTimeoutAbortResetBackoff` 校验 `clearBackoff()` 立即解除退避。<br>**变异（`settleFrameWait` 忽略 `outcome` 无条件置位；`clearBackoff()` 改为空实现）**：`OledUiCheck:421/423` FAIL，`OledDriverCheck:240`（`panel.flush(fb) == kWritten`）FAIL。 |
| `ROL2-04` | Minor | `OledAnim.cpp` `FixedSpring::step()` 记录步前 `prevPos`/`prevVel`，若整步更新无变化且 `absErr <= kScale` 强制吸附 `targetQ8_` 并置静止；Python 穷举 `dt=1..64 ms × 104` 组正负位移 0 失败。 | `OledUiCheck` 增补 `dt={1,2,16,33} ms × delta={1,5,10,32,96,-10,-96}` 逐帧单调性与收敛断言。<br>**变异（删除 `stalled \|\|` 判断）**：`OledUiCheck:206`（`sp.settled() && sp.position() == targetPx`）4 处 FAIL（`dt=1 ms, delta=1/5/10/-10`）。 |
| `ROL2-05` | Minor | `OledUi.hpp` 提取 `isGnssRunningLike(gnss::State)`（`kRunning \|\| kReadback`）供 `SystemCheckSm`、`UiController`、`drawStatusBar` 与 `renderDialplate` 共用；`dialHeroBadge` 完整区分 9 个枚举值（`kPowerUp -> "BOOT"`、`kOutput`/`kMaintenance -> "CFG"`、`kReadback` 沿用运行态）。 | `OledUiCheck` 覆盖全部 9 个 `gnss::State` 及 `kReadback` 与 `kRunning` 表盘逐字节一致渲染校验。<br>**变异（回退 `dialHeroBadge` 仅识别 4 态）**：`OledUiCheck:474/476/482/484/640` 全部 FAIL。 |
| `ROL2-06` | Minor | `OledUi.cpp` `UiController::finishActiveStep()` 在离开自检页后持续监测 `vm.gnssEnabled && vm.gnssState == kFailed` 并补置 `hasWarning_ = true`。 | `OledUiCheck` 增补 Logo/自检期 GNSS `kProbing` 双击跳过后转 `kFailed` 补挂告警断言。<br>**变异（删除该判断）**：`OledUiCheck:411`（`skipCtrl.hasWarning()`）FAIL。 |
| `ROL2-07` | Minor | 删除 `UiController`/`OledModule` 中不可达的 `poweredOff_`/`wakeRetryActive_`/`wakeFailedAtMs_`/`onWakeFailed`/`shouldWake`/`kWakeRetryBackoffMs`；将 `FixedSpring::settled_` 改为 `moving_ = false`、`PageTransition::durationMs` 默认改为 `0U`，使 `s_ui_ctrl` 真正全零落 `.dram0.bss`；同步修正 `ADR-0020` §5.3 描述。 | `xtensa-esp32-elf-nm -S builds/ol4-idf/rtk_main.elf` 实测 `s_ui_ctrl` 为 `0x7c` B `b`（`.dram0.bss`），OLED 目标文件 `.data = 0 B`；全量 43 项门禁（含 `G5b-tidy` 0 条）通过。 |

---

## 8. 第二轮独立复审修复记录（`OL2-review2.md`：`R2OL2-01`～`R2OL2-06`）

| 编号 | 级别 | 修复内容与位置 | 对应测试与变异核验（删/破坏实现后实测失败证据） |
| --- | --- | --- | --- |
| `R2OL2-01` | Major | `PowerModule.hpp/.cpp` 新增 `restartLogReady_`、`oledWaitDeadlineMs_`、`s_restart_task_handle`、`wakeRestartWait()`、`waitOledFinalFrame()`；`OledDef.hpp` 新增 `UiEventBus::setRestartWakeHook()` 并在 `markShutdownFinalFrameFlushed()` 末尾触发。首次日志排空或达到原日志上限即锁存 `restartLogReady_` 并设定绝对截止 `nowMs + 30 ms`，此后独立检查终帧完成与绝对截止；真实任务路径通过 `ulTaskNotifyTake` 阻塞至终帧完成钩子或精确剩余毫秒截止直接重启，消除日志回忙拖尾与 10/11 ms 轮询量化误差。 | `MainHostCheck.cpp` 新增 `power_oled_usb_log_rebusy`、`power_oled_usb_wake_on_flush`、`power_oled_usb_wake_on_deadline`（11 ms 非整除轮询 + 真实 FreeRTOS 模拟任务）。<br>**变异 1a**（移除 `restartLogReady_` 锁存）：`power_oled_usb_log_rebusy` 在 `MainHostCheck.cpp:576` FAIL。<br>**变异 1b**（移除 `waitOledFinalFrame()` 直接唤醒）：`power_oled_usb_wake_on_flush`（`L624`）与 `power_oled_usb_wake_on_deadline`（`L624/L630`）全部 FAIL。 |
| `R2OL2-02` | Major | `PowerLogic.hpp` 新增 `ShutdownDegradedBit`；`SystemDef.hpp` 新增 `PowerSnapshot::shutdownDegradedMask`；`PowerModule.cpp` 改为在真实完成点记录 `saveFencesDone_`/`saveFlushedDone_`/`peripheralsSafeDone_`/`radioStoppedDone_`，超时/安全电平失败分别记入 `shutdownDegradedMask`；`OledUi.cpp`/`OledRender.cpp` 按降级位渲染 `SAVE TIMEOUT`/`RF TIMEOUT`/`SAFE IO ERR` 与 `S/R/P` `!` 标记；在 `ADR-0020` §5.3 与本报告 §6 明确记录基线强制下电/USB 兜底重启策略例外。 | `MainHostCheck.cpp` 新增 `power_shutdown_degraded_masks`；`OledUiCheck.cpp` 新增降级文案与渲染差异断言。<br>**变异 2a**（`computeShutdownDoneMask` 恢复按阶段无条件置位）：`power_shutdown_degraded_masks` 在 `MainHostCheck.cpp:653/655/666/679/681/683` 全部 FAIL。<br>**变异 2b**（`shutdownStageText` 忽略 `degradedMask`）：`oled_ui_check` 在 `OledUiCheck.cpp:420/425/431` 全部 FAIL。 |
| `R2OL2-03` | Major | `OledDef.hpp` 将短按事件改为无锁 SPSC 时间戳队列（`kShortKeyQueueCap = 8`，`notifyShortKey(nowMs)`/`popShortKey()`）；`PowerModule.cpp` 投递消抖短按产生时刻；`OledAnim.hpp/.cpp` `ClickDetector` 改用生产时间戳 `onPress(pressMs)` / `pollTimeout(nowMs)` / `deadlineMs()` / `remainingMs()`；`OledUi.cpp` 先顺序消费全部时间戳再判截止，并由 `settleFrameWait(..., postFlushNowMs)` 按绝对截止 `clickDeadlineMs - postFlushNowMs` 重算休眠（可降为 0 ms）；`OledModule.cpp` 在刷屏后取 `rtos::nowMs()` 并支持 `waitMs == 0` 非阻塞重入。 | `OledModuleCheck.cpp` 新增 `busy_click_299_300_and_deadline_sleep`（OLED 写页正忙覆盖 299 ms 双击、300 ms 拆单击及 20/55 ms 忙帧休眠重算）；`OledUiCheck.cpp` 覆盖时间戳队列与 `settleFrameWait` 截止截断。<br>**变异 3a**（`OledUi.cpp` 改用消费时刻 `nowMs` 替代 `pressMs`）：`oled_module_busy_click_299_300_and_deadline_sleep`（`OledModuleCheck.cpp:260`）与 `oled_ui_check`（`L386/L395`）全部 FAIL。<br>**变异 3b**（`settleFrameWait` 删除 `hasClickDeadline` 截断）：`oled_module_busy_click_299_300_and_deadline_sleep`（`L301`）与 `oled_ui_check`（`L511/L512`）全部 FAIL。 |
| `R2OL2-04` | Major | `Sh1106.hpp/.cpp` 明确 当轮实现的每页 8 ms 超时参数分配（第三轮按传参预算验收，见 §9）；`writePage()` 记录取锁起始 `esp_timer_get_time()`，页地址命令完成后按剩余微秒换算数据段超时（剩余 < 1 ms 直接报 `ESP_ERR_TIMEOUT` 中止数据段并触发总线恢复）；页地址命令和数据使用剩余预算。 | `OledDriverCheck.cpp` 新增 `oled_driver_page_lock_budget`（注入命令迟到 5 ms → 数据超时 3 ms、命令耗尽 8 ms → 跳过数据 + `resetBus`、数据超时 6 ms + `resetBus`、锁争用 → `kError` 无 `resetBus` 且 Charger 返回 `kBusy`）。<br>**变异 4**（`writePage` 数据段改为固定传 8 ms）：`oled_driver_page_lock_budget` 在 `OledDriverCheck.cpp:289`（`s_last_data_timeout_ms == 3`）FAIL。 |
| `R2OL2-05` | Major | ① `Sh1106.hpp/.cpp` 与 `OledUi.hpp/.cpp` 区分 `kBackoff` 与 `kSkipped`，`kBackoff` 永不置终帧位；② 新增 `OledModuleCheck.cpp` 编译真实 `OledModule.cpp` 经 I2C 替身验证连续错→退避→清退避→部分页失败重试→严格先刷屏后置终帧位→0 脏页跳过；③ `MainHostCheck.cpp` 移除 USB fixture 中 `s_restarts = 0U` 清零并在 U3 翻转前断言 `s_restarts == 0U`；④ `OledUiCheck.cpp` 拆分顶栏卫星区（`x=10..30`）与电量区（`x=80..108`）独立断言滚动方向与终值。 | `oled_module_final_frame_orchestration`、`oled_ui_check`、`main_*_power_*`。<br>**变异 5a**（删 `OledModule.cpp` `clearBackoff()`）：`oled_module_final_frame_orchestration` 在 `OledModuleCheck.cpp:203` FAIL。<br>**变异 5b**（将 `markShutdownFinalFrameFlushed()` 提前到 `WouoUI_BuffSend()` 之前）：`oled_module_final_frame_orchestration` 在 `OledModuleCheck.cpp:204` FAIL。<br>**变异 5c**（分别关闭 `OledRender.cpp` `satsRoll` / `batRoll`）：`oled_ui_check` 分别在 `OledUiCheck.cpp:712/718` 与 `L737/L742` 独立 FAIL。 |
| `R2OL2-06` | Minor | `OledUi.hpp/.cpp` 新增 `DialGnssDetail` 与 `dialGnssDetail(vm)`，按非运行态精确输出 `DISABLED`/`POWER UP`/`PROBING`/`CONFIG`/`SET OUT`/`MAINT`/`FAULT`（不再统称 `GNSS WAIT`）；`OledRender.cpp` 表盘右侧改用 `dialGnssDetail(vm)`。 | `OledUiCheck.cpp` 校验全 9 态右侧文案并验证 `kOutput` 与 `kMaintenance` 表盘右侧像素差异。<br>**变异 6**（`dialGnssDetail` 回退为恒定 `"WAIT"`）：`oled_ui_check` 在 `OledUiCheck.cpp:758..774`（含像素差异断言 `L772`）全部 FAIL。 |

---

## 9. 第三轮复审修复记录（OL2-review3.md）

| 编号 | 修复 | 测试与变异核验 |
| --- | --- | --- |
| `R3OL2-01` | 每页超时传参预算 8 ms，命令耗时从数据段超时中扣减。同步驱动分段等待与恢复可超出，实板测最大持锁。IDF 返回 `ESP_ERR_TIMEOUT` 或 `ESP_ERR_INVALID_RESPONSE` 时，先释放页锁，再尝试总线复位。 | `oled_driver_page_lock_budget` 及 `oled_driver_internal_timeout_recovery` 通过。删掉 `INVALID_RESPONSE` 恢复分支后，后者在总线复位计数断言处失败。 |
| `R3OL2-02` | 日志放行后按 30 ms 绝对截止等待终帧。每次等待前重读时钟，每次恢复后重新检查。附加相对等待误差 ≤1 tick。终帧通知保留。 | 首次等待前分别注入 5 ms 与 31 ms 抢占间隙。恢复整段相对等待时两组超时上限断言均失败。改为首次 tick 超时就重启时，5 ms 用例的等待下限断言失败。 |
| `R3OL2-03` | 发每个脏页前检查单击截止，到期后保留已提交页并回到 UI。先处理队列中的时间戳再确认单击，跨截止的在途页写完后可立即推进。 | 正常电量动画中注入第二次短按，299 ms 进卫星子菜单，300 ms 留在表盘，均只发送一个跨截止的脏页。移除驱动截止检查或断开模块截止传递时，两组均在页数断言处失败。先判到期再消费队列时，299 ms 导航断言失败。 |
| `R3OL2-04` | 蓝牙 `R:+` 表示 SPP 已断开且扫描关闭请求已提交，提示 `SCAN REQ OK`。SoftAP 停用显示 `AP MODE OFF`。仍用现有无线收尾判据与等待。 | `oled_ui_check` 校验两种模式的阶段提示。把蓝牙完成提示改回 `STOP PERIPH` 后，蓝牙文案断言失败。 |
| `R3OL2-05` | 实际传输失败的页摘要失效。下一帧恢复旧内容时仍重写该页，其余已提交页可跳过。 | `oled_driver_partial_page_revert` 模拟 `0xAA → 0x55` 的部分数据到达后报错。恢复 `0xAA` 时只补写一页。移除摘要失效时，驱动返回 `kSkipped`，重写断言失败。 |
| `R3OL2-06` | 恢复进入 `kCutPower` 时记录起点，`kShutdownAbort` 以该点计算耗时。 | `main_*_power_timeout_recovery` 同时断言 GPIO4 释放、恢复 `kIdle` 和 trace `30000 ms`。去掉入口记录时，仅 trace 耗时断言失败。 |

9 个变异均用隔离的翻译单元和正常主机构建对象重链。工作区产品源码未用于制造变异。 [变异明细](../../builds/ol8-logs/fix3-mutations.log) · [定向测试](../../builds/ol8-logs/fix3-focused.log)
