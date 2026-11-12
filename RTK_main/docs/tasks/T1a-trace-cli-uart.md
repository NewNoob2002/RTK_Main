# T1a 追踪环 + 控制台回执/子命令 + 调试串口（ADR-0009）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0009-trace-console.md`、`docs/CODING-STYLE.md`，
再读冻结头：`components/app_fw/include/fw/trace.hpp`、`components/cli/include/cli/console.hpp`，参照 `components/app_fw/src/fanout_ring.cpp`、`test/app_fw/test_fanout_ring.cpp`、`components/cli/src/console.cpp`、`test/cli/`。

## 要做

1. **`fw::trace` 实现**：`components/app_fw/src/trace.cpp`（加进 `components/app_fw/CMakeLists.txt` 的 `_app_fw_srcs`）。
   - 掩码用 `std::atomic<uint32_t>`（relaxed），sink/ctx/clock 安装一次（启动期单线程，之后只读；`trace()` 读 sink 用 acquire，install 用 release）。
   - `TraceRing`：push/pop/take_dropped/stats/depth 全部在 `LockGuard` 内，临界区只做索引运算与一次 16 B 拷贝。满了丢新事件，`dropped` 累加；`take_dropped()` 返回自上次调用以来的增量。
   - 不用堆、不用异常；纯组件告警集不得放宽。
2. **`test/app_fw/test_trace.cpp`**（加入现有 CMake 循环）：掩码开关与越界位、未安装 sink 不调用、重复 install、clock 被使用、环满丢新且顺序保持、take_dropped 增量语义、depth/max_depth、wrap-around 多轮、init 参数校验。普通 + ASan/UBSan 两种构建都要过。
3. **`cli` 回执与子命令**：
   - 把 `fail`、`dispatch`、`print_usage`、`print_section`、`print_kv` 放进新的 `components/cli/src/reply.cpp`（只用 `<cstdio>`/`<cstdarg>`，不含 IDF 头，以便 host 测试）；回执状态（本次命令是否已 fail）用 console 任务私有的文件静态变量，并给 trampoline 提供内部的 reset/查询函数（放 `components/cli/private_include/`，不冻结）。
   - `console.cpp` 的 `start()`：每条命令注册时 `func_w_context` 指向 trampoline，`context` 指向该 `Cmd` 表项；trampoline 调 handler 后按 ADR-0009 §2.5 打印 `OK`/`ERR …`，**总是返回 0**。
   - `add_verbs()`：登记组的动词表与一行摘要。自定义 `help`（不再用 IDF 内置 help）：无参 → 每组一行 `组名  摘要`（按登记顺序，组名左对齐）；`help <组>` → `print_usage`；未登记动词表的组显示其 `Cmd::help`。
   - `print_kv` 键宽 14 字符左对齐。
   - 现有命令表**本步不改**（T2 再按组重排），但因 trampoline 生效，旧命令执行后也会多一行 `OK`/`ERR`，这是预期。
4. **`test/cli/test_reply.cpp`**：用 host 编译 `reply.cpp`（把 stdout 重定向到 `tmpfile()`/`open_memstream` 抓输出），覆盖 fail 文本、dispatch 命中/缺动词/未知动词/default_verb、print_usage 格式、print_kv 对齐。
5. **调试串口（ADR-0009 §2.6）**：
   - `components/cli/Kconfig` 新增 `RTK_CONSOLE_UART_TX_BUF`（int，默认 8192，范围 0..32768，0=同步阻塞旧行为）；`RTK_CONSOLE_UART_BAUD` 默认改为 `ESP_CONSOLE_UART_BAUDRATE`（若 Kconfig 不能引用则默认 2000000 并在 help 写明须与之一致）。
   - `sdkconfig.defaults` 与 `sdkconfig`：`CONFIG_ESP_CONSOLE_UART_BAUDRATE=2000000`、`CONFIG_RTK_CONSOLE_UART_BAUD=2000000`。用 `idf.py -B builds/t1a reconfigure` 或 menuconfig 等价方式生成，确认 `sdkconfig` 中只出现预期的差异。
   - `start()` 的 UART 分支：自行 `uart_param_config` + `uart_driver_install(ch, 256, TX_BUF, 0, nullptr, 0)` + `uart_vfs_dev_use_driver(ch)` + `uart_vfs_dev_port_set_rx_line_endings(ch, ESP_LINE_ENDINGS_CR)` / `set_tx_line_endings(CRLF)`，再 `esp_console_new_repl_stdio()`。对照 IDF `components/console/esp_console_repl_chip.c` 的 `esp_console_new_repl_uart_legacy` 与 `components/esp_driver_uart/src/uart_vfs.c` 的 `uart_vfs_dev_port_init`，保证行为等价（linenoise、换行、阻塞读）；任何失败按逆序清理并返回错误码。在代码注释中说明“经典 ESP32 无 UHCI/UART DMA，用 TX 环形缓冲 + 中断搬运代替”。
6. 同步：`docs/CODING-STYLE.md` 无需改；`main/modules/_template/README.md` 增加一句“命令用 cli::Verb + dispatch，失败用 cli::fail”。

## 门槛（写入 `docs/reports/T1a.md`，逐项 PASS/FAIL 附命令）

1. `IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1a build` 0 warning（项目内）；`idf.py -B builds/t1a size` 与 `builds/v-r7b` 对比，列出 DRAM/IRAM/Flash 差异并解释（TX 环在堆上，不应进 .bss）。
2. host：`test/app_fw`（含新 test_trace）、`test/cli`（含新 test_reply）普通 + ASan/UBSan；`test/host`、`test/stream_demux`、`test/bt_reconnect`、`test/services`、`tools/main_host_check`（30，普通 + ASan）、`tools/t13_host_check`（29）全部通过，构建目录 `builds/t1a-*`。
3. `python3 -I tools/g7b_check/g7b_check.py --build builds/t1a` PASS。
4. clang-format `--dry-run --Werror` 覆盖所有改动/新增的 C/C++ 文件。
5. `sha256sum -c docs/frozen.sha256` 全 OK（冻结头一律不改；发现冻结接口无法实现时停下写 BLOCKED，说明原因与建议）。

报告最后一行 `T1a DONE` 或 `T1a BLOCKED: <原因>`。

## 禁止

修改冻结文件（`AGENTS.md`、`docs/**`（`docs/reports/` 与本卡指定的 README 除外）、`components/*/include/**`、`test/host/**`、`reference/**`）；烧录、连接串口、联网安装、git commit。只用 `builds/t1a*`。
