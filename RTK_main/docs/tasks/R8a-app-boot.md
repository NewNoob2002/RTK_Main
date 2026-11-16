# R8a：app_main 收敛为启动入口 + 启动步骤表与启动日志（2026-11-16，待 K5 提交后下达）

状态：ISSUED 2026-11-16（基线 K5 提交 1e03c15，已按 K5 实际代码核对，见 §0）。方向：参照 X-TRACK `App.cpp` 收敛入口、参照 SparkFun RTK Everywhere `setup()` 的启动计时/在线状态。实施：pi + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

## 0 K5 基线事实（以此为准，与下文冲突时以本节为准）

- `main/app_main.cpp` 627 行，实际启动顺序：`enable_power_hold` → 版本/复位原因日志 → `bind_contexts`、执行器指针、`rtcm_pipe.init_lock`、`data.init` → **`start_console()`（先于一切分配；控制台只有一份命令表，由 `gated_command` 按 `BootPhase` 门控：kStarting/kStopping 回 `unavailable: booting`，kConsoleOnly 只放行 status/log/sys/config）** → `allocate_boot_buffers` → `bind_boot_buffers` → `boot_arena.seal()` → `debug.init()` → `storage.init()` → `system.prepare()` → `start_link` → `start_io` → `wait_started` → `BootPhase::kRunning`；任务已启动后失败走 `stop_boot_tasks(launched)`。
- 失败处理现状：控制台启动失败直接 return；分配失败 abort arena（reason `boot_alloc:<owner>`）；绑定/封存失败 `rollback_boot_bindings(bufs, reason)`（reason `bind:debug`/`bind:spp`/`bind:gnss`/`boot_alloc:seal`，`rollback:spp_in_use` 时保留全部块）；`debug_init`、`storage_init`（**K5 起 storage 失败也降级，不再“记错继续”**）、`link_init`/`link_task_start`/`link_module_start`、`io_init`/`io_task_start`/`io_module_start`、`task_start_timeout`、`stop:task_timeout`/`stop:gnss`/`stop:spp` 经 `boot_arena.set_degraded_reason`。这些 reason 字符串、回滚/停机语义、SEALED 块永不释放的约束**一字不改**。
- `tools/main_host_check/boot_alloc_check.cpp:30` 直接 `#include "../../main/app_main.cpp"` 测试 `allocate_boot_buffers(…, ops)`（故障注入经 `rtos::BootAllocatorOps`）、`rollback_boot_bindings`、`bind_boot_buffers`、`gated_command`。拆文件后改为包含/链接新文件，**这些测试一项不删、语义不变**；`tools/main_host_check/CMakeLists.txt` 可相应修改。
- `reset_reason_name`（app_main.cpp:230）与 `reset_reason_str`（main/modules/system/system_cmds.cpp:63）重复。
- `sys` 组已有 `mem`/`tasks`（system_cmds.cpp:548），`sys boot` 加在同一表。

## 1 目标

1. `main/app_main.cpp` 只保留入口：`extern "C" void app_main(void)` 调用 `app::App::boot()`（或 `AppContext::boot()`，命名由实施者定，报告说明理由），约 10–30 行。
2. 启动流程改为**一张有序步骤表**由统一执行器运行；成功/失败/跳过的处理、降级控制台、日志都只写一处（当前 4 段重复的“ESP_LOGE + set_degraded_reason + start_console(true) + return”合并）。
3. 启动日志：成功时也有简洁的一句话输出（当前组件成功时完全无日志）；完整计时/堆表按需用 `sys boot` 查看。
4. 纯重构 + 新增只读诊断：**不改**协议、NVS 命名空间/键、BT 名称、执行器优先级/核/栈、模块顺序、运行时行为、K5 的内存布局与降级语义。

## 2 文件布局（建议，实施者可微调并在报告说明）

