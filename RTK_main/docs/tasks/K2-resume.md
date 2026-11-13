# K2 续做交接（前一 agent 因额度中断）

任务卡：docs/tasks/K2-spp-nonblocking-tx.md；依据 docs/adr/ADR-0011-data-path-memory.md（用户已接受）。基线提交 5fa2861（K1）。
前一 agent（gpt）已在工作区完成**未提交**的大部分实现：spp_server 非阻塞多在途 TX、链路回复预留/反压、删除 main/modules/bt_link/async_tx.*、
trace 码、system_cmds/ debug 输出、test/services/test_link_credit.cpp、tools/t13_host_check/k2_tx_check.c 等（`git status` / `git diff 5fa2861` 查看）。
它的进度表：①核对契约 ✓ ②实现 ✓ ③补齐故障/容量/公平性/兼容回归（进行中）④全套门禁 + 报告（未做）。

你要做：
1. 先通读 `git diff 5fa2861` 与任务卡，核对实现是否符合卡和 ADR §4（每会话 reply 2048B、仅 PHONE stream 4096B、全局/每会话在途 ≤4 并 static_assert 引用 btc_spp.c:37 SLOT_TX_QUEUE_SIZE 10、匿名 WRITE 失败计 anon_write_fail 并关闭有在途会话、stray_write、dispatch 前预留 280B）。发现缺陷就修。
2. 补完卡中要求的 host 故障/容量/公平性/兼容用例。
3. 冻结授权（已由用户批准）：spp_server.h、spp/spp_transport.hpp、core/transport.hpp、core/link_service.hpp、core/trace_codes.hpp（仅追加）及 ADR-0007/0009 相关段落；只更新 docs/frozen.sha256 中这些文件的条目，最后 `sha256sum -c docs/frozen.sha256` 必须全 OK（当前 5 条 FAILED 正是因为尚未更新）。
4. 门禁：idf.py build 0 warning、idf.py size、g7b、全部 host 套件（含 ASan/UBSan）、你改动文件 clang-tidy 0 findings（G5b 既有 22 条与本卡无关，只记录）。
5. 写 docs/reports/K2.md（PASS/FAIL/NOT_RUN、sizeof/内存、命令）。不 commit、不连串口/烧录（HIL 由审查方做）。完成后停下。
