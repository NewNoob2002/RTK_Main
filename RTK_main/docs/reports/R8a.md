# R8a 报告：app_main 收敛为 boot() + 步骤表、BootRecord、`sys boot`

任务卡：`docs/tasks/R8a-app-boot.md`；基线 `2282a85`（分支 `feat/gnss`）。仅改动 `main/` 与 `tools/main_host_check/`。

## 1. 结论

- `app_main.cpp` 由 627 行收敛为 8 行，只调用 `app::app_context().boot()`。
- 启动流程改为 13 步有序步骤表（power → banner → contexts → console → alloc → bind → seal → debug → storage → system → link → io → started），单点负责成功/失败/跳过、降级与控制台切换、日志。
- 每步记录 `BootRecord`（起始毫秒、耗时、堆三项、结果、原因）；`sys boot` 只读输出该表（console-only 下亦可用），`RTK_BOOT_VERBOSE=y` 在启动末尾自动打印整表。
- 启动顺序、失败原因字符串、回滚/停机语义、BootArena 的 owner 名称/顺序/对齐/容量与 K5 完全一致；`rollback_boot_bindings` / `stop_boot_tasks` / `gated_command` 逻辑逐字迁移，未改语义。
- 所有本地门禁 PASS；静态 DRAM 63,743 B，比 K5 的 63,319 B **+424 B**（预算 ≤ +512 B）。

## 2. 改动文件

新增（`main/`）：

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `main/app/app.hpp` | 60 | 组合根单例 `inline constinit AppContext s_app{}`、启动期内部接口（`gated_command`、`set_console_only`、`boot_records`、`start_console`/`start_link`/`start_io`/`wait_started`/`stop_boot_tasks`） |
| `main/app/app.cpp` | 55 | `app_context()`、`AppContext::await_boot`（自 K5 原样迁入） |
| `main/app/boot_steps.hpp` | 78 | 纯 C++ 步骤协议：`BootResult`/`OnFail`/`BootStep`/`BootRecord`/`BootHeap`/`BootHooks`/`BootCtx` |
| `main/app/boot_steps.cpp` | 102 | `run_boot_steps`：计时、堆采样、故障注入、`kDegrade` 中止 / `kContinue` 继续、记录容量边界 |
| `main/app/boot_buffers.hpp` | 50 | `BootBuffers`（自 K5 原样迁入） |
| `main/app/boot_buffers.cpp` | 183 | `allocate_boot_buffers` / `bind_boot_buffers` / `rollback_boot_bindings`，owner 名/顺序/对齐/容量不变 |
| `main/app/executors.cpp` | 179 | `kModules`、链路/IO 执行器配置、`start_link`/`start_io`/`wait_started`/`stop_boot_tasks` |
| `main/app/console_setup.cpp` | 118 | 控制台命令表/动词组、`gated_command` 相位门禁、`set_console_only` |
| `main/app/boot_flow.cpp` | 355 | `kBootSteps` 表与 13 个步骤函数、横幅与启动摘要、降级处理器、`BootRecord` 存储、`AppContext::boot()` |
| `main/app/boot_report.hpp` | 54 | `app::print_boot_table`（`sys boot` 与 `RTK_BOOT_VERBOSE` 共用） |
| `main/support/reset_reason.hpp` / `.cpp` | 12 / 47 | `app::reset_reason_name`，消除 `reset_reason_name` 与 `reset_reason_str` 的重复 switch |

新增（`tools/main_host_check/`）：

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `tools/main_host_check/boot_steps_check.cpp` | 236 | 步骤执行器 host 测试：全成功、`kDegrade` 中止+原因、`kContinue` 继续、故障注入、记录容量边界 |

修改：

| 文件 | 变化 |
| --- | --- |
| `main/app_main.cpp` | 627 → 8 行，仅 `app::app_context().boot()` |
| `main/app_context.hpp` | 新增 `void boot() noexcept;` 声明（非冻结头） |
| `main/CMakeLists.txt` | `SRCS` 增加 `app/*.cpp`、`support/reset_reason.cpp` |
| `main/Kconfig.projbuild` | 新增 `RTK_BOOT_VERBOSE`（bool，默认 n）、`RTK_BOOT_FAULT_STEP`（string，默认 ""） |
| `main/modules/system/system_cmds.cpp` | 新增 `sys boot`；`kSysVerbs` 5 → 6 项；`reset_reason_str` 删除，改用 `app::reset_reason_name`；`sys` 帮助文本更新 |
| `tools/main_host_check/CMakeLists.txt` | boot 目标改为链接 `main/app/{app,boot_buffers,boot_steps,console_setup}.cpp`；新增 `boot_steps_check` 目标与 `boot_steps` 测试 |
| `tools/main_host_check/boot_alloc_check.cpp` | 去掉 `#include "../../main/app_main.cpp"`，改用 `app/app.hpp` + `app/boot_buffers.hpp`，符号加 `app::` 限定 |