| 文件 | 内容 |
| --- | --- |
| `main/app_main.cpp` | 仅入口 |
| `main/app/app.hpp/.cpp` | `App`：持有/引用 `s_app`、执行器对象；`boot()` 跑步骤表；`app_context()` 定义移到这里 |
| `main/app/boot_steps.hpp/.cpp` | `BootStep{name, fn, policy}`、`BootRecord`、步骤执行器、`kSteps[]` |
| `main/app/boot_buffers.cpp` | K5 的 `BootBuffers`、`allocate_boot_buffers`、`bind_boot_buffers`、`rollback_boot_bindings`（原样搬出；16 段重复的 alloc_array/fail_alloc 可改为表驱动，但 owner 名称、顺序、对齐、caps 与 `sys mem` 输出必须逐项不变） |
| `main/app/executors.cpp` | `kLinkConfig/kIoConfig`、`start_link/start_io`（配置常量也可放 `app_config.hpp`） |
| `main/app/console_setup.cpp` + `main/cmd_list.inc` | `start_console`、`gated_command`、命令/动词组表；cmd_tables 与 verb_groups 两份手写列表合并为一份 X-macro 清单（名称、summary、cmds、verbs、console-only 是否放行、#if 条件），放行集合 status/log/sys/config 不变 |
| `main/support/reset_reason.*` | `reset_reason_name`，与 `main/modules/system/system_cmds.cpp` 的 `reset_reason_str` 去重 |

`kModules`/`module_list.inc`/`AppContext` 结构保持；`s_app` 仍为单个 constinit 对象（K5 的 .bss 布局不变）。

## 3 启动步骤表

```cpp
enum class BootResult : uint8_t { kPending, kOk, kSkipped, kAbsent, kFail };
enum class OnFail : uint8_t { kDegrade, kContinue };   // kDegrade → console-only；kContinue → 记 FAIL 继续
struct BootStep   { const char *name; BootResult (*fn)(BootCtx &) noexcept; OnFail on_fail; };
struct BootRecord { const char *name; BootResult result; uint32_t start_ms, took_ms;
                    uint32_t heap_free, heap_min, heap_largest; const char *reason; };
```

- 步骤顺序与 §0 的 K5 顺序完全一致（power → banner → contexts → console → alloc → bind → seal → debug → storage → system → link → io → started）；`#if RTK_GNSS_ENABLED`/`FW_TRACE_ENABLED` 关闭的步骤记 `kSkipped`，不从表里删（表长度可 constexpr 计算）。
- K5 当前所有失败都是降级（console 启动失败为直接返回），全部保持；`OnFail::kContinue` 只在实施者确有现存“记错继续”分支时使用，没有就不引入该枚举值。失败后的回滚（bind/seal 失败 → rollback；任务启动后失败 → stop_boot_tasks）由执行器按步骤所处阶段统一调用，reason 字符串取 §0 现值。
- 每步结束记 `esp_timer_get_time()/1000`、`heap_caps_get_free_size/minimum_free_size/largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)`。`BootRecord` 数组静态（≤16 项），启动后只读。
- 不使用 SparkFun 的 `DMW_b` 宏式插桩，不使用 `reportFatalError` 式死循环停机（保留 console-only 降级）。

## 4 启动日志格式

默认输出（面向客户，一句话一条，遵循 memory《console-and-log-style》）：

```
=================================
 E1-Lite RTK_main <version> (<git>)
=================================
Reset: POWERON
Storage online, SN <sn>, whitelist <n>
Bluetooth online, <name> <mac>, <n> sessions
GNSS UART<n> started, <baud>
Console ready
Boot <ms> ms, heap <x.y> KB free
```

