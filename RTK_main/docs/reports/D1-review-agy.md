# D1 交叉评审报告（AGY）

- **评审对象**：分支 `feat/d1-console-profile`，提交 `b4d5488`（基线 `362035c`）
- **依据文档**：`AGENTS.md`、`docs/adr/ADR-0016-console-profiles.md`（§2.1–§2.6）、`docs/tasks/D1-console-profile.md`、`docs/reports/D1.md`
- **评审方式**：静态代码审查 + 既有构建产物/日志核验（`builds/d1-idf`、`builds/d1-production`、`builds/d1-console-none`、`builds/d1-logs/`、`nm -C`、`strings`、`sha256sum -c docs/frozen.sha256`）；未修改源码、未提交、未烧录。
- **结论汇总**：**0 阻塞 / 3 应改 / 2 建议**。核心功能（三档 Kconfig、PRODUCTION/NONE 命令与 help 裁剪、链接器段回收、factory 依赖的 BT 函数保留、`kConsoleOnly` 门控、NONE 降级不重启/不调 REPL、新 CI 门禁与冻结文件保护）均满足 ADR-0016 与 D1 要求；存在 1 处 `config build` 输出格式不一致及 2 处主机测试覆盖缺口需要整改后再合入。

---

## 1. 评审发现清单

### [应改-1] `config build` 绕过 `cli::printKv`，破坏 `[build]` 节格式且与 `console` 键名冲突

- **位置**：`main/modules/storage/ConfigCmds.cpp:116-120`（具体 `:119`）
- **问题**：`showBuild()`（以及整个 `config` 命令的 `[identity]`/`[link]`/`[build]` 各节）其余所有行均调用 `cli::printKv(key, fmt, ...)` 输出统一的 `  %-14s %s\n`（2 空格缩进、14 字符左对齐键名、复合键用 `snake_case`，如 `gnss_uart`、`gnss_power`、`gnss_port`、`boot_mode`）。第 119 行却直接使用 `cli::outPrintf("console profile %s\n", kConsoleProfileName);`：
  1. 无 2 空格缩进、无列对齐，在 `[build]` 节中输出突兀：
     ```text
     [build]
       console        UART0 2000000 baud
     console profile dev
       trace          compiled in
     ```
  2. 首词同样为 `console`，与上一行 `cli::printKv("console", ...)` 键名冲突；任何按 `key value` 解析 `[build]` 节的脚本都会误判或覆盖 `console` 键。
- **触发场景**：DEV 或 PRODUCTION 固件在控制台执行 `config` 或 `config build`。
- **严重度**：**应改**
- **建议改法**：改为 `cli::printKv("console_profile", "%s", kConsoleProfileName);`（与同函数 `gnss_uart`/`gnss_power`/`gnss_port` 风格一致）。

---

### [应改-2] `console_profile_check` 对 PRODUCTION 的 `verbGroupNames` 仅传 `(false, false)`，未覆盖真实量产配置（`gnssEnabled=true`）

- **位置**：`tools/main_host_check/ConsoleProfileCheck.cpp:41-43`
- **问题**：
  ```cpp
  static_assert(std::ranges::equal(verbGroupNames(ConsoleProfile::kProduction, false, false) |
                                       kRegistered,
                                   kProdGroups));
  ```
  实际量产配置 `configs/production.defaults` 开启了 GNSS（`CONFIG_RTK_GNSS_ENABLE=y`，即 `gnssEnabled=true`），且手动配置也可能同时开启 trace（`traceEnabled=true`）。此处只传 `(false, false)`，未能验证 `gnss`/`debug` 动词组在 `PRODUCTION` 下不会被错误放行——若 `ConsoleRegistration.hpp:74-75` 遗漏 `dev &&` 守卫，该测试仍会静默通过。此外也未覆盖 `verbGroupNames(ConsoleProfile::kDev, false, false)`。
- **触发场景**：后续修改 `ConsoleRegistration.hpp` 导致 `PRODUCTION` 在启用 GNSS 或 trace 时误登记 `gnss`/`debug` 动词组，`console_profile_check` 无法拦住。
- **严重度**：**应改**
- **建议改法**：将第 41 行改为或补充对 `(ConsoleProfile::kProduction, true, true)`、`(ConsoleProfile::kProduction, true, false)` 以及 `(ConsoleProfile::kDev, false, false)` 的 `static_assert`。

