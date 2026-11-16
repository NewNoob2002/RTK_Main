# K5 实施下达（2026-11-13）

用户已批准实施 K5（docs/tasks/K5-boot-memory.md），基线提交 17aeeab（K4）。卡中“本轮只修订本卡文档 / 实施与冻结契约定稿另行下达”由本文件取代：现在实施，范围以卡 §2 文件表为准。

- 冻结授权：
  - 新增 `components/rtos/include/rtos/boot_alloc.hpp`、`boot_task.hpp`、`boot_stream_buffer.hpp`（AGENTS 允许的新增 RAII 包装），完成后加入 `docs/frozen.sha256`；既有 rtos 头（StaticTask/StaticStreamBuffer/Executor 等）不得修改。
  - `components/spp_server/include/spp_server.h` **不预授权**：优先把 storage view 放在 `private_include/spp_server_priv.h` 或新增非冻结头中实现；确需改 spp_server.h 时停下，在报告中给出精确结构/函数签名、对齐、长度、初始化时机与回调终结前寿命的说明，由审查方决定。
  - ADR-0009/CODING-STYLE 若与 sys mem/启动期堆描述冲突，在报告中列出，不自行改。其它冻结头、AGENTS、sdkconfig.defaults、partitions、test/host、reference 不得改。
  - 只更新获批文件在 `docs/frozen.sha256` 的条目，最终 `sha256sum -c docs/frozen.sha256` 全 OK；不全表重算。
- 格式：clang-format 是唯一格式门禁（AGENTS.md:94），不要运行 astyle。strict warnings 全开，0 警告。
- 基线（K4 实测，17aeeab）：静态 DRAM 119,207 / 124,580 B；负载下 heap min 88.8 KB；HIL win16×3000 下行 36.8 KiB/s（已知 −4%，见 docs/reports/K4.md §5.4），win32 41.3，upwin 3072B 95.9 KiB/s，全部 0 丢；GNSS 订阅 HIL `tools/hil/spp_gnss_sub.py` 15/15。K5 不得回退；负载下内部 8bit heap min ≥ 81920、最大块 ≥ 32768。
- 内存账以 `build/rtk_main.map` / `idf.py size` 实测为准，卡 §5 的估算数字不得硬编码为测试真值；.data→.bss 不算释放运行时 RAM。
- 若改 HIL 脚本须沿用 `tools/hil/spp_stress.py` 的 `WAIT_SLICE_S` 分片等待，输出 `flush=True`。
- 构建：idf 主工程用 `build/`；其它构建只放 `builds/<name>/`（如 `builds/k5-main_host_check`）。
- 不 commit、不连串口/烧录（HIL 由审查方执行），不安装/联网。完成后写 `docs/reports/K5.md`（逐项 PASS/FAIL/NOT_RUN/BLOCKED，含 HIL 计划的可执行命令与 `sys mem` 期望输出）并停下。