- 上次复位为 PANIC/INT_WDT/TASK_WDT/BROWNOUT 时多一行 `Warning: last reset was <reason>`。
- 失败：`<Step> failed: <reason>, console-only`（kDegrade）或 `<Step> failed: <reason>`（kContinue）。
- GNSS 接收机探测是异步的（io 执行器），由 gnss 模块在探测结束时补一句 `GNSS <model> online, <fw>` 或 `GNSS not detected`（经现有 tlog/日志路径，非告警级别）；若该句已存在则只统一措辞。
- 摘要信息来源优先用已有 DataCenter Slot（SystemStatus/LinkStatus 等）、Kconfig 常量和 IDF API（如 `esp_bt_dev_get_address`）；**不得为此修改冻结头**。确需组件新增信息接口且该头冻结时，停下并在报告里给出精确签名。组件内部不新增 `ESP_LOGI`（组件保持安静，日志由步骤执行器统一打）。
- 输出走现有统一输出路径（ADR-0009 Plan A：ESP_LOG 经 `esp_log_set_vprintf` 直写 UART）；横幅/句子是否带 ESP_LOG 前缀由实施者按现有 console 风格决定并在报告给出实际样例。

`sys boot`（新增只读 verb，挂在现有 `sys` 组下，降级控制台也可用）：

```
step      result  start ms  took ms  heap free  heap min  largest
power     ok           0        0    ...
arena     ok           1        3    ...
...
total                         520
slowest: spp 410, storage 12, arena 3
```

Kconfig `RTK_BOOT_VERBOSE`（main/Kconfig.projbuild，默认 n）：y 时启动结束自动打印上表。

## 5 验收门禁

- idf 主工程 `build/` 0 warning；`idf.py size` 静态 DRAM 与 K5 基线差 ≤ +512 B（BootRecord 表+字符串），报告给出实测差值；`builds/r8a-off`（`CONFIG_RTK_GNSS_ENABLE=n` + `CONFIG_RTK_TRACE_ENABLE=n`）也 0 warning。
- 全部 host 套件（test/host、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/cli、tools/main_host_check 含 ASan、tools/t13_host_check）通过；新增 host 测试：步骤执行器（kDegrade 中止并记 reason、kContinue 继续、kSkipped 记录、计时/堆回调可注入）。
- clang-format 唯一格式门禁（AGENTS.md:94，禁止 astyle）；G5b `tools/tidy_check/tidy_check.py` 0 新增；G7b `python3 -I tools/g7b_check/g7b_check.py --build build` PASS；`sha256sum -c docs/frozen.sha256` 全 OK（本卡不改任何冻结文件）。
- 新增 Kconfig `RTK_BOOT_FAULT_STEP`（string，默认空，help 注明仅供 HIL；非空时名为该值的步骤直接返回 kFail，reason `fault:<step>`），供审查方在板上验证降级；默认空时编译出的行为与 K5 相同。
- HIL（审查方执行）：冷启动/软复位日志与 §4 一致；`sys boot` 输出；用 `RTK_BOOT_FAULT_STEP` 制造降级，显示 `failed: ..., console-only`，status/log/sys/config 可用、其他命令回 `unavailable: console-only`；spp_stress `--mode all` 0 丢、win16/win32 不低于 K5 基线 −2%。

## 6 约束

- 每个逻辑步骤一个提交候选（拆文件 / 步骤表 / 日志与 sys boot / reset_reason 去重），但实施者不 commit；完成后写 `docs/reports/R8a.md`（逐项 PASS/FAIL/NOT_RUN，附启动日志与 `sys boot` 期望样例、DRAM 差值、文件行数前后对比）并停下。
- 不连串口、不烧录、不安装/联网；构建只用 `build/` 与 `builds/<name>/`。
- 不改 test/host、reference、AGENTS、sdkconfig.defaults、partitions、任何 `docs/frozen.sha256` 所列文件、components/ 下源码（组件保持安静：本卡只动 main/ 与 tools/main_host_check）；ADR-0007/0009 与本卡冲突处在报告列出，不自行改。
- 文件首行说明、snake_case 文件名、中文注释风格按 ADR-0008；提交 trailer `Generated-by: pi + <model>`。
