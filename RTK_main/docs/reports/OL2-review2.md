# OL2 独立复审 · FAIL

复审范围为基线 `78e5801` 至当前工作区的差异，包含未跟踪的 OLED 源文件。按 ESP-IDF / C++ T2 项目规则，核对任务卡、实施报告、首轮审查及其修复表。全量门禁复跑 **43/43 PASS**。功能验收 **FAIL**，阻塞项为 USB 重启截止、关机完成状态、按键时间语义和共享 I2C 的 8 ms 持锁约束。

## 复审发现

### R2OL2-01（Major）USB 终帧额外等待可超过 30 ms

依据为任务卡 §B。日志排空条件包住了每次终帧截止检查。以 U3 关断为零点，主机复现中 `+30 ms` 首次排空并启动 OLED 计时，随后日志环重新有数据，`+60 ms` 到期时仍未重启，直到 `+150 ms` 日志兜底才重启。日志始终空闲时也有量化误差。用符合 P2 空闲等待与每轮让出一 tick 的 11 ms 轮询序列，`+33 ms` 开始计时，`+66 ms` 才重启，实际额外等待 33 ms。OLED 完成标志也只由下一轮 Power 轮询观察。证据见 [PowerModule.cpp:740](../../main/modules/power/PowerModule.cpp#L740)、[BoardProfile.hpp:146](../../main/board/BoardProfile.hpp#L146)、[Executors.cpp:104](../../main/app/Executors.cpp#L104)、[module_list.inc:29](../../main/module_list.inc#L29)、[Executor.cpp:563](../../components/rtos/Executor.cpp#L563)、[Executor.cpp:611](../../components/rtos/Executor.cpp#L611) 和 [PowerProbe.log](../../builds/ol5-logs/PowerProbe.log)。

建议首次排空或达到原日志上限时锁存放行，之后独立检查 OLED 完成和绝对截止。让完成事件和截止都能唤醒负责重启的路径，避免只靠 10 ms 轮询兑现硬上限。补上日志重新变忙和非整除轮询的回归。

### R2OL2-02（Major）保存、射频超时后仍发布“全部完成”并进入 USB 重启

依据为任务卡 §B 的真实收尾进度与重启前置条件。基线允许保存和射频等待超时后推进。OL2 在进入 `kQuiesce` 时无条件标记保存 fence、落盘和外设安全，在进入 `kCutPower` 时无条件标记射频已停。保存超时和 GPIO 安全电平失败都有各自的失败路径，无法从阶段反推成功。主机让白名单 NVS 写入失败、蓝牙暂停保持未完成，`kCutPower` 发布 `0x3f`，U3 变低后发布 `0x7f` 并走 USB 重启，重启时仍有待写数据且 `powerPauseDone()==false`。已有 `power_fence_timeout` 也明确期望超时后重启。证据见 [PowerLogic.cpp:290](../../main/board/PowerLogic.cpp#L290)、[PowerModule.cpp:518](../../main/modules/power/PowerModule.cpp#L518)、[PowerModule.cpp:527](../../main/modules/power/PowerModule.cpp#L527)、[PowerModule.cpp:668](../../main/modules/power/PowerModule.cpp#L668)、[OledRender.cpp:867](../../main/modules/oled/OledRender.cpp#L867)、[MainHostCheck.cpp:644](../../tools/main_host_check/MainHostCheck.cpp#L644) 和 [PowerProbe.log](../../builds/ol5-logs/PowerProbe.log)。

建议在实际完成点记录完成位，并把保存、射频超时及安全输出失败暴露为降级结果。若要保留基线强制下电策略，需明确该例外是否也获准触发 USB 重启，并为降级结果提供独立文案。按“全部收尾完成后才重启”验收时，应在 U3 断开前拦住这条路径并执行约定的失败回退。

### R2OL2-03（Major）双击窗口随 OLED 调度漂移，空闲单击也可超出 300 ms

依据为任务卡按键方案 A。Power 只投递计数，UI 用消费时刻衡量两次短按。两次消抖短按相隔 290 ms，第二次若因 OLED 正在写页晚 15 ms 才消费，判断间隔就成为 305 ms 并拆成两个单击。另一条路径是在窗口已经过去 250 ms 时，UI 决定再等 50 ms，但空闲帧分支在本帧耗时 20 ms 后仍睡 50 ms，确认最早为第 320 ms。独立主机探针覆盖这两种时间输入。一次 2.5 s 长按仍只产生 `kLongHold`，松开无短按脉冲，关机中与充电待机都会清除待判单击。证据见 [PowerModule.cpp:763](../../main/modules/power/PowerModule.cpp#L763)、[OledDef.hpp:184](../../main/def/OledDef.hpp#L184)、[OledAnim.cpp:26](../../main/modules/oled/OledAnim.cpp#L26)、[OledUi.cpp:424](../../main/modules/oled/OledUi.cpp#L424)、[OledUi.cpp:740](../../main/modules/oled/OledUi.cpp#L740)、[OledModule.cpp:295](../../main/modules/oled/OledModule.cpp#L295)、[PowerLogic.cpp:203](../../main/board/PowerLogic.cpp#L203) 和 [UiProbe.log](../../builds/ol5-logs/UiProbe.log)。

建议以消抖短按事件的生产时间消歧，保留足够的事件顺序信息。休眠前按绝对窗口截止重新计算剩余时间，覆盖静止帧、动画帧和关机抢占。补测 OLED 在 299/300 ms 边界处正忙时的最终导航结果。

### R2OL2-04（Major）共享 I2C 的单页持锁缺少 8 ms 总时限

依据为任务卡 §D。400 kHz 和按页加解锁保持原样。页地址命令与 129 B 数据在同一次 `BusLock` 内分别使用 8 ms 超时，健康总线的线速估算约 3.04 ms／页（135 字节含地址，未计软件开销）。即使命令在约 1 ms 内成功，数据再等满 8 ms 就已超过 8 ms 持锁约束。IDF 同步发送还把同一超时值用于多次命令等待和结果等待。静态断言及主机 shim 只检查 `2 × 8 < 20` 和锁是否持有。该实现继承自基线，OL2 ADR §5.4 将上限写成 8 ms，与同一 ADR §2 的预算有出入。Charger 的 0 tick 获取仍立即返回 `kBusy`，锁争用会使该轮采样延后。证据见 [Sh1106.cpp:56](../../main/board/Sh1106.cpp#L56)、[Sh1106.cpp:106](../../main/board/Sh1106.cpp#L106)、[Charger.cpp:149](../../main/board/Charger.cpp#L149)、[OledDriverCheck.cpp:395](../../tools/main_host_check/OledDriverCheck.cpp#L395)、[ADR-0020:36](../adr/ADR-0020-oled-display.md#L36)、[ADR-0020:143](../adr/ADR-0020-oled-display.md#L143) 和 [本机 ESP-IDF i2c_master.c](/home/gtc/esp/esp-idf-v6.1/components/esp_driver_i2c/i2c_master.c:529)。

建议以单页获取锁到释放锁为单位实现统一预算，并验证底层调用能兑现这个预算。若任务实际允许的是逐事务 8 ms，应由任务卡和 ADR 给出一致的持锁约束。主机注入命令迟到、数据超时和锁争用，实板量最大持锁时长。

### R2OL2-05（Major）当前主机门禁仍放过终帧接线回归与部分渲染回归

依据为任务卡测试要求及 `ROL2-02`、`ROL2-03`。以下缺口均可保留绿色门禁。

- 电源用例手工置终帧位，驱动用例单独调用 `clearBackoff()`，UI 用例给后处理传入人工枚举。三者都未编译实际连接这些步骤的 `OledModule.cpp`。删掉模块内的 `clearBackoff()` 或将成功通知移到刷屏之前，这些用例仍会通过。组合探针确认，连续写失败后跳过清退避会得到 `kSkipped`，零页写出但后处理允许标记终帧；调用现有清退避后补写 8 页。证据见 [CMakeLists.txt:62](../../tools/main_host_check/CMakeLists.txt#L62)、[CMakeLists.txt:376](../../tools/main_host_check/CMakeLists.txt#L376)、[MainHostCheck.cpp:482](../../tools/main_host_check/MainHostCheck.cpp#L482)、[OledModule.cpp:316](../../main/modules/oled/OledModule.cpp#L316)、[OledDriverCheck.cpp:233](../../tools/main_host_check/OledDriverCheck.cpp#L233) 和 [DriverProbe.log](../../builds/ol5-logs/DriverProbe.log)。
- USB fixture 在进入 `kCutPower` 并推进一段时间后清零 `s_restarts`。一次发生在 U3 变低之前的错误重启可能被清掉。应全程保留累计值并在 U3 翻转前断言零次。证据见 [MainHostCheck.cpp:433](../../tools/main_host_check/MainHostCheck.cpp#L433)、[MainHostCheck.cpp:469](../../tools/main_host_check/MainHostCheck.cpp#L469) 和 [MainHostCheck.cpp:1596](../../tools/main_host_check/MainHostCheck.cpp#L1596)。
- 数字滚动用例同时改变卫星数和电量，只断言整帧存在差异。探针分别关闭卫星或电量滚动，原断言两次都通过，一路动画被删仍无告警。自检的等待秒数与固件文本已有实际对比，两个数字的路径需独立断言。证据见 [OledUiCheck.cpp:592](../../tools/main_host_check/OledUiCheck.cpp#L592)、[OledUiCheck.cpp:612](../../tools/main_host_check/OledUiCheck.cpp#L612) 和 [UiProbe.log](../../builds/ol5-logs/UiProbe.log)。

建议用真实终帧编排接入 I2C shim，覆盖连续错误、退避、成功补写、零脏页和重启顺序。让“内容已一致”和“退避跳过”返回不同结果可进一步降低误置位风险。渲染测试分别检查每个滚动区域，并验证方向及终值。

### R2OL2-06（Minor）GNSS 徽章已区分枚举，右侧仍出现笼统的等待文案

依据为任务卡 §C 与 `ROL2-05`。启动、配置、维护状态已显示 `BOOT` 或 `CFG`，`kReadback` 的定位数据与运行态相同。目前表盘右侧在 `kOutput`、`kMaintenance` 仍显示 `GNSS WAIT`，`kOff` 和 `kFailed` 也会落到该文案。比如运行中修改报文输出时，左侧显示配置，右侧仍切成等待，卫星数变成 `--`。建议按状态显示对应操作，只有定位快照有效且产品允许沿用时保留上一组定位数据。证据见 [OledUi.cpp:674](../../main/modules/oled/OledUi.cpp#L674)、[OledRender.cpp:316](../../main/modules/oled/OledRender.cpp#L316)、[OledRender.cpp:504](../../main/modules/oled/OledRender.cpp#L504)、[Controller.cpp:126](../../components/gnss/src/Controller.cpp#L126)。

## 首轮七项复核

| 编号 | 结论 | 证据 |
| --- | --- | --- |
| ROL2-01 | 部分修复 | 正常日志路径确实等待终帧。已提前刷完及 OLED 未登记在线时均无额外等待。日志回忙和轮询间隔仍能突破 30 ms，见 R2OL2-01。 [PowerModule.cpp:740](../../main/modules/power/PowerModule.cpp#L740) |
| ROL2-02 | 部分修复 | 四种握手边界、单击剩余 50 ms、活跃自检和数字滚动均有实际覆盖。模块接线与单路渲染仍可假阳性，见 R2OL2-05。 [MainHostCheck.cpp:482](../../tools/main_host_check/MainHostCheck.cpp#L482) · [OledUiCheck.cpp:328](../../tools/main_host_check/OledUiCheck.cpp#L328) |
| ROL2-03 | 生产逻辑修复 | 终帧先清退避，再刷屏。`kError` 不确认，休眠 2 ms 后可再试。`kSkipped` 在这一调用顺序下表示无脏页。持续写失败时 Power 路径仍有重启兜底，硬截止缺口见 R2OL2-01，测试缺口见 R2OL2-05。 [OledModule.cpp:316](../../main/modules/oled/OledModule.cpp#L316) · [OledUi.cpp:727](../../main/modules/oled/OledUi.cpp#L727) · [Sh1106.cpp:270](../../main/board/Sh1106.cpp#L270) |
| ROL2-04 | 修复 | Q8 小步停滞时吸附终值。1/2/16/33 ms 与正负位移的单调性及终值断言在两种主机构建均通过。 [OledAnim.cpp:121](../../main/modules/oled/OledAnim.cpp#L121) · [OledUiCheck.cpp:181](../../tools/main_host_check/OledUiCheck.cpp#L181) |
| ROL2-05 | 部分修复 | 补齐枚举徽章和 `kReadback` 数据路径，右侧等待文案仍有缺口，见 R2OL2-06。 [OledUi.cpp:674](../../main/modules/oled/OledUi.cpp#L674) · [OledUiCheck.cpp:632](../../tools/main_host_check/OledUiCheck.cpp#L632) |
| ROL2-06 | 修复 | 跳过自检后可在正常 UI 循环补挂 GNSS 失败告警，状态机及渲染状态均有主机断言。 [OledUi.cpp:399](../../main/modules/oled/OledUi.cpp#L399) · [OledUiCheck.cpp:400](../../tools/main_host_check/OledUiCheck.cpp#L400) |
| ROL2-07 | 修复 | OLED 控制器不可达唤醒路径已移除，独立构建的 `s_ui_ctrl` 在 `.dram0.bss`，大小 124 B。SH1106 低层电源方法仍由驱动测试覆盖。 [OledModule.cpp:35](../../main/modules/oled/OledModule.cpp#L35) · [ADR-0020:112](../adr/ADR-0020-oled-display.md#L112) |

## 专项验证

- **无 OLED、无 USB 与 30 s 回退。** 关闭 OLED 在线位的 USB 场景在原日志放行点直接重启。目标代码仅在探测成功且刷新任务已运行时置在线位，驱动主机测试覆盖面板缺失。无 USB 路径无软件关屏调用，P2 安全输出表也不包含 OLED 供电 GPIO12。U3 持续无法断开时，30 s 截止回到 `kIdle`，释放 GPIO4、恢复射频与保存、清除终帧标志并通知 UI 恢复表盘。真实 Power 超时和 UI 恢复有各自的主机测试。物理亮屏保持、面板恢复及持续 I2C 故障下的引脚状态为 **NOT_RUN**。 [OledModule.cpp:216](../../main/modules/oled/OledModule.cpp#L216) · [OledModule.cpp:277](../../main/modules/oled/OledModule.cpp#L277) · [BoardProfile.cpp:10](../../main/board/BoardProfile.cpp#L10) · [PowerLogic.cpp:328](../../main/board/PowerLogic.cpp#L328) · [PowerModule.cpp:587](../../main/modules/power/PowerModule.cpp#L587) · [MainHostCheck.cpp:898](../../tools/main_host_check/MainHostCheck.cpp#L898) · [OledUiCheck.cpp:394](../../tools/main_host_check/OledUiCheck.cpp#L394)
- **静止零写。** 确认已提交且内容相同的帧在计算脏页后直接返回，不进入页锁。主机测试同时覆盖静帧零 I2C 事务、单页变化和失败重试仅补未提交页。实际数据或页面内容更新时允许写脏页。动画调度按 33 ms 扣除本帧耗时，实际 FPS 与 I2C 尾延迟为 **NOT_RUN**。 [Sh1106.cpp:270](../../main/board/Sh1106.cpp#L270) · [OledDriverCheck.cpp:118](../../tools/main_host_check/OledDriverCheck.cpp#L118) · [OledDriverCheck.cpp:182](../../tools/main_host_check/OledDriverCheck.cpp#L182) · [OledUi.cpp:740](../../main/modules/oled/OledUi.cpp#L740)
- **DRAM、IRAM 与 Flash。** 从归档的 `78e5801` 源码用同一份 `sdkconfig` 独立构建。DRAM 97,379 → 97,595 B（+216 B，满足 ≤1 KiB），IRAM 116,139 → 116,139 B，镜像 1,544,384 → 1,558,720 B（+14,336 B），Flash Code/Data 分别 +10,072 B / +4,160 B。本次基线构建口径不同于实施报告引用的旧构建，当前成品数值一致。 [elf-size-comparison.log](../../builds/ol5-logs/elf-size-comparison.log) · [实施报告 §3](OL2-oled-ui.md#L66)
- **栈。** 目标 ELF 反汇编确认采集先返回才进入渲染或 I2C。采集链到卫星汇总的可见栈帧合计 1,616 B。表盘滚动到格式化的代表性链约 1,296 B，页写到 IDF 同步发送的代表性链约 1,200 B。上述合计是可见调用帧，完整峰值还取决于下层调用与 Xtensa 寄存器窗口。任务配置保持 3,072 B，`sys tasks` 实板 HWM 为 **NOT_RUN**。 [OledModule.hpp:27](../../main/modules/oled/OledModule.hpp#L27) · [elf-stack-frames.log](../../builds/ol5-logs/elf-stack-frames.log)

## 执行结果

| 验证 | 结果 |
| --- | --- |
| `ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol5 --base 78e5801` | **43 PASS / 0 FAIL**。包括冻结校验、normal 与 ASan/UBSan、ESP-IDF 配置变体、ESP32/ESP32-C3 纯逻辑构建、格式、文档、tidy、运行期分配与 CLI 隔离。两份 `main_host_check` 各 140/140。初跑 42/43，G6 受只读 uv 缓存锁影响，复跑通过 PATH 使用已有的 lizard 1.26.1 缓存，未联网安装。 [门禁复跑日志](../../builds/ol5-logs/review2-gates-rerun.log) · [G6 初跑证据](../../builds/ol5-logs/G6-doc_check-initial.log) |
| 独立关机、按键、渲染及驱动探针 | **复现缺陷和测试缺口**。用门禁主机目标编译参数和已有对象链接，探针在忽略的 `builds/ol5-review-probes/`。 [PowerProbe](../../builds/ol5-logs/PowerProbe.log) · [UiProbe](../../builds/ol5-logs/UiProbe.log) · [DriverProbe](../../builds/ol5-logs/DriverProbe.log) |
| 独立基线构建、ELF 和 map 静态核查、`git diff --check 78e5801` | **PASS**。 |
| HIL、实际 I2C 持锁、关机显示停留、硬件时序、FPS 与运行栈 HWM | **NOT_RUN**。按授权范围执行本地构建与主机核查。 |

本次仅新增复审报告与忽略的验证产物。无代码变更、提交、烧录或串口操作。
