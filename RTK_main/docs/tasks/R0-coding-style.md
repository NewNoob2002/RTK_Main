# R0 编码规范文档与规则更新（ADR-0008 §4、§6、§7、§8）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`、`docs/adr/ADR-0002-cpp-esp-idf.md`、
`.clang-format`、`.clang-tidy`、`.clang-tidy-c`、`tools/g7b_check/g7b_check.py`，再浏览 `main/` 与 `components/` 现有代码，了解实际写法。

参考材料（只读）：X-TRACK 两个分支的源码已克隆在
`/tmp/claude-1000/-home-gtc-Desktop-workspace-ESP32-PROJ-P2-RTK/84006cc8-9758-45f1-a393-77c17b49f97b/scratchpad/xtrack/`
（`main/Software/X-Track/USER/App`、`v9/Firmware/X-TRACK`）。用户要求：**不必和 X-TRACK 一样，可以在其基础上优化，但必须保证可读性**。

## 交付
1. 新建 `docs/CODING-STYLE.md`（中文，面向人读，每条规则配一个简短的“这样写 / 不要这样写”代码示例）。至少包含：
   - 目录与文件：ADR-0008 §3 目标结构；一个功能一个目录；`def/` 放跨模块共享类型；模块清单文件；`_template/`；单文件建议 ≤ 500 行。
   - 文件头：第一行一句话说明“这个文件做什么”，第二行起写线程/调用约束（谁调用、可否阻塞、是否 ISR 安全）；**不写** `Generated-by`/`人工负责人`，生成来源记录在 git 提交信息的 trailer（`Generated-by: pi + <模型>`）。
   - 命名（保持 snake_case，ADR-0008 §7）：类型 PascalCase，函数/变量 snake_case，常量与枚举值 `kName`，私有成员 `name_`，C 接口 `rtk_<组件>_<动作>`、文件静态 `s_`、类型 `_t`；命名空间 `rtk::<组件>`；include 根 `rtk/<组件>/…`（ADR-0008 §4 对照表）。
   - 文件内容（ADR-0008 §6 全部条目）：类成员顺序、函数长度与嵌套、guard clause、事件/命令 switch 分派、错误处理与只记一次日志、参数传递、日志格式、常量分组与单位、注释（公开 API 用 `/** */` 说明做什么/谁调用/可否阻塞/错误返回；实现内只写“为什么”）。
   - 模块写法：ADR-0008 §4 的“模块类 + `fw::make_ops<T>()` + 清单一行注册”目标形态（R2 会实现，这里写规范与示例）。
   - 堆内存分级（ADR-0008 §8，用户已同意）：禁止区（ISR、蓝牙回调、链路任务每轮路径、发送任务）/ 启动期一次性分配 / 非实时路径限量分配；异常关闭下 `std::vector`/`std::string` 分配失败会 abort，需要处理失败处用 `new (std::nothrow)` 或预先 `reserve`；使用堆的文件必须登记在 g7b 白名单并在文件头注明属于哪一档。
   - C 文件规则：沿用 AGENTS.md 现有 C 规则（BARR-C 安全核心等），只做整理不改语义。
   - 与 `.clang-format`/`.clang-tidy` 的关系：格式交给工具，文档不重复格式细节。
   - 附录：“从 X-TRACK 学了什么、改了什么”简表（引用 ADR-0008 §2）。
2. 更新 `AGENTS.md`（本任务授权修改此冻结文件）：
   - 溯源条（第 34 行附近）改为 CODING-STYLE 的文件头规则 + 提交 trailer。
   - 禁止清单（第 27 行附近）中的 `new/delete/malloc/free`、`<vector>`、`<string>` 改为“按 CODING-STYLE 堆内存分级使用，须登记白名单”；`<map>`/`<list>`/`<deque>`/`<unordered_*>`/`std::function`/`shared_ptr`/异常/RTTI 等其余禁止项**保持不变**。
   - 加一条：编码规范以 `docs/CODING-STYLE.md` 为准，AGENTS.md 只保留硬性门禁与授权边界；删去与 CODING-STYLE 重复的细节时，硬性约束（告警集、冻结、授权）不得丢失。
3. 更新 `docs/adr/ADR-0002-cpp-esp-idf.md` 第 36 行“堆”一行与第 72 行 G7b 描述：改为分级规则并注明“2026-11-12 由 ADR-0008 §8 修订”（授权修改）。同步 `docs/acceptance.md` 中 G7b 条款的描述。
4. `tools/g7b_check/g7b_check.py`：新增 `tools/g7b_check/heap_allowlist.txt`（每行一个相对源文件路径 + `#` 注释写档位与理由；初始为空，只含格式说明注释）。自有目标文件出现堆符号时，若对应源文件在白名单中则放行并在输出中列出，否则 FAIL。全 ELF 异常/RTTI 白名单逻辑不变。顺带把该脚本文件头改成新规则（第一行说明用途，删 `Generated-by/人工负责人`）。

## 自检（写入 `docs/reports/R0.md`，逐项 PASS/FAIL）
1. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B builds/r0 build` 后 `python3 -I tools/g7b_check/g7b_check.py --build builds/r0` PASS（白名单为空时结果与改前一致）。
2. 构造验证：临时把一个 main 源文件加入白名单副本（用 `--allowlist` 参数指向临时文件，或在报告里说明测试方法），证明放行/拒绝两条路径都正确；不得把临时改动留在仓库。
3. `python3 -I -m py_compile tools/g7b_check/g7b_check.py`；如本机有 ruff 则 `ruff check` 该文件。
4. 对照清单：报告列出 AGENTS.md 修改前后的禁止项与硬性门禁，证明除堆规则与溯源规则外没有丢失任何约束。

## 并行约束
R5 同时运行，它修改 `components/rtk_bt/*`、`tools/t13_host_check/*`。你不得修改这些文件；你只能用 `builds/r0/`，不得使用 `build/`。

## 禁止
修改源代码（`main/`、`components/`、`test/`，g7b 脚本除外）；`docs/frozen.sha256` 由验收方重算，你不要改；烧录、连接串口、联网安装、git commit。

完成后在报告最后一行写 `R0 DONE` 或 `R0 BLOCKED: 原因`。
