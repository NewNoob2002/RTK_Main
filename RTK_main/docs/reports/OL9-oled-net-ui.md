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
- DataCenter 发布的 CSQ 每次变化都会唤醒 OLED，实际绘帧仍受帧节流约束。板上可测量抖动时的唤醒率。多类型告警仲裁由主机测试覆盖。

## HIL 步骤（由用户执行）

1. 在带 OLED、SIM 和蜂窝能力的板型上启用 `net.enable` 并重启。对照 `net status` 观察上电/搜网轮显、无 SIM 或 SIM 异常的 `x`、注册后信号格及 PDP 建立后的 `4G`。按 N1 的实验室步骤断开及恢复天线，核对首次有效信号和降档即时显示，以及后续升档取连续 10 s 窗口内的最小格数。
2. 单击选择 NET，双击进入。依次进入 4G、NTRIP、MQTT 详情，单击滚动至末行，双击返回摘要。核对 ICCID 与 `net status` 掩码一致，运营商无数据时及 N2/N3 占位行显示 `--`。关闭 `net.enable`，检查状态栏蜂窝区留空且 NET 仍可浏览。
3. 在具备 OLED 且无蜂窝能力的板型上单击轮询 Dock，核对顺序 SAT、POS、COMM、DEV、PWR 及首尾回绕。缺少该板型时以主机导航及表盘基准帧作为现有证据。
4. 在 DEV 档进入一个带选中项的子页，依次执行 `oled alarm`（默认 `enter`）、`oled alarm leave`、`oled alarm precision`、`oled alarm lowbat`。核对标题、左侧图标、右侧围栏名或详情及确认提示。围栏告警应以 250 ms 为完整周期，其余为 500 ms，标题和外框同步切换。任选一项等待 30 s，确认画面保持显示且停止闪烁。短按确认后应回到原子页和原选中项。
5. 完成 NET 列表滚动、长文本跑马灯和告警确认后执行 `sys tasks`，记录 `oled` 的 `stack_hwm` 并核对至少 528 B。定位页核对龄期与 `phone` 信息，设备页核对上次关机原因和可用的故障名称。

## HIL 顶栏修订（基线 `042c202`）

依据 HIL 反馈，`OledRender.cpp` 的共用顶栏采用两套固定列坐标，范围含首尾列。

| 内容 | 蜂窝图标可见 | 非蜂窝板或蜂窝图标隐藏 |
|---|---|---|
| 卫星 / 数字 / 告警 / 解状态 | 0–7 / 10–21 / 23–29 / 32–63 | 同左 |
| BT / Wi-Fi | 64–71 | 86–93 |
| 4G 信号格 | 74–75、77–78、80–81、83–84 | — |
| 4G 标识及会话箭头 | 86–92，箭头在第 0–3 行 | — |
| 充电 / USB 插头 | 95–99 | 同左 |
| 电池 | 102–126 | 同左 |

最长解状态短词 `PROBE`、`FLOAT` 均为 30 px，在 32 px 槽内居中，x63 保留为空列。三位卫星数使用 3×5 数字，以保留完整计数及滚动。四格信号高 3、4、6、8 px，底边对齐第 8 行，两列宽的未点亮格留空右列中部以区别满格。USB 未插时电源图标槽留空。蜂窝图标可见与隐藏状态之间切换时，仅 BT/Wi-Fi 使用相应的另一固定槽。蜂窝板的卫星图表专用顶栏保留 x30–71 给 used/visible 计数，避免最高三位计数进入信号格。

`OledUiCheck.cpp` 逐列检查两个布局的槽位与间隙，验证信号格高度、箭头范围、USB 拔插与蜂窝状态切换范围、三位数字滚动和卫星图表计数。模拟器脚本新增蜂窝与充电同时显示的 `comm-4g-charging.pbm`，原有 14 张金图仅改动顶部 10 行。

定点回归将当前 `OledUiCheck.cpp` 与 `042c202` 的渲染器组合构建，旧渲染器未通过新槽位断言，日志在 `builds/ol9w-logs/status-host-before.log`。修订后的 OLED 定点测试及四套模拟器金图测试均通过。

```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
tools/ci/run_gates.sh --prefix ol9w --base 54b01b6
```

完整门禁 **45 PASS / 0 FAIL**，包含冻结校验、普通与 ASan/UBSan 主机测试、无告警固件构建、clang-format、clang-tidy、函数文档及运行期分配门禁。普通与 ASan/UBSan 的 `main_host_check` 各通过 214 个用例。门禁总日志在 `builds/ol9w-gates.log`，逐项日志在 `builds/ol9w-logs/`。

用 `git archive` 提取 `042c202` 并构建 `builds/ol9w-baseline`。`idf.py -B builds/ol9w-idf size` 及两份 ELF 的节与栈帧对照记录在 `builds/ol9w-resources/`。