`git diff --stat`：7 个跟踪文件 108 insertions / 712 deletions；新增文件共 1529 行（含测试）。

## 3. 实现要点

- **步骤表**：`constexpr std::array<app::BootStep, 13U> kBootSteps`，全部 `OnFail::kDegrade`（与 K5 一致：所有失败都降级为 console-only；K5 中 `start_console` 失败是裸 `return`，现改为走同一降级路径，效果相同且记录原因）。禁用 GNSS / 关闭 trace 时对应步骤记 `kSkipped`，不从表里删除，因此 `sys boot` 的行号稳定。
- **失败原因字符串**（逐字保留）：`boot_alloc:<owner>`、`bind:debug`/`bind:spp`/`bind:gnss`、`boot_alloc:seal`、`rollback:spp_in_use`、`debug_init`、`storage_init`、`link_init`/`link_task_start`/`link_module_start`、`io_init`/`io_task_start`/`io_module_start`、`task_start_timeout`、`stop:task_timeout`/`stop:gnss`/`stop:spp`。SEALED 后 `BootArena` 永不释放，未变。
- **降级处理器**：`kOpen`（alloc/bind/seal 失败）→ `rollback_boot_bindings`；`kSealed` → `set_degraded_reason`（保留首个原因）后按 `launched` 决定 `stop_boot_tasks` 或 `set_console_only`；`kUninit` → 仅 `set_console_only`。
- **计时/堆采样**：`esp_timer_get_time()/1000`、`heap_caps_get_free_size`/`get_minimum_free_size`/`get_largest_free_block`（`MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`），全部经 `BootHooks` 注入，host 测试不需要 IDF。
- **日志**：横幅与逐句启动摘要走 `cli::out_printf`（单行整写 UART），显式 `\r\n`，因此控制台启动前后都能正确换行，且不冒充 ESP_LOG。示例：

  ```text
  =================================
   E1-Lite RTK_main 6.1.0-dev
  =================================
  Reset: power-on
  Storage online, SN A1B2, whitelist 0
  Bluetooth online, E1-Lite-A1B2 aa:bb:cc:dd:ee:ff, 0 sessions
  GNSS UART2 started, 115200
  Console ready
  Boot 412 ms, heap 148.2 KB free
  ```

  失败时输出 `<Step> failed: <reason>, console-only`。GNSS 探测结果仍由 gnss 模块经既有 tlog 通道输出（未在 main 重复打印）。
- **`sys boot`**（console-only 下可用，只读）：每步 `step/result/start_ms/took_ms/heap_free/heap_min/heap_largest[/reason]`，末行 `boot: 13 steps, 2 skipped, 412 ms total, slowest spp 210 ms`。
- **`RTK_BOOT_FAULT_STEP`**：非空且命中某步名时，该步不执行、记 `kFail`、原因 `fault:<step>`，按 `OnFail` 语义处理；为空即 K5 行为。HIL 注入由审查方执行。
- **`RTK_BOOT_VERBOSE=y`**：启动末尾自动打印整表，默认关闭。

## 4. 门禁结果（原始输出）

全部在 `RTK_main/` 下执行，先 `. ~/esp/esp-idf/export.sh`。