---

### [应改-3] `BootStepsCheck` 仅测试 `finishDegradedConsole<kNone>`，未覆盖新增模板的 `kDev`/`kProduction` 降级分支

- **位置**：`tools/main_host_check/BootStepsCheck.cpp:253-284`（对应实现 `main/app/ConsoleProfile.hpp:31-41`）
- **问题**：本次把 `BootFlow.cpp` 的降级控制台收尾抽成三档共用的 `app::finishDegradedConsole<Profile, Init, Attach, Run>(consoleReached)`，但 `testNoneDegradesWithoutConsole()` 仅实例化并断言了 `ConsoleProfile::kNone`，未对 `kDev` 与 `kProduction` 验证：
  1. 早期故障（`consoleReached == false`）且 `Init()` 返回 `true` → 依次调用 `Init`、`Attach`、`Run`；
  2. 早期故障（`consoleReached == false`）且 `Init()` 返回 `false` → 调用 `Init`，跳过 `Attach` 与 `Run`；
  3. 晚期故障或 `console` 步自身失败（`consoleReached == true`）→ 跳过 `Init`，调用 `Attach` 与 `Run`。
  同时第 273 行以 `records[5].result == kOk` 计算 `consoleReached`，与 `BootFlow.cpp:485`（按 `record.name == "console"` 判定，即使 `console` 步失败也为 `true`）语义略有出入，扩测 `faultStep="console"` 时应保持与 `BootFlow.cpp` 一致。
- **触发场景**：调整 `finishDegradedConsole` 或降级逻辑时，`DEV`/`PRODUCTION` 的故障补初始化控制台行为缺少主机测试回归保护。
- **严重度**：**应改**
- **建议改法**：在 `BootStepsCheck.cpp` 补充覆盖 `kDev` 与 `kProduction` 三种组合（`consoleReached=false` + `Init=true/false`、`consoleReached=true`）及 `kNone` 早期/晚期故障的断言，验证各钩子调用次数与顺序。

---

### [建议-1] `registerCommandTables()` 依赖优化器死分支消除且对多命令表只抽检首命令名

- **位置**：`main/app/ConsoleSetup.cpp:96-116`（对比同文件 `:150-170`）
- **问题**：
  1. `registerVerbGroups()` 使用 `constexpr std::array kVerbGroups` 存放函数指针，禁用项在语言层面即固定为 `nullptr`，与优化级别无关；而 `registerCommandTables()` 用运行期 `const std::array cmdTables` + 三目运算符（`kEnabled("bt") ? linkCmds() : kNoCommands`），当前 `-O2`（`CONFIG_COMPILER_OPTIMIZATION_PERF=y`）下 GCC 能常量折叠并消除未引用表（已通过 `nm` 实测确认），但若以 `-O0`（`CONFIG_COMPILER_OPTIMIZATION_NONE`）编译，死分支仍会生成对禁用表的未定义引用，使 `--gc-sections` 失效。
  2. `systemCmds()` 包含 `status`/`log`/`sys` 三条命令，`linkCmds()` 包含 `bt`/`alarm` 两条命令，此处仅分别检查 `kEnabled("status")` 与 `kEnabled("bt")`。当前三档下同组开关一致故无误，但与 `ConsoleRegistration.hpp` 存在隐式耦合。
  3. 附注：`kNames` 用空串 `""sv` 表示关闭项，故 `kEnabled("")` 在 PRODUCTION/NONE 下为 `true`（实表无空命令名，当前无害，加 `!name.empty()` 更稳妥）；另外 `kVerbGroups` 未将禁用项的 `name`/`summary` 置空，导致 `"RTCM forwarding stats and reset"` 等 3 条摘要字面量（约 100 B）仍留在 PRODUCTION 的 `.rodata` 中。
