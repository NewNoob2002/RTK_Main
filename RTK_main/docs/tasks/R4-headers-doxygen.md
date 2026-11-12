# R4 文件头清理与公开 API doxygen（ADR-0008 §5 R4）

执行方：pi + deepseek。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`、`docs/CODING-STYLE.md`（重点 §2 文件头、§6 注释、§7 堆档位）。

## 范围
仓库自有源码：`components/**`、`main/**`、`test/**`、`tools/**`（含 .py）、`test_apps/rtk_spp_peer/**`。不碰 `reference/**`、`docs/adr/**`、`docs/reports/**`、`builds/**`、第三方代码。

## 要做
1. **文件头**（约 124 个文件含 `Generated-by` / `人工负责人` 行）：
   - 删除 `Generated-by: ...`、`人工负责人: ...` 等工具/模型/负责人行（来源已改为写在提交信息里，ADR-0008 §7）。
   - 第一行改为一句中文说明“这个文件做什么”；第二行起按需写：谁调用、能否阻塞、是否 ISR 安全、线程/核约束。头文件和实现文件都要有，内容不必重复（实现文件可简短）。
   - 使用堆的文件保留/补齐 §7 的档位、容量、失败策略说明，与 `tools/g7b_check/heap_allowlist.txt` 一致。
   - 不改许可证文本（如有）。
2. **公开 API doxygen**（`components/*/include/**` 与 `main/**/*.hpp` 中被其他文件使用的声明）：
   - 每个公开类/结构体：`/** 一句话用途 + 所有权/生命周期 */`。
   - 每个公开函数：`@brief`（可省略写成首句）、需要时 `@param`/`@return`/`@retval`，并写清：单位、可否阻塞/超时、调用上下文（任务/ISR/回调）、失败时状态是否改变。已有等价注释的就整理成统一格式，不要重复堆叠。
   - 常量组：标单位；同组放一起。
3. **实现内注释**：只保留解释“为什么”的注释；删除复述代码的注释、过时注释（提到已删除的文件、旧命名空间、旧阶段号如 “J2b 临时” 但已不成立的）。硬件/协议栈 workaround 注释必须保留并带来源（如 IDF 源文件:行、实测报告名）。
4. 注释语言：中文 + 必要英文术语（AGENTS/CODING-STYLE 约定）。

## 不做
任何代码行为、符号名、签名、include 结构、格式化以外的代码改动。若发现 bug 或不一致，写进报告“发现但未改”一节，不要修。

## 冻结文件
`components/*/include/**` 注释改动会改变哈希：完成后用
`sha256sum $(awk '{print $2}' docs/frozen.sha256) > docs/frozen.sha256` 重算并 `sha256sum -c` 全 OK（条目数不变）。

## 自检（写入 `docs/reports/R4.md`，逐项 PASS/FAIL，附命令）
1. `. ~/esp/esp-idf/export.sh >/dev/null && IDF_COMPONENT_MANAGER=0 idf.py -B builds/r4 build` 0 warning；`idf.py -B builds/r4 size` 与改前 `builds/r4-base` 相比 DRAM/Flash **完全一致**（只改注释；`__FILE__`/断言字符串除外，如有差异必须解释）。
2. `idf.py -C test_apps/rtk_spp_peer -B builds/r4-peer build` 0 warning。
3. 主机测试全部通过、用例数不变：main_host_check（普通 + ASan/UBSan 29/29）、t13 29/29、test/host 7、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/rtk_console（构建目录 `builds/r4-*`）。
4. `rg -n "Generated-by|人工负责人" components main test tools test_apps` 为空。
5. 每个改动文件第一行是一句中文说明（脚本检查并列出清单）。
6. 代码 token 不变：对每个改动的 C/C++ 文件，去掉注释后与 HEAD 版本比较一致（可用 `gcc -fpreprocessed -dD -E` 或自写去注释脚本），列出结果。
7. clang-format `--dry-run --Werror` 覆盖改动文件；`git diff --check` 为空；G7b PASS；冻结清单 OK。

报告最后一行写 `R4 DONE` 或 `R4 BLOCKED: <原因>`。

## 禁止
改代码逻辑；改 `reference/**`、历史报告/ADR；烧录、串口、联网安装、git commit。只用 `builds/r4*`。
