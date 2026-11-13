# K2 复审 + 收尾（deepseek）

用户（2026-11-13）指派：对未提交的 K2 改动（`git diff 5fa2861` + 未跟踪 test/services/test_link_credit.cpp、tools/t13_host_check/k2_tx_check.c）做独立复审，并完成以下未做项。
依据：docs/tasks/K2-spp-nonblocking-tx.md、docs/adr/ADR-0011-data-path-memory.md、docs/reports/K2.md、AGENTS.md。

## 1. 独立复审
对照卡与 ADR-0011 §4 逐条核对正确性（并发/临界区、在途计数与 generation、匿名/短写失败处理、回复预留与反压、断开清理、计数回绕）。
缺陷直接修复并补回归测试；结论写入报告“复审发现”表（文件:行、问题、修复/理由）。

## 2. G5b 全仓 clang-tidy 22 条既有失败 → 清零
`python3 -I tools/tidy_check/tidy_check.py --host-build builds/host --idf-build builds/k2`（构建目录一律 builds/<name>/，**禁止在根目录生成 build/**）。
- 6 条 demux_frame.cpp：修 builds/host（或 tidy_check 的编译库选择）缺 stream_demux 头路径问题，属工具/CMake 配置修复。
- 16 条冻结头（gnss/rtos/stream_demux）+ main/app_config.hpp:24：用户授权本次修改这些冻结头，**仅限消除诊断、不改 ABI/语义**（命名常量、补 rule-of-5 声明等）；只更新 docs/frozen.sha256 中被改文件的条目，`sha256sum -c` 全 OK。确属误报的用 NOLINT + 理由。

## 3. astyle 权威格式门禁
用户授权本次可安装：`uv tool install astyle_py==3.4.7`（或 `uv run --with astyle_py==3.4.7`），规则 `~/esp/esp-idf/tools/ci/astyle-rules.yml`，对 K2 改动及第 2 步改动的文件运行；只格式化这些文件。不要用系统 pip。

## 4. 硬件实测（HIL）
用户授权烧录 RTK 板 /dev/ttyACM0（不要碰 /dev/ttyUSB0）。
- 烧录：`. ~/esp/esp-idf/export.sh; idf.py -B builds/k2 -p /dev/ttyACM0 flash`。
- 控制台/日志：用 `gtui-terms --pipe -b 2000000 /dev/ttyACM0`（或 `--headless`，`--log <path>`），打开串口会复位板子，等约 8s 启动；控制台命令 `sys heap`、`sys tasks`、`status sys`、`debug list`。
- 压测（主机 BlueZ，RTK MAC F8:B3:B7:25:D8:B2）：`python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode all`；另单独跑 win16、up256、下行吞吐。
- 对比基线（K2 前，9a7d7c5）：rtt..win8 0 丢 ~300 rep/s、下行 25 KiB/s；win16 丢 8 条回复；up256 丢 8 帧；heap min 98.4KB。K2 验收目标：win16 0 丢；下行 ≥37.5 KiB/s（若未达，给出瓶颈证据，不要擅自改设计）；up256 可能需 K3 解决，如实记录。
- 记录压测后 heap free/min/largest、各任务栈 HWM、tx 统计（anon_write_fail/stray_write 等）。
- 压测脚本运行时不要同时占用串口；先压测再开串口读统计（注意开串口会复位，统计需在不复位情况下读：用 `--no-reset`）。

## 5. 收尾
重跑全套门禁（idf build 0 warning、size、g7b、9 套 host 普通+ASan/UBSan、frozen、G5b、astyle）。
更新 docs/reports/K2.md：增加“复审（deepseek）”“G5b 清零”“astyle”“HIL 实测”各节，PASS/FAIL/NOT_RUN 与实测数字。
不 commit。完成后停下。
