# Q1：按最新 embedded-engineering 技能对全工程做规范/架构符合性检查，并同步文档

状态：ISSUED 2026-11-17，基线 `feat/gnss` HEAD（见 `git log -1 -- docs/tasks/Q1-skill-conformance-audit.md`）。
实施：omp + workbuddy/deepseek-v4.1-flash（QA/Architect 审查角色）；规划/验收：Claude。

## 0 目标

用户要求（2026-11-17）：用**最新版** embedded-engineering 技能检查当前工程有无不匹配之处（编码规范/风格、系统架构等），
并同步更新相关文档。本卡分两阶段：A 只读审计；B 只改文档。**不改任何代码、构建脚本、测试、门禁配置。**

技能位置：`/home/gtc/.claude/skills/embedded-engineering/`（git `5cae6d9`，2026-11-17；源仓库 `/home/gtc/Desktop/workspace/Tools/Skills`）。
重点关注技能近期变更：`git -C /home/gtc/Desktop/workspace/Tools/Skills log -p 08ca799..5cae6d9 -- skills/embedded-engineering/standards skills/embedded-engineering/references/cpp skills/embedded-engineering/references/platforms/esp-idf.md skills/embedded-engineering/profiles/esp-idf.yaml skills/embedded-engineering/references/verification/review-checklist.md`
（尤其 f3aab8b、5cae6d9 对 rules.yaml、cpp/coding.md、cpp/features.md、review-checklist 的改动）。

## 1 先读

