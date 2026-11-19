# ADR-0016：控制台构建档位（开发 / 量产 / 无控制台）

状态：草案（2026-11-19，待用户批准）。由 D 系列任务卡实施（D1，见 §5）。
关联：ADR-0007（cli 组件）、ADR-0009 §2.6–§2.12（控制台输出、trace、factory 命令、日志通道）、ADR-0011（启动步骤）、
ADR-0015（P2 板级识别）。

> 命名：D = Debug console。

## 1 背景

- P2 v1.41 的 Type-C 经 USB2514 集线器只接到 GNSS 的 CH340（COM2），**ESP32 的 UART0 不对用户引出**；控制台只能通过
  板上调试/烧录焊盘外接串口板（台架上是 `/dev/ttyACM0` 的 WCH 适配器）访问。对量产用户，控制台是开发与工厂工具，不是功能。
- 现固件只有一种控制台：全部命令组（`status sys log config rtcm gnss bt alarm debug factory freset`）。其中 `gnss`、`rtcm`、`bt`、
  `debug` 能改运行状态（发 GNSS 命令、改输出频率、开配对、改 trace 掩码），交付后误用的代价大于便利。
- 资源（2026-11-18 由符号表估算，D1 实测替换）：控制台相关 flash 约 48 KB（IDF console 约 15.4 KB、libcli 约 4.2 KB、
  `ConsoleSetup` 约 1.8 KB、12 个 `*Cmds.cpp` 约 27 KB），镜像 869 KB；RAM 约 2.5 KB 静态 + 约 14–16 KB 堆
  （UART TX 环 8192 B、RX 256 B、REPL 栈 4096 B、linenoise 历史 32 条）。P2 启动后空闲堆约 82.6 KB。
- 工厂写入 SN、蓝牙白名单、配对窗口目前**只能**经控制台 `factory` 命令完成，手机协议没有对应指令。

## 2 决定

### 2.1 Kconfig 档位 `RTK_CONSOLE_PROFILE`

`main/Kconfig.projbuild` 新增 choice（编译期，无运行期切换）：

| 选项 | 用途 | 默认 |
|---|---|---|
| `RTK_CONSOLE_PROFILE_DEV` | 开发、HIL：现状不变 | **是** |
| `RTK_CONSOLE_PROFILE_PRODUCTION` | 量产固件：只读诊断 + 隐藏 factory | 否 |
| `RTK_CONSOLE_PROFILE_NONE` | 无 REPL：只保留串口日志 | 否 |

`sdkconfig.defaults` 不改（冻结）：默认档位来自 Kconfig `default`。量产档位经叠加文件选择（§2.5）。
代码中用 `main` 私有宏 `RTK_CONSOLE_PROFILE`（取值 `kDev/kProduction/kNone` 的 `constexpr` 枚举）判断，不在业务代码散布 `#ifdef CONFIG_…`；
只有命令表登记与 CMake 处可用预处理分支。

### 2.2 DEV：保持现状

命令集合、门控（`kStarting/kStopping` 拒绝、`kConsoleOnly` 只放行 `status/log/sys/config`）、输出通道、REPL 任务参数全部不变。

### 2.3 PRODUCTION：只读诊断 + 隐藏 factory

- REPL 与输出通道同 DEV（UART0、2 Mbaud、TX 环、REPL 任务 prio 2 core 0）。
- **登记**：`status`（全部动词，只读）、`sys`（`info/heap/mem/boot/tasks/reboot`）、`log`、`config`（只读）、
  `factory`（隐藏，需 `factory unlock <key>`，ADR-0009 §2.10）、`freset`。
- **不登记**：`gnss`、`rtcm`、`bt`、`alarm`、`debug` 命令表及其 help 动词组。只改登记清单（`main/app/ConsoleSetup.cpp`），
  命令实现文件照常编译，未被引用的表与处理函数由 `--gc-sections` 去掉——不为此拆文件或改 `components/cli/include/**`。
  `factory pair/wl-add/wl-del` 依赖的 `openDefaultPairing()/editTrustedAlarm()` 等函数保留。