| 项目 | `042c202` | 顶栏修订后 | 增量 |
|---|---:|---:|---:|
| DRAM `.data` + `.bss` | 99,451 B | 99,451 B | 0 B |
| IRAM 代码及向量 | 116,139 B | 116,139 B | 0 B |
| 固件二进制 | 1,633,856 B | 1,633,936 B | +80 B |

`refreshOnce`、`drawFrame`、`renderUi`、`renderPageInto` 的 Xtensa 栈帧仍为 736、144、160、448 B。`drawStatusBar` 从 112 B 降到 80 B，新增数字绘制入口为 64 B，`drawSatHeader` 从 96 B 增到 112 B。

本轮变更范围以 `042c202` 为起点。此前合入 N1 时带入了 `components/cellular/include/cellular/LinkFsm.hpp` 和 `Policy.hpp` 的修改，本轮没有触及这些头或其它冻结文件。`docs/frozen.sha256` 原样保留。

修改后顶栏的板上 HIL 与 OLED 任务栈高水位为 **NOT_RUN**。板上可复测 USB 拔插、4G 搜网至 PDP、SIM 故障、关闭蜂窝及两套射频槽位，记录 `sys tasks` 中 `oled` 的 `stack_hwm` 是否至少为 528 B。本轮未访问串口或烧录。

## 全屏告警图标与闪烁（基线 `06f348b`）

- `OledIcons.hpp` 增加五枚各 72 B 的 24×24 单色 Flash 位图。围栏进入和离开使用相反箭头，精度丢失使用带叉号的准星，低电使用空电池，网络及未知类型使用感叹号。
- `OledRender.cpp` 绘制反色标题条、闪烁外框和左右分栏。右侧长围栏名沿用跑马灯，确认提示分为两行 `press to` 和 `confirm`。
- `OledAlarm.hpp` 集中定义 250/500 ms 完整周期和 30 s 上限，供后续 WP-ALARM 复用。`OledUi.hpp/.cpp` 在现有 30 fps 调度下切换亮暗相位。到期后退出闪烁刷新，保留告警至确认。已显示的告警被 OTA 或关机抢占时仍按原起点计时。
- `OledCmds.cpp` 接受四种 DEV 注入参数，默认 `enter`。模拟器复用测试快照构造，新加 `scripts/alarm.txt` 及六张金图。主机测试核对图标像素、亮暗相位、跨回绕计时、30 s 停闪与确认后选中项恢复。

普通和 ASan/UBSan 的 `main_host_check` 各通过 215 个用例。任务卡门禁 `tools/ci/run_gates.sh --prefix ol9w --base 54b01b6` **45 PASS / 0 FAIL**，日志在 `builds/ol9w-gates.log` 与 `builds/ol9w-logs/`。

用 `idf.py -B builds/ol9w-idf size` 及两版 ELF 对照，DRAM 仍为 99,451 B，IRAM 仍为 116,139 B，固件从 1,633,936 B 增至 1,635,136 B。位图符号各占 72 B 并位于 Flash，资源日志在 `builds/ol9-alarm-resources/`。

`stepAlarm` 的 Xtensa 栈帧仍为 48 B，`renderState` 从 32 B 增至 48 B，`renderPageInto` 从 448 B 降至 400 B。主机反汇编估算告警文字绘制路径从 `taskEntry` 到 WouoUI 写入帧缓冲合计 1,872 B 静态栈帧，3,072 B 配额下该路径约余 1,200 B。设备任务整体高水位及硬件验证 **NOT_RUN**，实板验收仍需确认 `oled` 的 `stack_hwm` ≥ 528 B。本轮未改冻结文件、未访问串口或烧录。

## HIL 结果（2026-11-27，P2 v1.41 F9P 测试板 + EC600U + 电信 SIM，构建 5772ab6 + 合入 N1 99e2878）
- 状态栏：首轮反馈 BT 与 4G 间空隙大、4G 标识与电池重叠、充电图标未贴电池 → 用户选固定槽位方案，06f348b 重排；复测 PASS（无重叠、USB 拔插不位移）。
- 步骤 4 告警：首轮纯文字 PASS（确认后回原页与选中项）；用户要求加图标与闪烁 → 5772ab6；复测 enter/leave/precision/lowbat 四类图标、闪烁、30 s 停闪、确认返回全部 PASS。
- 步骤 5：告警四类与 NET 浏览后 `sys tasks` oled stack_hwm = 656 B（≥528 B）PASS。
- 步骤 1 信号格随状态栏复测 PASS；步骤 2 NET 详情浏览、定位页龄期/来源、设备页上次关机原因：用户确认 PASS；天线拔插降档在测试板天线座故障下未单独验证。
- 步骤 3（无蜂窝板型）：无此板型，以主机导航与金图为证据。
- 备注：围栏告警 250 ms 为完整周期（125 ms 翻转），用户接受。
