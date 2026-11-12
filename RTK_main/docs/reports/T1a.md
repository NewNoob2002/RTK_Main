# T1a 报告：fw::trace + cli 回执/动词分派 + 调试串口（ADR-0009）

## 1. 改动文件

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `components/app_fw/src/trace.cpp` | 新建 | `fw::trace` 全局接线（掩码原子、sink/clock 一次性安装）+ `fw::TraceRing`（临界区只做索引运算 + 一次 16 B 拷贝，满则丢新并计数） |
| `components/app_fw/CMakeLists.txt` | 修改 | `_app_fw_srcs` 追加 `src/trace.cpp` |
| `test/app_fw/test_trace.cpp` | 新建 | 10 个用例：掩码/安装校验/事件字段与时钟/环 init 校验/满丢新且保序/多轮 wrap/部分取空/注入锁/sink 适配器 |
| `test/app_fw/CMakeLists.txt` | 修改 | foreach 追加 `test_trace` |
| `components/cli/src/reply.cpp` | 新建 | `fail`/`dispatch`/`print_usage`/`print_section`/`print_kv`，只用 `<cstdio>`/`<cstdarg>`，无 IDF 头 |
| `components/cli/src/reply_state.hpp` | 新建 | 模块内部 `detail::reply_reset()` / `reply_failed()`（回执状态文件静态 bool） |
| `components/cli/src/console.cpp` | 重写 | 每条命令注册 trampoline（`func_w_context` + `Entry{handler,ctx}`），统一补 `OK`/`ERR …` 且恒返回 0；`add_verbs` + 自定义 `help`（无参列组、`help <组>` 打用法）；UART 分支在 `esp_console_new_repl_uart` 之后按 `RTK_CONSOLE_UART_TX_BUF` 重装带 TX 环形的驱动 |
| `components/cli/CMakeLists.txt` | 修改 | SRCS 追加 `src/reply.cpp` |
| `components/cli/Kconfig` | 修改 | `RTK_CONSOLE_UART_BAUD` 默认改为 `ESP_CONSOLE_UART_BAUDRATE`；新增 `RTK_CONSOLE_UART_TX_BUF`（默认 8192，0..32768） |
| `test/cli/test_reply.cpp` | 新建 | 10 个用例，`tmpfile()` + `dup2` 捕获 stdout，覆盖 fail 文本/dispatch 四个分支/usage 精确格式/kv 与 section 对齐 |
| `test/cli/CMakeLists.txt` | 修改 | 新增 `cli_reply` 静态库与 `test_reply` 测试 |
| `main/modules/_template/README.md` | 修改 | §7 补充：命令用 `cli::Verb` + `cli::dispatch`，失败 `return cli::fail(...)`，状态行由 trampoline 补 |
| `sdkconfig.defaults` | 修改（用户授权） | 控制台改 CUSTOM 变体 + UART0 + TX1/RX3 + 2 Mbaud，`RTK_CONSOLE_UART_BAUD=2000000`、`RTK_CONSOLE_UART_TX_BUF=8192` |
| `docs/frozen.sha256` | 修改（用户授权） | 更新 `sdkconfig.defaults` 的哈希行为 `869ee9065e5b70e5f7e29fd2fde0bd90847d306e32573a7a55e15772b2b0bd0c` |

未改动冻结头文件（`fw/trace.hpp`、`cli/console.hpp`、`fw/lock.hpp`、`fw/result.hpp` 均按契约实现）。

## 2. 执行的命令与结果

| 项目 | 命令 | 结果 |
| --- | --- | --- |
| Host：app_fw | `cmake -S test/app_fw -B builds/t1a-appfw && cmake --build … && ctest --test-dir …` | PASS（3/3：`test_app_fw_basics`、`test_fanout_ring`、`test_trace`） |
| Host：cli | `cmake -S test/cli -B builds/t1a-cli && … ctest` | PASS（2/2：`test_parse`、`test_reply`） |
| ASan/UBSan：app_fw | `builds/t1a-asan-appfw`（`-fsanitize=address,undefined -fno-omit-frame-pointer -g`） | PASS（3/3） |
| ASan/UBSan：cli | `builds/t1a-asan-cli` | PASS（2/2） |
| Host：既有套件 | `test/host`、`test/stream_demux`、`test/bt_reconnect`、`test/services`（`builds/t1a-*`） | PASS（7/7、4/4、1/1、2/2） |
| Host：main 模板 | `cmake -S tools/main_host_check -B builds/t1a-main_host_check` + ctest | PASS（30/30） |
| Host：T13 | `cmake -S tools/t13_host_check -B builds/t1a-t13_host_check` + ctest | PASS（29/29） |
| IDF 配置 | `IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1a reconfigure` | PASS（4 条 `component_validation.cmake:106` 的既有 CMake Warning，非本次引入） |
| IDF 构建 | `IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1a build` | PASS，0 warning / 0 error |
| 体积 | `idf.py -B builds/t1a size` + `idf_size.py --archives`（与 `builds/v-r7b` 对比） | 见 §3 |
| 堆/异常门禁 | `python3 -I tools/g7b_check/g7b_check.py --build builds/t1a` | PASS（51 个自有目标文件；堆白名单仍为 0 源文件；ELF 异常/RTTI 仅 6 个白名单符号） |
| 格式 | `clang-format --style=file --dry-run --Werror`（6 个改动源文件） | PASS（初次 4 处偏移，`clang-format -i` 修正后复检通过） |
| 冻结校验 | `sha256sum -c docs/frozen.sha256` | PASS（无 FAILED 行） |