- `help` 只列登记的组；`kConsoleOnly` 门控规则不变。
- 量产工厂测试所需的 GNSS/蓝牙状态由 `status gnss`、`status link` 提供；需要改 GNSS 的产线动作不在本档位范围内（另开卡）。

### 2.4 NONE：无 REPL，只留日志

- 启动不调用 `cli::init()/cli::run()`：不建 REPL 任务、不装 linenoise、不登记命令表；`esp_console` 与命令实现因无引用被链接器去掉。
  不修改 `components/cli` 的冻结头。
- 日志：`identify` 步骤照常 `installDirectLog()`。没有 TX 环驱动时 `cli::outWrite` 回落 stdout（IDF UART0 VFS，逐字节阻塞，
  约 24.6 KB/s，ADR-0009 §2.8）。默认 I 级日志稀疏（启动约 20 行，运行期只有状态变化与错误），可接受；
  建议与 `RTK_TRACE_ENABLE=n` 同用，避免 trace_out 在慢通道上积压。
- 启动步骤表不变：`console` 步骤保留（`BootStepsCheck` 与 `sys boot` 不受影响），本档位下步骤体只记一行 I
  `console disabled (profile none)` 并返回 kOk。
- 降级：`AppContext::boot()` 遇到失败步骤时打 E（同现在），不再尝试 `initConsole()/runConsole()`，启动任务直接返回；
  `bootPhase` 仍置 `kConsoleOnly`（含义即“已降级、业务未运行”），不重启，避免电池供电下的重启循环。
- **约束**：NONE 档位下没有任何写 SN/白名单/配对的途径。在出现替代的工厂写入通道（例如手机协议指令或产线专用固件）之前，
  P2 量产不得使用 NONE（见 §4 Q1）。

### 2.5 构建与门禁

- 叠加文件（新增，非冻结）：`configs/production.defaults`：
  ```
  CONFIG_RTK_CONSOLE_PROFILE_PRODUCTION=y
  CONFIG_RTK_TRACE_ENABLE=n
  ```
  以及 `configs/console-none.defaults`（`CONFIG_RTK_CONSOLE_PROFILE_NONE=y`、`CONFIG_RTK_TRACE_ENABLE=n`）。
  用法：`idf.py -B builds/<name> -DSDKCONFIG=builds/<name>/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/production.defaults" build`。
- `tools/ci/run_gates.sh` 增加 `G3-idf-production` 与 `G3-idf-console-none` 两个 IDF 构建门（与 `G3-idf-trace-off` 同写法），
  均 0 告警。G7b 仍只查默认构建。
- 启动横幅（`stepBanner`）加档位名：`console profile dev|production|none`；`config build` 显示同一字段。

### 2.6 不变

- UART0 波特率、`CONFIG_ESP_CONSOLE_*`、ROM/bootloader 日志、烧录方式。
- factory 密钥机制（只防误操作，不是安全边界，ADR-0009 §2.10）；量产密钥值由发布构建在本地叠加文件里设置，不进仓库。
- 任务优先级/核心/栈、分区表、NVS 键、蓝牙名。

## 3 实测（D1 填写）

| 构建 | DRAM 静态 | Flash code | Flash data | 镜像 | 启动后空闲堆 |
|---|---|---|---|---|---|
| dev（默认） | | | | | |
| production | | | | | |
| console-none | | | | | |

## 4 待定问题

- **Q1（用户）**：P2 量产是否需要写 SN、白名单、配对？白名单与 `alarm` 来自 E1 报警器产品线，P2 是否沿用？
  若不需要，P2 可直接用 NONE；若需要，P2 用 PRODUCTION，或先提供替代写入通道再用 NONE。
- Q2：PRODUCTION 是否保留 `log level`（改 IDF 日志级别，不影响业务）。本草案保留。

## 5 分步

- **D1**：档位 Kconfig、三档登记/启动/降级行为、叠加文件、两个新门禁、横幅与 `config build` 字段、§3 实测表、HIL（Architect）。