- **触发场景**：以 `-O0` 编译 PRODUCTION 固件时 `--gc-sections` 无法回收未登记命令表；或未来某档位单独启用 `alarm`/`log`/`sys` 时漏登记。
- **严重度**：**建议**
- **建议改法**：`registerCommandTables()` 仿照 `registerVerbGroups()` 使用 `constexpr` 函数指针表，并以表内任一命令启用作为拉表条件（`kEnabled("status") || kEnabled("log") || kEnabled("sys")`、`kEnabled("bt") || kEnabled("alarm")`）；`kVerbGroups` 禁用项同时将 `name`/`summary` 置 `nullptr`。

---

### [建议-2] `console_profile_check` 编译选项缺少 `-fno-exceptions -fno-rtti` 及严格告警，且未测 `CONFIG_*` 宏映射

- **位置**：`tools/main_host_check/CMakeLists.txt:236-239`
- **问题**：同文件所有其他测试目标（如 `:232-233` `boot_steps_check`、`:244-246` `board_profile_check`）均开启 `-fno-exceptions -fno-rtti` 及项目标准告警选项（纯目标还开启 `-Wconversion -Wsign-conversion -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef`）；`console_profile_check` 仅设置了 `-Wall -Wextra -Werror -Wshadow`（而 `run_gates.sh:51` 的 ASan 假定目标关闭 RTTI）。此外 `ConsoleProfile.hpp:15-24` 的 `CONFIG_RTK_CONSOLE_PROFILE_*` 宏到 `kConsoleProfile`/`kConsoleProfileName` 映射未在主机测试中用 `-D` 分别编译验证。
- **触发场景**：头文件引入异常/RTTI 或窄化/未定义宏告警，或宏拼写偏差时主机测试未能捕获。
- **严重度**：**建议**
- **建议改法**：为 `console_profile_check` 补齐与 `board_profile_check` 同等级的编译选项，并可在 host 侧以三种 `-DCONFIG_RTK_CONSOLE_PROFILE_*`（或默认）编译断言 `kConsoleProfile` 与 `kConsoleProfileName`。

---

## 2. 逐项核对记录（ADR-0016 §2.1–§2.6 与 D1 目标/测试）