| # | 门禁 | 命令 | 结果 |
| --- | --- | --- | --- |
| 1 | IDF 构建 0 warning | `idf.py build` | **PASS**（`grep -cE "warning:\|error:"` → `0`） |
| 2 | 二进制/分区 | 同上 | `rtk_main.bin binary size 0xc0f30 bytes`（790,320 B），最小 app 分区 0x1a0000，54% free |
| 3 | 静态 DRAM | `xtensa-esp-elf-size -A build/rtk_main.elf` | **PASS**：`.dram0.data 20623` + `.dram0.bss 43120` = **63,743 B**（K5 63,319 B，**+424 B**） |
| 4 | trace/GNSS 全关构建 | `idf.py -B builds/r8a-off -D SDKCONFIG=builds/r8a-off/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;builds/r8a-off/off.defaults" build` | **PASS**：0 warning/0 error；`# CONFIG_RTK_TRACE_ENABLE is not set`、`# CONFIG_RTK_GNSS_ENABLE is not set`；bin 0xaf8d0；`.data 20447 + .bss 37544` |
| 5 | host：test/host | `cmake -S test/host -B builds/r8a-host2 && cmake --build … && ctest` | **PASS** 7/7，0 warning |
| 6 | host：t13 | `tools/t13_host_check` | **PASS** 43/43 |
| 7 | host：main_host_check | `tools/main_host_check`（含新增 `boot_steps`） | **PASS** 44/44 |
| 8 | host：services | `test/services` | **PASS** 6/6 |
| 9 | host：cli | `test/cli` | **PASS** 3/3 |
| 10 | host：gnss | `test/gnss` | **PASS** 5/5 |
| 11 | host：stream_demux | `test/stream_demux` | **PASS** 4/4 |
| 12 | host：app_fw | `test/app_fw` | **PASS** 6/6 |
| 13 | host：bt_reconnect | `test/bt_reconnect` | **PASS** 1/1 |
| 14 | ASan+UBSan 全套 | 上述 9 个套件各加 `-fsanitize=address,undefined` 重建 | **PASS**：app_fw 6/6、bt_reconnect 1/1、cli 3/3、gnss 5/5、test/host 7/7、services 6/6、stream_demux 4/4、main_host_check 44/44、t13 43/43 |
| 15 | clang-format | `clang-format --style=file --dry-run --Werror <diff 中的 .c/.cpp/.h/.hpp>` | **PASS**（格式化后 `cf_exit=0`） |
| 16 | G5b clang-tidy | `python3 -I tools/tidy_check/tidy_check.py --host-build builds/host --idf-build build` | **PASS**：`文件 30 个，发现 0 条` / `G5b PASS` |
| 17 | G7b 堆/异常门禁 | `python3 -I tools/g7b_check/g7b_check.py --build build` | **PASS**：自有目标 70 个，堆白名单 1 个源文件放行 0 个堆符号，ELF 异常/RTTI 符号 6/6 均在白名单 |
| 18 | 冻结文件 | `sha256sum -c docs/frozen.sha256` | **PASS**：全部 `OK`，退出码 0 |

新增 host 测试输出：`boot_steps_check: OK`（ctest 名称 `boot_steps`，在 main_host_check 的 44 项中）。

## 5. DRAM 对比

| 段 | K5（基线） | R8a | 差 |
| --- | --- | --- | --- |
| `.dram0.data` | 20,623 | 20,623 | 0 |
| `.dram0.bss` | 42,696 | 43,120 | **+424** |
| 合计 | 63,319 | **63,743** | **+424 B**（预算 ≤ +512 B） |

新增 .bss 来自 `std::array<BootRecord, 13>`（32 B/条 = 416 B）与执行计数/对齐余量 8 B；`BootBuffers` 在函数栈上，未进 .bss。

## 6. 与任务卡的偏差（均为实现选择，需审查确认）

1. **入口形态**：采用 `AppContext::boot()` + `app_main.cpp` 八行，而非卡中并列的 `app::App::boot()`。卡的 §2 允许二选一；`AppContext` 已持有全部模块与 arena，再加一层 `App` 空壳只会多一次转发。
2. **未引入 `main/cmd_list.inc` X 宏**：`cmd_tables` 与 `verb_groups` 不是一一对应（`system` 命令表提供 `status`/`log`/`sys` 三组动词，`link`/`config` 命令表没有动词组），强行合表会引入额外映射。改为在 `main/app/console_setup.cpp` 保留两个常量数组并写明原因；允许的命令集合与 K5 逐字相同（`status|log|sys|config`）。
3. **横幅复位行用既有小写连字符名称**（如 `Reset: power-on`、`Reset: task-wdt`），而非卡示例的 `POWERON`：复用 `esp_reset_reason_t` → 名字的唯一映射（`app::reset_reason_name`），避免第二套命名；该映射也是 `sys info` 里 `reset_reason=` 的既有输出。
4. **`kContinue` 无生产步骤使用**：卡 §5 要求枚举与 host 测试覆盖"继续"语义，但 §2 又要求不改运行期行为，K5 没有任何一步是"失败即继续"，因此枚举保留、`boot_steps_check` 覆盖，生产表 13 步全为 `kDegrade`。
5. **`start_console` 失败路径**：K5 是裸 `return`（无原因、无 console-only），现统一走步骤表降级路径并记录 `console` 步骤失败；可观察行为相同（都不启动后续任务），但 `sys boot` 现在能看到原因。
6. **`s_app` 位置**：从 `app_main.cpp` 的匿名命名空间移到 `main/app/app.hpp` 的 `inline constinit` 变量，因为执行器模块表的 `&app::s_app.member` 需要跨翻译单元常量地址；`constinit` 单例语义不变。`app.hpp` 非冻结头，已在文件注释说明。
7. **`sys` 帮助文本**由 `<info|heap|mem|tasks|reboot>` 改为 `<info|heap|mem|boot|tasks|reboot>`（新增子命令的必然结果，属允许的文本更新）。

