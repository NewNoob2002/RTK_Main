# R6 新增模块骨架 `_template/` 与“如何新增一个模块”说明（ADR-0008 §5 R6）

执行方：pi + gemini。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`、`docs/CODING-STYLE.md`（§1.3 注册集中、§3.3 命名空间、§5.4 全局状态、§6 注释、§7 堆档位），再读现有模块作参照：
`main/module_list.inc`、`main/app_config.hpp`（ModuleKind/ExecKind）、`main/app_context.hpp`、`main/app_main.cpp`（MODULE_DEF 展开、kModuleOps）、
`components/app_fw/include/rtk/fw/module.hpp`（make_ops<T>() 的 requires 检测）、`main/modules/system/*`（最简单的现有模块）、`main/modules/rtcm/rtcm_cmds.cpp`（控制台命令表写法）、`main/def/*_def.hpp`。

## 目标
新人复制一个目录、改名、在一处注册，就能得到一个可编译、可被执行器调度、带控制台命令的模块；模板本身持续被编译，不会烂掉。

## 要做
1. `main/modules/_template/`：
   - `template_module.hpp` / `template_module.cpp`：`final` 类 `TemplateModule`，构造 `constexpr explicit TemplateModule(AppContext &)`，禁拷贝/移动；只实现**最小必需**操作（以 make_ops 实际检测的签名为准，例如 `init`/`poll`/`on_command` 中哪些是可选的就演示“可选”），每个操作写清调用上下文（哪个执行器、是否可阻塞、返回值含义）。私有成员演示：状态放成员、常量放 `def/` 或 .cpp 匿名命名空间并标单位。
   - `template_cmds.cpp`：一个最小的控制台命令表（如 `tmpl status`），演示参数校验 + guard clause + 具名处理函数。
   - `README.md`（中文）：“如何新增一个模块”分步清单：复制目录 → 全局改名（列出要改的标识符：类名、文件名、命名空间内函数名、命令组名）→ `def/<name>_def.hpp` 放常量/类型 → `app_config.hpp` 加 `ModuleKind` → `module_list.inc` 加一行 MODULE_DEF（说明每列含义与顺序约束）→ `app_context.hpp` 的成员如何由 MODULE_DEF 生成（如需手改说明在哪）→ 控制台命令表如何接入 `app_main.cpp` → 数据发布到 DataCenter 的方式（Topic/Slot）→ 需要持久化时如何加 StoreKey → 需要堆时 §7 的登记 → 必跑的检查清单。每步给出真实文件路径；与现有代码不一致的地方以代码为准并在报告里列出。
2. **模板不进固件**：`main/CMakeLists.txt` 的 `GLOB_RECURSE modules/*.cpp` 当前会把 `_template` 编进固件——改成排除 `modules/_template/`（加注释说明原因）。固件大小必须与改前完全一致。
3. **模板持续编译**：在主机测试里编译模板（优先放进 `tools/main_host_check`，新增一个 `template_module` 场景：用 `fw::make_ops<TemplateModule>()` 生成 ops，跑一遍 init/poll/command 与命令表解析，断言基本行为）。主机测试数 +1。
4. 修正 R4 遗留的注释不一致：`main/modules/system/system_module.hpp` 第 1 行与 `system_module.cpp` 第 1 行写“kSystem 执行器/任务”，但 `ExecKind` 只有 `kLink`，类注释也写 kLink——统一为实际情况（kLink）。顺手检查其它模块文件头里的执行器名是否与 `module_list.inc` 一致，只改注释。
5. `docs/CODING-STYLE.md` §1.3 把“（R6 提供）”改为指向 `main/modules/_template/README.md`；用 `sha256sum $(awk '{print $2}' docs/frozen.sha256) > docs/frozen.sha256` 重算（若 CODING-STYLE 不在清单内则不变），`sha256sum -c` 全 OK。

## 不做
不改现有模块行为、不改 app_fw 公共头、不加新组件、不加依赖。模板保持短小（三个源文件合计建议 ≤ 200 行），读起来像真实模块而不是注释堆。

## 自检（写入 `docs/reports/R6.md`，逐项 PASS/FAIL，附命令）
1. `. ~/esp/esp-idf/export.sh >/dev/null && IDF_COMPONENT_MANAGER=0 idf.py -B builds/r6 build` 0 warning；`idf.py -B builds/r6 size` 与 HEAD 构建（`builds/r6-base`）DRAM/Flash/镜像大小**完全一致**。
2. main_host_check 普通 + ASan/UBSan：30/30（原 29 + template）；t13 29/29；test/host、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/rtk_console 全部通过、用例数不变（构建目录 `builds/r6-*`）。
3. **演练**：在临时副本（`builds/r6-drill/` 下的仓库拷贝或 git worktree，不得留在主工作区）按 README 步骤把模板复制成 `demo` 模块并注册，`idf.py build` 0 warning、控制台命令表包含 `demo`；把演练中 README 不清楚或缺步骤的地方修回 README，并在报告里记录演练过程与最终 diff 摘要。
4. clang-format `--dry-run --Werror`、`git diff --check`、G7b、冻结清单全部 PASS。

报告最后一行写 `R6 DONE` 或 `R6 BLOCKED: <原因>`。

## 禁止
烧录、串口、联网安装、git commit；改 `reference/**`、历史报告/ADR。只用 `builds/r6*`。
