# OL9 OLED 网络信息与预留界面

分支 `feat/ol9-net-ui`，对照基线 `54b01b6`。工程 profile 为 ESP-IDF 6.1、ESP32。

## 实现与改动文件

| 文件 | 改动 |
|---|---|
| `main/modules/oled/OledNet.hpp`、`OledUi.hpp/.cpp`、`OledRender.cpp`、`OledIcons.hpp`、`OledMarquee.hpp`、`main/def/OledDef.hpp` | x84–102 蜂窝图标、10 s 升格迟滞与立即降格、NET 导航和滚动列表、告警画面、定位来源及设备关机记录。无蜂窝板仍使用现有五项 Dock。蜂窝板即使关闭 `net.enable` 仍能查看 NET。 |
| `main/def/NetStatusText.hpp`、`main/modules/net/NetCmds.cpp`、`NetModule.cpp`、`main/modules/storage/StorageModule.cpp`、`main/modules/oled/OledModule.cpp` | DataCenter 的蜂窝显示字段或开关变化后唤醒 OLED。采集阶段掩码 ICCID，和 `net status` 共用同一个纯函数。 |
| `main/def/OledAlarm.hpp`、`main/data/AppData.hpp/.cpp`、`main/modules/oled/OledCmds.cpp` | DataCenter 告警批次及 DEV 测试源 `oled alarm`。正常运行时，告警依次按围栏进入、围栏离开、精度丢失、低电、网络排序。一次短按确认当前项，关机和 OTA 抢占告警。 |
| `main/def/SystemDef.hpp`、`main/modules/power/PowerModule.hpp/.cpp` | 复用 `PowerSnapshot::lastShutdown` 并发布最近故障参与者名。在同一阶段有多项故障时显示登记顺序中的第一项，射频超时可见 `radio_pause`。记录沿用本次启动期间的生命周期。 |
| `main/def/OtaDef.hpp`、`main/modules/web/WebOta.cpp` | OLED 从 DataCenter 的通用 `ota::Progress` 读取，Web 是当前唯一发布者，保留蜂窝来源枚举。 |
| `tools/main_host_check/CMakeLists.txt`、`MainHostCheck.cpp`、`OledModuleCheck.cpp`、`OledUiCheck.cpp`、`WebModuleCheck.cpp` | 覆盖 CSQ 边界、候选被打断、计时回绕、图标选择及像素边界、两种 Dock 的导航、三级详情、ICCID 掩码、告警确认与优先级、关机参与者名和 OTA 来源。真实 OLED 模块测试经 DataCenter 验证蜂窝与告警采集。 |
| `tools/oled_sim/CMakeLists.txt`、`OledSim.cpp`、`golden/{device,device-scrolled,pos,pos-scrolled}.pbm` | 适配共用类型并更新受新增列表行影响的四张基准帧；表盘、其它页面和现有 OTA 基准帧通过原始像素比对。 |

4G 详情的运营商暂无 N1 快照数据，显示 `--`。NTRIP 和 MQTT 字段等待 N2/N3 接入。定位页从有效 GNSS 解及其差分龄期显示当前唯一的 `phone` 来源，数据缺失时显示 `--`。

## 评审修订

- 开机首次收到有效 CSQ，或从未知状态恢复时，立即显示对应格数。后续目标连续高于当前格数满 10 s，升到该窗口内的最小格数；降格即时生效。
- 告警覆盖期间的配对请求与 OTA 一样交由蓝牙端按原截止时间处理。告警确认后可显示随后到达的新配对请求。
- `oled_ui_check` 回归覆盖首次有效值、99 恢复、计时回绕、3/4 格抖动、升格窗口中断，以及告警覆盖前和确认帧收到的配对请求。修订前 FAIL，修订后 PASS，日志为 `builds/ol9-review-regression-before.log` 和 `builds/ol9-review-regression-after.log`。

## 验证

```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
tools/ci/run_gates.sh --prefix ol9 --base 54b01b6
```

| 门禁 | 结果 |
|---|---|
| G1 冻结 SHA-256 | PASS |
| G2 普通主机测试 | PASS，15 套 |
| G2b ASan/UBSan | PASS，15 套 |
| G3 ESP-IDF 默认、trace-off、wifi-off、ota-off、production、console-none、none-wifi | PASS，7 套均无告警 |
| G3b 纯组件测试应用 | PASS，ESP32 与 ESP32-C3 |
| G5 clang-format、G5b clang-tidy | PASS |
| G6 函数文档 | PASS |
| G7b 运行期分配门禁、G7c 无 CLI 依赖 | PASS |
| OLED 硬件验证及任务栈高水位 | NOT_RUN，板子正用于其它 HIL |