| 核对项 | 依据 | 结果 | 证据与说明 |
|---|---|---|---|
| Kconfig `RTK_CONSOLE_PROFILE` choice 与默认值 | ADR-0016 §2.1 / D1 §1.1 | **PASS** | `main/Kconfig.projbuild:90-107` 定义三档，默认 `RTK_CONSOLE_PROFILE_DEV`，中文 help 引用 ADR-0016；`sdkconfig.defaults` 未改；`CONFIG_RTK_CONSOLE_PROFILE_*` 仅出现在 `ConsoleProfile.hpp:15-24`，未散布到业务代码。 |
| DEV 行为不变 | ADR-0016 §2.2 / D1 §1.2 | **PASS** | 11 个命令与 8 个动词组顺序、隐藏属性与 `kConsoleOnly` 门控均与基线一致；`sdkconfig` 默认选择 `DEV` 与 `TRACE_ENABLE=y`。 |
| PRODUCTION 命令与 help 登记（不漏不多） | ADR-0016 §2.3 / D1 §1.2 | **PASS** | `ConsoleRegistration.hpp` + `ConsoleSetup.cpp` 精确登记 `status/log/sys/config/factory(hidden)/freset` 与动词组 `status/log/sys/factory`；`gnss/rtcm/bt/alarm/debug` 未登记；`s_gated_commands[used] = command` 保留 `factory.hidden = true`，`help` 不列 `factory` 而 `help factory` 可查。 |
| `kConsoleOnly` 门控 | ADR-0016 §2.2–§2.3 / D1 §1.2 | **PASS** | `ConsoleRegistration.hpp:22-26` `isSafe` 固定为 `status/log/sys/config`；`ConsoleSetup.cpp:55-67` `gatedCommand` 保持 `kStarting/kStopping` 全拒、`kConsoleOnly` 仅放行 safe（`factory`/`freset` 被拒）。 |
| `factory` 依赖的 BT 函数与 `status` 依赖在 PRODUCTION 可用 | ADR-0016 §2.3 | **PASS** | `nm -C --defined-only builds/d1-production/rtk_main.elf` 实测确认：`app::openDefaultPairing()`（`0x400ebeec`）、`app::editTrustedAlarm(bool, char const*)`（`0x400ebf24`）、`app::confirmFactoryReset`（`0x400e9be8`）、`app::printRtcmStatus()`（`0x400ec780`）均已链接入 ELF。 |
| 链接器裁剪未登记表与 REPL/linenoise | ADR-0016 §2.3–§2.4 / D1 §1.3 | **PASS**（附建议-1） | 独立 `nm` 核验：`builds/d1-production/rtk_main.elf` 与 `ConsoleSetup.cpp.obj` 均不含 `linkCmds/btVerbs/rtcmCmds/rtcmVerbs/gnssCmds/gnssVerbs/debug*` 及 `cmdBt/btStatus/cmdAlarm/cmdTrust/cmdUntrust/cmdRtcm*/cmdGnss*`；`builds/d1-console-none/rtk_main.elf` 与 `BootFlow.cpp.obj` 均不含 `esp_console_*`、`linenoise*`、`initConsole`、`runConsole`、`__wrap_esp_console_run`、`s_command_gates`、`s_gated_commands`，仅保留 `app::installDirectLog()` 与 `cli::outWrite()`。 |
| NONE 启动、日志直写与降级路径 | ADR-0016 §2.4 / D1 §1.3 | **PASS** | `BootStepList.inc:8` 保留 `console` 步骤；`BootFlow.cpp:178-187` 在 `kNone` 下打 I `console disabled (profile none)` 返回 `kOk`；成功路径 `BootFlow.cpp:503-506` 执行 `printBootSummary`、`attachDebugLog()` 后跳过 `runConsole()`；失败路径 `handleDegrade`/`stopBootTasks` 置 `kConsoleOnly` 且不调用 `esp_restart()`，`BootFlow.cpp:495-496` 通过 `finishDegradedConsole<kNone>` 跳过全部 REPL 钩子直接返回。 |
| 横幅与 `config build` 显示 | ADR-0016 §2.5 / D1 §1.4 | **部分通过**（见应改-1） | `BootFlow.cpp:121-122` 横幅输出 `board %s, console profile %s` 符合要求；`ConfigCmds.cpp:119` 改用 `cli::outPrintf` 破坏 `[build]` KV 格式且键名冲突。 |
| 叠加文件 `configs/*.defaults` | ADR-0016 §2.5 / D1 §1.5 | **PASS** | `configs/production.defaults` 与 `configs/console-none.defaults` 均精确配置对应档位 + `CONFIG_RTK_TRACE_ENABLE=n`；对应生成 `builds/d1-*/sdkconfig` 已核验生效。 |
| `run_gates.sh` 新门禁与 CI 覆盖 | ADR-0016 §2.5 / D1 §1.6 | **PASS** | `tools/ci/run_gates.sh:110-115` 新增 `G3-idf-production` 与 `G3-idf-console-none`，写法与 `G3-idf-trace-off` 一致；`.github/workflows/gates.yml:83` 调 `--only-idf` 自动覆盖；`G7b` 仍只查默认构建；三档构建日志均 0 warning。 |
| 不变项与冻结文件保护 | ADR-0016 §2.6 / D1 §1.8 | **PASS** | `sha256sum -c --quiet docs/frozen.sha256` 188 项全 PASS；UART0 波特率、factory 密钥默认值、任务参数、分区表、NVS 键、蓝牙名均未改动。 |
| 主机测试三档覆盖 | D1 §2 | **部分通过**（见应改-2、应改-3、建议-2） | `builds/d1-logs/G2-main_host_check.log` 与 `G2b` 均 61/61 PASS；但 `ConsoleProfileCheck.cpp:41` 对 PRODUCTION 动词组未测 `gnssEnabled=true`，`BootStepsCheck.cpp` 未测 `finishDegradedConsole` 的 `kDev`/`kProduction` 分支。 |
| 报告 `docs/reports/D1.md` 实测表与门禁证据 | D1 §1.7 / §3 | **PASS** | 逐字节核对 `builds/d1-logs/D1-size-{idf,production,console-none}.log` 及 `G5b-tidy.log`（0 条）、`G6-doc_check.log`（10/10 合格）、`G7b-runtime.log`（PASS），尺寸表与验证记录均真实一致；空闲堆与 HIL 正确标为 `NOT_RUN`。 |
