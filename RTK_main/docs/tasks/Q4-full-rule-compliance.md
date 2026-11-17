# Q4：按技能 v0.1 逐条核对全工程代码合规（全量，不是增量）

状态：ISSUED 2026-11-17，基线 `feat/gnss` HEAD（见 `git log -1 -- docs/tasks/Q4-full-rule-compliance.md`）。
实施：omp + workbuddy/deepseek-v4.1-flash（QA 角色，只读审计）；规划/验收：Claude。

## 0 为什么还要再查一次

用户发现（2026-11-17）：技能 `DOC-001` 与项目 `docs/CODING-STYLE.md` §6.2 都要求每个函数有 Doxygen，
但工程大部分函数没有（`tools/doc_check --report`：1293 个函数只有 315 个合格，24%），而 Q1 在 §10 把 DOC-001 判为「覆盖」。
原因：Q1 的 B 类是「项目文档有没有对应规则」，没有用**全量代码证据**核实规则是否真的被遵守；A 类也只抽查了部分模式。
本卡补上这一步：对每条适用规则给出**全量代码**的合规结论和数字。

技能：`/home/gtc/.claude/skills/embedded-engineering/`（v0.1，源仓库 `/home/gtc/Desktop/workspace/Tools/Skills` 提交 `3254fb6`）。
Q1 报告 `docs/reports/Q1.md`、Q2/Q3 卡与 `docs/CODING-STYLE.md` §10 是起点，不是结论：§10 里每个「覆盖」都要重新用代码证据验证。

## 1 先读

项目：`AGENTS.md`、`docs/CODING-STYLE.md`（全文，尤其 §6.2、§10）、`docs/acceptance.md`、`docs/adr/*.md`、`docs/reports/Q1.md`。
技能：`SKILL.md`、`profiles/esp-idf.yaml`、`references/profiles.md`、`standards/rules.yaml`（135 条，筛 `profiles` 含 `esp-idf` 或 `all` 且 `when` 成立）、
各规则 `ref` 指向的 reference（按需读，不要全读）、`references/cpp/readable-cpp.md`（style_gate 用法）。
仓库有 `.codegraph/`：定位代码先用 `codegraph explore "<符号或问题>"`。

## 2 判定方法（关键）

对每条适用规则（含项目自己的 AGENTS/CODING-STYLE/ADR 中可机械或半机械核对的条款）给出：

| 字段 | 内容 |
| --- | --- |
| 规则 | 技能 Rule ID 或项目条款（如 `CODING-STYLE §6.2`） |
| 项目口径 | 项目规则是否覆盖/有意偏离（引用 §10 或文档位置） |
| 检查方法 | 具体命令/脚本/codegraph 查询；纯人工判断的写“评审抽查 N 处” |
| 全量结果 | 被检对象总数、违规数、违规率；按组件分列（`components/*`、`main`） |
| 结论 | **遵守**（违规 0）/ **部分遵守**（列数量）/ **未遵守** / **不适用**（理由）/ **NOT_RUN/BLOCKED**（原因） |
| 证据 | 违规 ≤10 处列全部 `文件:行号`；>10 处给计数 + 前 10 处 + 完整清单文件路径 `builds/q4-*/...` |

规则优先级照技能 SKILL.md §1：项目已登记理由的有意偏离不算违规，但要按**项目自己的规则**检查代码（例：注释语言按项目用中文，不按技能默认英文）。
只有「规则写了、代码没做到」或「技能强制、项目没写也没做」才计入违规。不得以“存量/历史代码”为由跳过——存量也要计数，只在结论里注明是存量。

范围：`main/`、`components/*`（C 与 C++）、`test_apps/` 自有代码；`test/` 只核对测试相关规则（TEST-*）。
不审 `reference/`、`builds/`、`build/`、IDF/厂商代码、生成文件（`test/stream_demux_dev/h2/H2Vectors.h` 等，按 CODING-STYLE §9）。

## 3 必须覆盖的检查（至少这些，规则表其余条目也要逐条过）

1. **Doxygen（DOC-001 / CODING-STYLE §6.2）**：`python3 -I tools/doc_check/doc_check.py --report` 与 `--list <组件>` 全量；
   再用技能 `style_gate.py`（`doxygen:true`）交叉验证——编译库用 host 构建的真实 `compile_commands.json`
   （如 `builds/` 下 test/gnss、test/main_host_check、test/services 等 host 构建；不够就 `cmake -B builds/q4-host-<x> -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 重新配置），
   不要再用 Xtensa 的编译库（Q1 因 LLVM 不认 xtensa 标志而 BLOCKED）。两个工具结论不一致时列出差异原因。
   另抽查 30 个“合格”函数的标签完整性（`@param` 每个参数、`@retval` 每种结果、`@note` 线程/ISR 约束）——doc_check 只看存在性。
2. **命名**（CODING-STYLE 命名节 / 技能命名规则）：style_gate 命名检查全量。
3. **文件头**（CODING-STYLE §2）：全部自有 `.c/.cpp/.h/.hpp`。
4. **格式**：`clang-format --style=file --dry-run --Werror` 全部自有文件。
5. **clang-tidy**：`tools/tidy_check` 全量（Q3 后应为 0，复核）。
6. **架构/依赖**：`arch_lint.py`（复用或更新 `builds/q1-arch/layers.json`，对照 AGENTS §3）、各组件 `CMakeLists.txt` REQUIRES 与实际 `#include`。
7. **运行时成本**：`cpp_cost.py check <elf> --tier T2`、`tools/g7b_check`（需要 IDF 构建：`idf.py -B builds/q4 build` 一次）。
8. **ISR/RTOS/并发**（ISR-*、RTOS-*、ESP-001…、CPP-008 原子）：ISR 内调用、临界区内调用、任务创建是否静态+绑核、栈/优先级常量位置、`volatile` 误用、原子 lock-free 断言——codegraph/grep 全量列举后逐个判定。
9. **错误处理**（`esp_err_t`/`fw::Status` 丢弃、`(void)` 无理由、`ESP_ERROR_CHECK` 在非启动路径）、**堆/异常/RTTI**、**魔数**、`static_cast` 范围注释（CODING-STYLE §4.5 收窄后的口径）。
10. **测试规则**（TEST-*）：每个自有组件是否有 host 测试；TEST-002 nvs_store 已知缺口复核。

## 4 约束

- **只读**：不改任何代码、文档、配置；不 git add/commit/push/stash/reset；不安装软件、不联网。
- 只允许写：`docs/reports/Q4.md` 与 `builds/q4-*/` 下的产物（清单、日志、manifest、layers.json）。
- 不接触硬件。工作区他人改动只读不还原。无法核实写 `UNKNOWN`。

## 5 报告 `docs/reports/Q4.md`

1. 总表：每条规则一行（§2 的字段），按 结论 排序（未遵守 → 部分遵守 → 遵守 → 不适用 → NOT_RUN）。
2. 与 Q1/§10 的差异：列出 §10 标「覆盖」但全量证据不支持的条目（DOC-001 已知一条），以及 Q1 漏掉的违规。
3. 违规按组件汇总表（组件 × 规则 的违规计数）。
4. 修复建议：按工作量分批（每批可作为一张任务卡），给出估计改动函数/文件数，标出可机械修复的项。
5. 实际执行的命令与关键输出行（长日志放 `builds/q4-*/`）；未覆盖范围与原因。

完成后在终端输出一行 `Q4 DONE` 并停止。