评审修订后完整门禁共 **45 PASS / 0 FAIL**。普通和 sanitizer 的 `main_host_check` 各通过 214 个用例。总日志在 `builds/ol9-gates.log`，逐项日志在 `builds/ol9-logs/`。初次实现时的首轮沙箱执行受 ptrace 限制，LeakSanitizer 退出，日志保存在 `builds/ol9-sandbox-logs/`。评审前后的完整门禁均经审批在沙箱外执行，保留所有 sanitizer 检查。函数文档门禁使用本机缓存的 lizard 1.26.1。

评审修订后另独立构建 `tools/main_host_check`，运行 `ctest --test-dir builds/ol9-main_host_check --output-on-failure`，214/214 PASS，日志在 `builds/ol9-review-host.log`。

## 资源与冻结输入

用 `git archive` 提取 `54b01b6` 的工程并在 `builds/ol9-baseline` 以同一 ESP-IDF 工具链构建。两份 ELF 通过 `xtensa-esp-elf-size -A` 和 `xtensa-esp-elf-objdump -dC` 对照，另运行 `idf.py -B builds/ol9-idf size`。日志在 `builds/ol9-resources/`。

| 项目 | 基线 | OL9 | 增量 |
|---|---:|---:|---:|
| DRAM `.data` | 53,883 B | 54,011 B | +128 B |
| DRAM `.bss` | 45,384 B | 45,440 B | +56 B |
| DRAM 合计 | 99,267 B | 99,451 B | **+184 B** |
| IRAM 代码及向量 | 116,139 B | 116,139 B | **0 B** |
| 固件二进制 | 1,627,552 B | 1,633,424 B | +5,872 B |

评审修订使固件二进制增加 80 B。静态 DRAM、IRAM 与下列栈帧在修订前后相同。`step`、`stepActive`、`stepAlarm` 的栈帧也分别保持 128、96、48 B。

分配门禁通过，OLED 任务仍配置 3,072 B 栈。Xtensa `entry` 栈帧对照中，`refreshOnce` 为 656 → 736 B，`collectViewModel` 为 816 → 592 B，`drawFrame` 为 208 → 144 B，`renderUi` 均为 160 B，`renderPageInto` 为 432 → 448 B。常规渲染链 refreshOnce → drawFrame → renderUi → renderPageInto 的这四个栈帧合计增加 32 B。目标板 `oled` 栈高水位为 **UNKNOWN**，验收阈值为 **至少 528 B**。

冻结文件和 `docs/frozen.sha256` 均无改动。实施期间未访问串口、未烧录，提交仅在本地工作树完成。

## 评审后续项

- 全屏告警期间隐藏长按进度环，并暂停底层页面的弹簧和转场动画。确认后恢复页面交互并重置子页空闲计时。
- 面板熄屏后保持熄屏，充电待机保留原画面。接入真实告警源时再确定唤屏和待机显示策略。
- DataCenter 发布的 CSQ 每次变化都会唤醒 OLED，实际绘帧仍受帧节流约束。板上可测量抖动时的唤醒率。DEV 命令 `oled alarm` 注入围栏进入，其余告警类型及多类型仲裁由主机测试覆盖。

## HIL 步骤（由用户执行）

1. 在带 OLED、SIM 和蜂窝能力的板型上启用 `net.enable` 并重启。对照 `net status` 观察上电/搜网轮显、无 SIM 或 SIM 异常的 `x`、注册后信号格及 PDP 建立后的 `4G`。按 N1 的实验室步骤断开及恢复天线，核对首次有效信号和降档即时显示，以及后续升档取连续 10 s 窗口内的最小格数。
2. 单击选择 NET，双击进入。依次进入 4G、NTRIP、MQTT 详情，单击滚动至末行，双击返回摘要。核对 ICCID 与 `net status` 掩码一致，运营商无数据时及 N2/N3 占位行显示 `--`。关闭 `net.enable`，检查状态栏蜂窝区留空且 NET 仍可浏览。
3. 在具备 OLED 且无蜂窝能力的板型上单击轮询 Dock，核对顺序 SAT、POS、COMM、DEV、PWR 及首尾回绕。缺少该板型时以主机导航及表盘基准帧作为现有证据。
4. 在 DEV 档正常运行时执行 `oled alarm`，检查全屏 `FENCE ENTER` 与 `TEST FENCE`。短按确认后应回到此前页面，且选中项未被这次按键改变。再次执行可显示下一批测试告警。
5. 完成 NET 列表滚动、长文本跑马灯和告警确认后执行 `sys tasks`，记录 `oled` 的 `stack_hwm` 并核对至少 528 B。定位页核对龄期与 `phone` 信息，设备页核对上次关机原因和可用的故障名称。
