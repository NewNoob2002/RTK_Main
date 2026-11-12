# R5 拆分 rtk_bt_spp.c（ADR-0008 §5 R5）

执行方：pi + workbuddy/deepseek-v4.1-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`、
`components/rtk_bt/` 全部文件、`tools/t13_host_check/`（CMakeLists、shim、t13_host_check.c）。

## 目标
`components/rtk_bt/rtk_bt_spp.c`（1644 行）按职责拆成若干文件，**行为与对外接口完全不变**，读的人能按文件名找到代码。建议划分（可按实际代码调整，报告里说明理由）：

| 文件 | 内容 |
| --- | --- |
| `rtk_bt_spp_priv.h`（放 `components/rtk_bt/private_include/`） | 内部类型、共享静态状态的 extern 声明、内部函数原型 |
| `rtk_bt_spp_init.c` | 控制器/Bluedroid/SPP 初始化与启动等待、回滚 |
| `rtk_bt_spp_session.c` | 会话槽 find/acquire/release/alloc/free、拥塞标志 |
| `rtk_bt_spp_rx.c` | 数据入 StreamBuffer、`rtk_bt_spp_read` 等读路径 |
| `rtk_bt_spp_tx.c` | 发送任务、ctrl 队列、`write_chunk`/`write`、WRITE_EVT 处理 |
| `rtk_bt_spp_gap.c` | GAP/SPP 回调分派、可见性、PIN/CFM 应答 |

规则：
- 原来的文件静态变量改为跨文件共享时，统一放进一个内部状态结构体（例如 `rtk_bt_spp_state_t s_spp`），定义在一个 .c 里，`priv.h` 中 `extern`；不要散成多个 extern 全局。只在单个文件内使用的仍保持 `static`。
- 内部函数跨文件使用时去掉 `static` 并加 `rtk_bt_spp_` 前缀原型进 `priv.h`；只在一个文件内用的保持 `static`。
- 逻辑、锁的范围、日志文本、错误码一律不变；只允许为拆分必需的改动（声明位置、static 修饰）。
- 每个新文件第一行写“这个文件做什么”，第二行起写线程/调用约束（谁调用、是否在 BTC 回调线程、是否持锁）；不写 `Generated-by/人工负责人`（ADR-0008 §7）。
- 每个文件 ≤ 500 行。
- 公开头 `include/rtk_bt_spp.h`、`include/rtk/bt/spp_transport.hpp` 不得修改。

同步修改：`components/rtk_bt/CMakeLists.txt`（SRCS、PRIV_INCLUDE_DIRS）、`tools/t13_host_check/CMakeLists.txt`（`rtk_bt_spp_driver` 源列表与 include）。

## 自检（写入 `docs/reports/R5.md`，逐项 PASS/FAIL）
1. `cmake -S tools/t13_host_check -B builds/r5-t13 && cmake --build builds/r5-t13 && ctest --test-dir builds/r5-t13 --output-on-failure` 全部通过（与拆分前用例数一致；先在拆分前跑一次记录基线）。
2. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B builds/r5 build`：0 warning；报告拆分前后 `idf.py -B builds/r5 size` 的 DRAM/IRAM/Flash 差异（预期为 0 或极小，有差异需解释）。
3. 等价性证据：用脚本把拆分前（`git show HEAD:RTK_main/components/rtk_bt/rtk_bt_spp.c`）每个函数体与拆分后同名函数体做规范化（去空白）比较，列出所有不同的函数及原因；日志字符串集合前后一致。
4. clang-format 检查新文件（`--dry-run --Werror`）；`clang-tidy --config-file=.clang-tidy-c` 对新文件无新增告警（对比拆分前）。
5. 每个文件行数表。

## 并行约束
R0 同时运行，它修改 `docs/CODING-STYLE.md`、`AGENTS.md`、`docs/adr/ADR-0002*`、`docs/acceptance.md`、`tools/g7b_check/*`。你不得修改这些文件；只能用 `builds/r5*`，不得使用 `build/`。

## 禁止
修改公开头与其他组件；烧录、连接串口、联网安装、git commit。`docs/frozen.sha256` 由验收方重算。

完成后在报告最后一行写 `R5 DONE` 或 `R5 BLOCKED: 原因`。
