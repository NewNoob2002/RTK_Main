# OL2 第三轮独立复审 · FAIL

正式门禁 **43/43 PASS**。任务验收 **FAIL**。I2C 单页持锁仍可超过 8 ms，USB 终帧等待仍可超过 30 ms，同步刷屏可推迟 300 ms 单击判定。另有蓝牙完成位语义、失败页缓存和 30 秒回退诊断三项缺口。

复核范围为 `78e5801` 至未提交工作区，包含未跟踪的 OLED 源文件和测试。按 ESP-IDF / C++ T2 工程规则核对任务卡、第二轮复审及实施报告 §6、§8。保存、射频或安全输出收尾降级后继续强制下电及 USB 重启，按本次已授权策略验收。 [任务卡](../tasks/OL2-oled-ui.md) · [第二轮复审](OL2-review2.md) · [实施报告 §6](OL2-oled-ui.md#6-与契约的偏离及未验证风险) · [实施报告 §8](OL2-oled-ui.md#8-第二轮独立复审修复记录r2ol2-01r2ol2-06)

## 发现

### R3OL2-01（Major）单页 8 ms 仍是传参预算

`writePage()` 现在扣掉页地址命令的实际耗时，数据段拿到剩余预算。锁仍覆盖整个同步传输及其错误处理。ESP-IDF v6.1 对 FIFO 分段逐次用同一个 `ticks_to_wait` 等待命令信号量，随后再用它等结束事件。同步错误路径还会在返回前复位硬件 FSM。因此一次调用累计耗时可超过传参值。命令用掉 2 ms、数据段传入 6 ms 时，数据可先经历一个 3 ms 的子等待，再在后续子等待用完 6 ms，单页已达 11 ms。主机用 8.4 ms 数据返回延迟模拟多阶段耗时，真实 `Sh1106` 持锁 **10.4 ms**，且接受了返回成功的迟到写入。该数值为故障注入。 [任务卡 §D](../tasks/OL2-oled-ui.md#d-动画与性能) · [Sh1106.cpp:117](../../main/board/Sh1106.cpp#L117) · [IDF 分段发送与等待](/home/gtc/esp/esp-idf-v6.1/components/esp_driver_i2c/i2c_master.c#L529) · [IDF 同步错误处理](/home/gtc/esp/esp-idf-v6.1/components/esp_driver_i2c/i2c_master.c#L1014) · [探针日志](../../builds/ol7-logs/review3-probes.log)

现有预算用例让替身在传参上限内返回，在该模型下通过。目标驱动也可能把内部传输超时报成 `ESP_ERR_INVALID_RESPONSE`，从而走 `kIoError`，与用例注入 `ESP_ERR_TIMEOUT` 后触发应用层 `resetBus()` 的路径有别。Charger 的 0 tick 拿锁仍立即返回 `kBusy`。要兑现 8 ms，应使用能按同一截止结束页传输和释锁的路径，并覆盖驱动分段等待及恢复开销。 [OledDriverCheck.cpp:270](../../tools/main_host_check/OledDriverCheck.cpp#L270) · [IDF 返回码](/home/gtc/esp/esp-idf-v6.1/components/esp_driver_i2c/i2c_master.c#L723) · [EspStatus.cpp:18](../../components/rtos/EspStatus.cpp#L18) · [Charger.cpp:149](../../main/board/Charger.cpp#L149)

### R3OL2-02（Major）USB 重启的相对等待可越过绝对截止

日志放行锁存和终帧通知已生效。等待函数先读单调时钟计算 `remMs`，再以相对 tick 调用 `ulTaskNotifyTake()`。如果在读时钟与挂等待之间被抢占，相对等待会在恢复后才开始。注入这段间隙 **5 ms**，终帧始终未完成，真实 `PowerModule` 仍请求等 30 tick，放行到 `esp_restart()` 为 **35 ms**。现有 11 ms 轮询用例能抓到移除任务等待的变异，超时断言允许 25–80 ms，覆盖不到此次越界。任务通知可继续用于提前完成。截止路径需要以同一绝对时刻挂定时唤醒，并在相对等待入口与任务恢复时覆盖调度延迟。 [PowerModule.cpp:762](../../main/modules/power/PowerModule.cpp#L762) · [PowerModule.cpp:829](../../main/modules/power/PowerModule.cpp#L829) · [MainHostCheck.cpp:584](../../tools/main_host_check/MainHostCheck.cpp#L584) · [探针日志](../../builds/ol7-logs/review3-probes.log)

### R3OL2-03（Major）正常动画写页可跨过单击判定截止

生产时间戳已修正 OLED 晚消费造成的双击误拆。刷完后的剩余时间也会重算。单击判定仍只在 `UiController::step()` 中进行。用真实 `OledModule`、变更电量触发数字滚动、每页 3 ms 的替身运行正常脏页刷新，短按在 `40000 ms`，最后一轮从 `40299 ms` 开始写两个脏页，到 `40305 ms` 才返回 `wait=0`。下一轮最早在 **305 ms** 判定单击。这条路径无需强制全刷，现有测试把跨截止后返回零等待视为通过。若保留 300 ms 硬约束，应在相邻写页之间处理到期输入或将按键判定与同步刷新分离，同时保持截止前生产的第二次短按优先。 [任务卡按键方案 A](../tasks/OL2-oled-ui.md#用户已定决策) · [OledModule.cpp:313](../../main/modules/oled/OledModule.cpp#L313) · [OledUi.cpp:470](../../main/modules/oled/OledUi.cpp#L470) · [OledModuleCheck.cpp:303](../../tools/main_host_check/OledModuleCheck.cpp#L303) · [探针日志](../../builds/ol7-logs/review3-probes.log)

### R3OL2-04（Medium）蓝牙完成位仍早于扫描静默确认

现有 `powerPauseDone()` 以 SPP 会话已关闭、应用已拒绝新会话和 GAP 扫描关闭请求已提交为完成条件。OL2 将这个结果直接显示为 `R:+` 与 `kRadioStopped`。IDF 的 `esp_bt_gap_set_scan_mode()` 返回的是向 BTC 投递成功，真正执行可发现、可连接模式切换发生在后续任务，后续 BTA 消息分配失败也不会反映到该 `ESP_OK`。主机探针在无 SPP 会话、仅模拟提交成功时即得到 RF 完成位。若完成位要求实际扫描静默，需要等生效证据。也可将 OLED 指标明确限定为 SPP 断连和扫描关闭请求已提交。保存失败、射频暂停未完成及安全 GPIO 设置失败的三个降级位均通过独立注入核实。 [PowerModule.cpp:692](../../main/modules/power/PowerModule.cpp#L692) · [BtLinkModule.cpp:580](../../main/modules/bt_link/BtLinkModule.cpp#L580) · [SppServerWorker.c:44](../../components/spp_server/SppServerWorker.c#L44) · [IDF GAP 提交](/home/gtc/esp/esp-idf-v6.1/components/bt/host/bluedroid/api/esp_gap_bt_api.c#L35) · [IDF BTA 投递](/home/gtc/esp/esp-idf-v6.1/components/bt/host/bluedroid/bta/dm/bta_dm_api.c#L585) · [探针日志](../../builds/ol7-logs/review3-probes.log)

### R3OL2-05（Medium）部分写入失败后，零脏页可能掩盖不同的屏幕内容

失败页保留上次成功的哈希及有效位。若面板已接收部分字节后传输报错，下一帧又回到上次成功内容，比较会返回 `kSkipped`，脏页也不再补写。探针先写 `0xAA`，模拟面板收到 `0x55` 后报超时，再请求 `0xAA`，实得 `kSkipped`、零事务、模拟面板仍为 `0x55`。终帧后处理接受 `kSkipped` 作为确认，因此它依赖有效位能代表真实屏幕内容。该缓存缺口继承自基线。使传输失败页失效即可保留其他已提交页和静止零写的收益。 [Sh1106.cpp:295](../../main/board/Sh1106.cpp#L295) · [OledUi.cpp:500](../../main/modules/oled/OledUi.cpp#L500) · [OledUi.cpp:756](../../main/modules/oled/OledUi.cpp#L756) · [探针日志](../../builds/ol7-logs/review3-probes.log)

### R3OL2-06（Minor）30 秒回退 trace 丢失断电起点

`handleShutdownStageEntry()` 中 `case kCutPower` 被删除，`cutStartMs_ = nowMs` 留在上一分支的 `break` 后，永远不会执行。`ShutdownSm` 自己的截止仍准时生效。主机记录进入断电为 `160 ms`、回退为 `30160 ms`，GPIO4 正常释放，状态回到 `kIdle`，无重启。`kShutdownAbort` 报告的耗时却为 `30160 ms`，会随进入关机前的运行时间增长。恢复 `kCutPower` 的记录分支并断言 trace 耗时即可。 [PowerModule.cpp:585](../../main/modules/power/PowerModule.cpp#L585) · [PowerModule.cpp:603](../../main/modules/power/PowerModule.cpp#L603) · [PowerLogic.cpp:328](../../main/board/PowerLogic.cpp#L328) · [探针日志](../../builds/ol7-logs/review3-probes.log)

## 上轮六项逐条复核

| 编号 | 结论 | 证据 |
| --- | --- | --- |
| `R2OL2-01` | 部分修复 | 日志回忙后仍继续等终帧，11 ms 非整除轮询的任务唤醒和超时路径通过。无 OLED 或已提前刷完均无额外等待。绝对硬上限仍有 R3OL2-02。 [MainHostCheck.cpp:485](../../tools/main_host_check/MainHostCheck.cpp#L485) · [探针日志](../../builds/ol7-logs/review3-probes.log) |
| `R2OL2-02` | 部分修复 | 三项失败并发注入时，USB 终帧 `done=0x63`、`degraded=0x07`，`S/R/P` 的三个成功位保持清零，U3 低后按授权策略重启一次。UI 三种降级文字分别可达，多项失败由 `S/R/P` 标记同时展示。蓝牙成功判据仍有 R3OL2-04。实施报告 §6 的 `200 ms` 与目标代码不符，BT 放行上限为 `150 ms`，SoftAP 为 `5.5 s`。 [PowerModule.cpp:35](../../main/modules/power/PowerModule.cpp#L35) · [PowerModule.cpp:539](../../main/modules/power/PowerModule.cpp#L539) · [OledUi.cpp:812](../../main/modules/oled/OledUi.cpp#L812) · [OledRender.cpp:827](../../main/modules/oled/OledRender.cpp#L827) |
| `R2OL2-03` | 部分修复 | 生产时间差 299 ms 判双击、300 ms 拆单击，延迟消费时仍正确。20 ms 忙帧后的休眠由 50 ms 缩到 30 ms。2.5 s 长按只触发一次长按事件，松开无短按。关机、待机的丢弃规则保持。实际判定晚于 300 ms 的边界见 R3OL2-03。 [PowerLogic.cpp:182](../../main/board/PowerLogic.cpp#L182) · [OledAnim.cpp:26](../../main/modules/oled/OledAnim.cpp#L26) · [OledModuleCheck.cpp:229](../../tools/main_host_check/OledModuleCheck.cpp#L229) |
| `R2OL2-04` | 部分修复 | 400 kHz、按页锁、Charger 0 tick 和命令后扣预算均已核实。8 ms 硬上限仍未成立，见 R3OL2-01。 [Sh1106.cpp:106](../../main/board/Sh1106.cpp#L106) · [OledDriverCheck.cpp:270](../../tools/main_host_check/OledDriverCheck.cpp#L270) |
| `R2OL2-05` | 覆盖修复 | 主机已链接真实 `OledModule`，覆盖退避、部分页补刷、确认晚于 I2C 及一致帧零事务。补齐 U3 翻转前的重启计数断言，两路滚动各有独立区域、方向和终值断言。独立复测 `kBackoff` 零事务且不确认，清退避后补写 8 页。部分写入后目标恢复旧内容的残余驱动问题见 R3OL2-05。 [OledModuleCheck.cpp:162](../../tools/main_host_check/OledModuleCheck.cpp#L162) · [MainHostCheck.cpp:464](../../tools/main_host_check/MainHostCheck.cpp#L464) · [OledUiCheck.cpp:698](../../tools/main_host_check/OledUiCheck.cpp#L698) |
| `R2OL2-06` | PASS | 非运行态渲染走 `POWER UP`、`PROBING`、`SET OUT`、`MAINT`、`FAULT` 等对应文案。`kRunning`/`kReadback` 保持定位数据路径。全九态和右侧像素差异用例通过。 [OledUi.cpp:729](../../main/modules/oled/OledUi.cpp#L729) · [OledRender.cpp:500](../../main/modules/oled/OledRender.cpp#L500) · [OledUiCheck.cpp:750](../../tools/main_host_check/OledUiCheck.cpp#L750) |

## 构建、内存和实板边界

| 验证 | 结果 |
| --- | --- |
| `ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol7 --base 78e5801` | **43 PASS / 0 FAIL**，退出码 0。使用本机 IDF v6.1 与本地 lizard 缓存。`main_host_check` 普通和 ASan/UBSan 各 **151/151 PASS**。 [门禁汇总](../../builds/ol7-logs/review3-gates.log) · [普通主机](../../builds/ol7-logs/G2-main_host_check.log) · [ASan/UBSan](../../builds/ol7-logs/G2b-main_host_check-asan.log) |
| 定向主机探针 | 复用上一轮的主机链接方法，与 `ol7-main_host_check` 对象重链。确认正向用例，并复现上述时序、I2C 与 trace 缺口。事件、GPIO 与 I2C 均由主机替身驱动。 [探针源码目录](../../builds/ol7-review-probes/) · [探针日志](../../builds/ol7-logs/review3-probes.log) |
| 静态内存和镜像 | `ol7-idf` 与同 `sdkconfig` 的 `78e5801` 基线比对。DRAM `97,379 → 97,643 B`（**+264 B**，预算 ≤1 KiB），IRAM `116,139 → 116,139 B`。Flash Code **+11,160 B**、Flash Data **+4,240 B**，镜像 `1,544,384 → 1,559,936 B`。`s_ui_ctrl` 120 B、重启任务句柄 4 B，均在 `.dram0.bss`。P2 的 BootArena 仍为 OLED 外供 3,072 B 栈和 1,024 B 帧缓冲。 [ELF 与 map 对比](../../builds/ol7-logs/elf-size-comparison.log) · [BootBuffers.cpp:174](../../main/app/BootBuffers.cpp#L174) |
| OLED 栈静态核对 | 当前 `refreshOnce()` 入口帧为 384 B。可见快照采集链计 **1,648 B**（含 `summarizeSatTable`），表盘渲染链到 `renderDialplate` 为 **992 B**。采集在渲染及发送前返回。实施报告的旧入口帧为 352 B，不能用作当前栈占用。实际 HWM 为 **NOT_RUN**。 [目标 ELF 栈帧](../../builds/ol7-logs/elf-stack-frames.log) |
| 无 USB、30 s 回退 | 源码无 `kCutPower` 提前关屏动作，P2 安全输出表无 OLED 供电 GPIO12。主机覆盖 GPIO4 释放、无线恢复、保存恢复及 UI 转回表盘，trace 问题见 R3OL2-06。 [BoardProfile.cpp:9](../../main/board/BoardProfile.cpp#L9) · [MainHostCheck.cpp:1031](../../tools/main_host_check/MainHostCheck.cpp#L1031) · [OledUi.cpp:382](../../main/modules/oled/OledUi.cpp#L382) |
| 实物验证 | **NOT_RUN**。运行时栈 HWM、实板 I2C 最大持锁、终帧停留时间、无 USB 亮屏到断电、实测 FPS 均未采样。 |

`git diff --check 78e5801` 通过。工作区新增本报告和忽略的验证产物。固件及仓库测试源码、提交历史和硬件均保持原状。