## 7. 未验证项

- **HIL 未执行**（受授权限制，未 `idf.py flash`/`monitor`、未连接串口）：真实 UART 上横幅/摘要的字节形态、`sys boot` 的实机输出、`RTK_BOOT_FAULT_STEP` 注入后的降级与停机时序、GNSS 探测行与启动摘要的先后顺序，均待审查方 HIL 确认。
- **`RTK_BOOT_VERBOSE=y` 与 `RTK_BOOT_FAULT_STEP` 非空**的组合未做编译期矩阵验证（默认值下已构建通过；两者只影响 main 内部常量与一个 `#ifdef` 分支）。
- 报告中的启动耗时示例（412 ms 等）是格式示意，不是实机测量值。

## 审查方修正与 HIL（2026-11-16，Claude Opus 5.5）

### 审查发现并已修正（main/app/boot_flow.cpp、main/app/boot_report.hpp）

1. **link/io 失败未把 trace_out 计入停机等待**：实施版只在 `started` 步骤加 `launched |= 4`。新增 `mark_trace_launched()`，`link`/`started` 入口调用，与 K5 一致在任务启动阶段任何失败时等待 trace_out 停稳。
2. **link 失败且 launched==0 时未调用 `stop_boot_tasks`**：K5 进入任务启动阶段后无条件 `stop_boot_tasks(launched)`（含 `gnss.stop_boot`/SPP 停止）。现 `step_link`/`step_started` 入口置 `ctx.stop_required`，`handle_degrade` 以 `stop_required || launched != 0` 判定停机。
3. **故障注入跳过 bind/seal 步骤体时未回滚**：分配器仍为 OPEN。`handle_degrade` 在 `boot_arena.state()==kOpen` 时调用 `rollback_boot_bindings`（解绑后 abort），实测 `fault:bind` → `state ABORTED`。
4. **`fault:<step>` 原因悬垂**：`BootRecord::reason` 指向栈上 `BootCtx::fault_reason`，启动任务返回后 `sys boot` 打印乱码；`BootCtx` 改为函数内 `static constinit`。
5. **alloc 失败记录无原因**：`step_alloc` 失败时取 `boot_arena.reason()`（`boot_alloc:<owner>`）。
6. **`sys boot` 列未对齐**：改为定宽列（step/result/start ms/took ms/heap free/heap min/largest）。

修正后 DRAM 63,807 B（K5 63,319，+488 ≤ +512）。门禁复跑：idf build 0 warning、r8a-off 0 warning、main_host_check 44/44、t13 43/43、services 6、cli 3、gnss 5、G5b PASS、G7b PASS、clang-format clean、frozen 0 fail。

### HIL（/dev/ttyACM0）

- 冷启动日志：
  ```
  =================================
   E1-Lite RTK_main 6.1.0-dev
  =================================
  Reset: power-on
  Storage online, SN D8B2, whitelist 1
  Bluetooth online, E1-Lite-D8B2 F8:B3:B7:25:D8:B2, 0/2 sessions
  GNSS UART2 started, 115200 baud
  Console ready
  Boot 1057 ms, heap 93.6 KB free
  ```
- `sys boot`：13 步全部 ok，总 1033 ms，最慢 console 564 ms（含 esp_console/终端探测）、system 449–474 ms（BT 控制器初始化）。
- `RTK_BOOT_FAULT_STEP="bind"`（builds/r8a-fbind）：`bind failed: fault:bind, console-only`，arena ABORTED，`sys mem` 显示 `degraded (console-only: fault:bind)`，业务命令回 `ERR unavailable: console-only`。
- `RTK_BOOT_FAULT_STEP="link"`（builds/r8a-flink）：`link failed: fault:link, console-only`，arena SEALED（块不释放），停机后 bt 状态 ready、link not running，status/sys/config 可用。
- spp_stress `--mode all` TOTAL lost=0；win16 ×3000 ×4：33.5–37.7 KiB/s，win32 ×4：36.2–43.8 KiB/s，全部 0 丢；与同日 K5（win16 34.4–36.0、win32 35.2–36.9）相当，未见回退。
- 已知（未改）：横幅后 `W (260) uart_vfs: light slee…` 一行被 console UART 切换截断（推测为既有现象，未在 K5 固件上复核）；`Reset:` 名称为小写（power-on），与卡样例 `POWERON` 不同，作为实施偏差接受。