首次 IDF 构建失败过一次：`components/cli/src/reply.cpp:88,155: error: invalid types 'va_list[int]' for array subscript`。原因是我为兼容 host 写的 `std::vprintf(fmt, &args[0])` 在 Xtensa 上非法（该目标 `va_list` 为数组/指针类型）；改为标准的 `std::vprintf(fmt, args)` 后两端同时通过。

## 3. 体积对比（`builds/v-r7b` → `builds/t1a`）

| 项 | v-r7b | t1a | 增量 |
| --- | --- | --- | --- |
| Flash Code (.text) | 415 900 | 416 972 | +1 072 |
| Flash Data (.rodata/.appdesc) | 118 848 | 119 328 | +480 |
| DRAM | 83 797 (67.26%) | 84 381 (67.73%) | +584 |
| IRAM | 82 779 (63.16%) | 82 791 (63.16%) | +12 |
| 镜像合计 | 662 228 | 663 792 | +1 564（+0.24%） |

解释（`idf_size.py --archives`）：`libcli.a` 由 1 142 B 增至 2 812 B（+1 670），其中代码 +1 085、其他 +585；`libmain.a`、`liblink_core.a` 完全不变；`libapp_fw.a` 仍为 127 B。也就是说 DRAM 的 +584 与 Flash 的增量全部来自 cli 的 trampoline/help/dispatch/回执代码与字符串常量，`fw::trace`/`TraceRing` 因当前没有任何调用者，静态库未拉入该目标文件，对固件体积贡献为 0（链接期死代码消除，符合预期）。`RTK_CONSOLE_UART_TX_BUF=8192` 是 `uart_driver_install` 的运行时堆分配，不占静态 DRAM，因此未反映在这里。

## 4. 与契约的偏离

1. **UART TX 环形缓冲的顺序**：任务卡写“先 `uart_param_config`/`uart_driver_install` 再 `esp_console_new_repl_stdio()`”。在 IDF v6.1 下不可行：`esp_console_new_repl_uart()` 创建期就调用 `uart_vfs_dev_port_init()`（`~/esp/esp-idf/components/console/esp_console_repl_chip.c:73`），其内部已 `uart_driver_install(ch, 256, 0, …)`，重复安装会打印 `UART driver already installed` 并返回 `ESP_FAIL`。改成先 `esp_console_new_repl_uart()`，随后 `uart_driver_delete` → `uart_param_config` → `uart_set_pin(-1×4)` → `uart_driver_install(ch, 256, CONFIG_RTK_CONSOLE_UART_TX_BUF, 0, nullptr, 0)`；`uart_vfs_dev_use_driver` 已生效，无需重复调用。`TX_BUF == 0` 时完全不动驱动，保留 IDF 同步阻塞行为。无竞态：REPL 任务体首行是 `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`（`~/esp/esp-idf/components/console/esp_console_common.c:23`），直到 `esp_console_start_repl()` 才会读串口。
2. **自定义 `help`**：IDF 的 `esp_console_common_init()` 已注册自带 help；本工程用同名 `esp_console_cmd_register` 覆盖（`~/esp/esp-idf/components/console/commands.c:114` 对同名条目复用 item），从而支持“每组一行摘要”。旧命令表未改，因此旧命令现在会多打印一行 `OK`/`ERR …`，属本步预期。
3. **内部头位置**：`reply_state.hpp` 放在 `components/cli/src/` 而非 `private_include/`（AGENTS §4 允许私有头放 `src/`）。放 `private_include/` 时 lens/LSP 无法解析该 include 路径，改放同目录后 host 与 IDF 两端都能直接编译。
4. **`sdkconfig.defaults` 改用 CUSTOM 控制台变体**：`~/esp/esp-idf/components/esp_stdio/Kconfig:150` 的 `config ESP_CONSOLE_UART_BAUDRATE` 只在 `ESP_CONSOLE_UART_CUSTOM` 下有 prompt。实测把 sdkconfig 里的值手工改成 2000000 后 `reconfigure` 会被压回 115200。因此改为 `ESP_CONSOLE_UART_DEFAULT=n` + `CUSTOM=y` + `CUSTOM_NUM_0=y` + `TX_GPIO=1`/`RX_GPIO=3`（ESP32 默认引脚）+ `BAUDRATE=2000000`，`reconfigure` 后四个 RTK/IDF 控制台项稳定为 2 Mbaud / 8192 B。
5. **`sdkconfig.defaults` 与 `docs/frozen.sha256` 的修改**来自用户在开工前的明确授权（同步更新哈希，使校验保持全 PASS）；`sdkconfig` 本身被 `.gitignore` 忽略，仅作为本机构建输入改了同一组值。

## 5. 未解决问题

- 硬件行为（2 Mbaud 实际通信、`help` 输出、长输出不阻塞）未验证：按授权边界不烧录、不连串口，需在硬件阶段确认。
- 权威格式门禁（astyle_py 3.4.7 + `astyle-rules.yml`）需联网下载，按 AGENTS §5 由验收方执行；本次只跑了 `clang-format` 近似规则。
- `fw::trace` 尚无调用者，因此本轮只有单元测试证据（host + ASan/UBSan），固件内无实际 trace 流量；接线留给后续链路/SPP 任务。

T1a DONE