项目：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/acceptance.md`、`docs/adr/*.md`（同主题新 ADR 优先）。
技能（只读需要的）：`SKILL.md`、`profiles/esp-idf.yaml`、`references/profiles.md`（冲突裁决）、`standards/rules.yaml`
（筛选 `profiles` 含 `esp-idf` 或 `all` 且 `when` 成立的条目）、`references/platforms/esp-idf.md`、`references/cpp/coding.md`、
`references/cpp/features.md`、`references/cpp/heap-guard.md`、`references/rtos/mcu-isr-rtos.md`、
`references/architecture/state-protocol-fault.md`、`references/architecture/mcu-architecture.md`、
`references/verification/review-checklist.md`、`references/workflows/architecture-review.md`（W02）、`references/workflows/code-review.md`（W05）。

仓库有 `.codegraph/`：定位代码先用 `codegraph explore "<符号或问题>"`，再对照源码。

## 2 规则优先级（按技能 SKILL.md §1，不得颠倒）

运行时/授权限制 → 项目显式规则（`AGENTS.md`、`docs/CODING-STYLE.md`、`.clang-format`、ADR）→ 官方生态规范 → BARR-C → 分层架构 → 技能默认值。
因此：项目已**明确**作出且有理由的选择（如：格式只用 clang-format 不用 astyle；注释用中文；C++ T2 等级；Doxygen 用 `@brief/@param/@retval` 中文正文）
**不是违规**，只需在 B 类中确认偏离已登记；项目文档**没写**而技能有强制规则的，才是“缺口”。

## 3 阶段 A：审计（只读）

范围：`main/`、`components/*`（含 C 文件）、`test/`、`test_apps/`、`tools/`（门禁工具）、`CMakeLists.txt`、`cmake/`、`main/Kconfig.projbuild`、
`sdkconfig.defaults`、`partitions.csv`。不审 `reference/`、`builds/`、`build/`、IDF/厂商代码。

发现分三类，每条必须给：ID（Q1-A01…/Q1-B01…/Q1-C01…）、严重度（必须修复/建议/信息）、`文件:行号`、技能 Rule ID（如 `ESP-004`、`CPP-010`、`ISR-002`）或项目条款（`AGENTS.md §2`、`CODING-STYLE §4.4`、`ADR-0012 §2.8`）、证据（原文片段）、建议动作。

- **A 类：代码不符合项目自己的规则**（AGENTS/CODING-STYLE/ADR）。例：`ESP_LOG*` 残留、`(void)` 丢弃 `esp_err_t`、未登记的堆分配、任务未静态创建/未绑核或无理由注释、`static_cast` 无范围注释、非 `final` 叶子类无 ADR 依据、公共函数缺线程/ISR 安全说明、文件头不符合 CODING-STYLE §2、命名违规、组件依赖越界（对照 AGENTS §3 表与各组件 `CMakeLists.txt` 的 `REQUIRES`/`PRIV_REQUIRES` 及实际 `#include`）。只列有证据的，不要按文件凑数；同一模式多处出现合并成一条并列出全部位置（>10 处给计数+前 10 处）。
- **B 类：项目规则 vs 最新技能规则的差异**。逐条过 rules.yaml 中适用于 esp-idf/all 的规则，判断：项目已覆盖 / 项目有意偏离（是否已在文档写明理由）/ 项目缺口。每条给出建议：采纳（写进哪份文档哪一节）/ 保持偏离（补登记理由）/ 不适用（理由）。
- **C 类：文档与代码漂移**。ADR/AGENTS/CODING-STYLE/acceptance 中描述的组件名、文件路径、函数名、命令、门禁、目录、依赖表、构建选项与当前代码不一致之处（例：旧组件名 `rtk_proto`/`rtk_bt`、已改名文件、已不存在的命令或开关、组件依赖表缺项）。以当前代码为准核实后才列。

机械门禁（能跑就跑，跑不了写 NOT_RUN/BLOCKED 及原因；所有产物放 `builds/q1-*/`，脚本不复制进仓库）：
- `python3 -I <skill>/scripts/detect_profile.py .`
- `python3 -I <skill>/scripts/arch_lint.py . [--config builds/q1-arch/layers.json]`：默认层映射是 MCU 分层模板，若不适用本工程组件模型，按 AGENTS §3 依赖表写一份 layers.json 放 `builds/q1-arch/` 再跑，并说明映射。
- `python3 -I <skill>/scripts/style_gate.py --root . --manifest builds/q1-style/style-gate.json`：范围选自有 C++（`main/`、`components/*/src|include`），用 `builds/` 下已有 IDF 构建的真实 `compile_commands.json`；`llvm_major` 与本机 clang 一致。命名/Doxygen 结果与项目规则冲突时以项目为准，归 B 类。
- `python3 -I <skill>/scripts/cpp_cost.py check <已有 IDF 构建的 rtk_main.elf> --tier T2`（若无现成构建，`idf.py -B builds/q1 build` 一次）。
- 项目自身门禁抽查：`clang-format --style=file --dry-run --Werror` 全部自有 C/C++、`sha256sum -c docs/frozen.sha256`、`python3 -I tools/g7b_check/g7b_check.py --build builds/q1`、
  `python3 -I tools/tidy_check/*.py`（若有用法说明）。host 测试本卡不要求全量重跑。

## 4 阶段 B：同步文档（只改以下文件）

允许修改：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/*.md`、`docs/acceptance.md`。其余一律不动（含代码、`.clang-*`、`tools/`、`docs/frozen.sha256`——哈希由 Claude 验收时重算）。

- **C 类漂移**：直接改文档使其与已核实的代码一致；每处改动在报告里列 `文件:节` + 改前/改后一句话。
- **B 类**：在 `docs/CODING-STYLE.md` 末尾新增一节「与 embedded-engineering 技能的对照（技能 git 5cae6d9）」，表格列：技能 Rule ID｜技能要求（一句话）｜本项目做法｜状态（覆盖/有意偏离/缺口）｜依据或理由。只登记有差异或易混的条目，不抄全量规则。
- **规则实质变更不自己拍板**：凡是会收紧或放宽现有规则、或要求改代码的（包括 B 类“缺口”的采纳），**只写进报告的“待决提议”**，不改文档正文。
- 修改 ADR 时不改其“决策”内容，只修正事实性路径/名称；必要时在 ADR 末尾加「勘误 2026-11-17」小节。
- 文档语言：中文，必要英文术语保留；Markdown 风格与原文件一致。

## 5 约束

- 不修改代码/测试/构建/门禁配置；不 git add/commit/push/stash/reset；不安装软件、不联网（技能仓库 `git log` 是本地操作，允许）。
- 不接触硬件（flash/monitor/串口均禁止）。
- 工作区可能有他人未提交改动：只读，不还原。
- 不得因“技能这么写”就把项目已登记的明确决策判为违规（见 §2）。
- 发现自己无法核实的事实写 `UNKNOWN`，不猜。

## 6 报告 `docs/reports/Q1.md`

1. 摘要表：A/B/C 各类数量与严重度分布；门禁逐项 PASS/FAIL/NOT_RUN/BLOCKED。
2. A 类发现（按严重度排序）。
3. B 类对照结论（与 CODING-STYLE 新节对应，并标出“缺口”）。
4. C 类漂移与已做的文档修正清单。
5. 待决提议（需要 Claude/用户决定的规则变更或代码修复，每条给影响面与建议）。
6. 实际执行的命令与关键输出行（不贴长日志，日志放 `builds/q1-*/`）。
7. 未覆盖范围与原因。

完成后在终端输出一行 `Q1 DONE` 并停止，不要继续修代码。
