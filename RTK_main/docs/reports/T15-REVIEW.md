# T15 验收记录

日期：2026-11-11。范围：`test_apps/rtk_pure`（双架构纯组件测试应用）。验收：Claude Opus 5.5（Claude Code，架构/验收方）。人工负责人 UNKNOWN（待人工审查签字）。

## 结论

**T15 PASS（软件门禁）**。上板运行与 pytest 为 NOT_RUN（未授权烧录）；新用例在目标上的实际运行结果尚未验证。

## 分工

| 角色 | 执行者 | 产出 |
|---|---|---|
| 实现 | omp + codex/gpt-6.1-sol（thinking high） | `test_apps/rtk_pure/**`、`T15.md` |
| 独立复审 | codex + antigravity/gemini-3.8-flash（high，workspace-write 沙箱） | `T15-REVIEW-GEMINI.md`：0 High / 1 Medium / 2 Low |
| 返工 | codex + gpt-6.1-sol（high，workspace-write 沙箱） | `test_main.cpp` 补 `<cstdint>`；`T15.md`“复审返工”节 |
| 规则修订、验收 | Claude Opus 5.5 | `test_apps/rtk_pure/.clang-tidy`、`docs/acceptance.md` G5b、本报告 |

## 复审发现处理

| 严重度 | 发现 | 处理 |
|---|---|---|
| Medium | `test_main.cpp` 15 条 clang-tidy 发现，全部来自 IDF `unity_test_runner.h:81/85` 中 `TEST_CASE` 宏的 `static test_func test_fn_[]` | 宏内无法 NOLINT；在 `test_apps/rtk_pure/.clang-tidy` 中对测试目录关闭 `cppcoreguidelines-avoid-c-arrays`、`cppcoreguidelines-pro-bounds-array-to-pointer-decay`、`misc-const-correctness` 并注明原因；产品代码仍执行。复跑 0 发现 |
| Low | 溯源头为 `omp + codex/gpt-6.1-sol`，与 AGENTS.md 模板不同 | 不改。验收方在实现任务说明中要求按实际模型署名；与 AGENTS.md §2 模板的偏离在此记录 |
| Low | `test_main.cpp` 未显式包含 `<cstdint>` | 已修复（返工） |

## 验收方独立执行的命令

每个 shell 先 `. ~/esp/esp-idf/export.sh`，`IDF_COMPONENT_MANAGER=0`。

| 命令 / 检查 | 结果 | 证据 |
|---|---|---|
| `idf.py -B build-accept-esp32 set-target esp32 build`（干净目录） | PASS | rc 0，0 warning |
| `idf.py -B build-accept-esp32c3 set-target esp32c3 build`（干净目录） | PASS | rc 0，0 warning |
| 自有 12 个 .obj（main、rtk_proto、rtk_core）`nm` / `nm -C` | PASS | 两目标堆/异常符号 0，typeinfo 0 |
| 两目标 `rtk_pure.elf` 异常/RTTI 符号 | PASS | 0 |
| `python3 -I tools/tidy_check/tidy_check.py --idf-build build-t14-review --test-app-build test_apps/rtk_pure/build-accept-esp32` | PASS | 27 个文件，0 发现（含 TIDY-FIX 后的 main/rtk_bt） |
| `clang-format --style=file --dry-run --Werror` 两个测试源文件 | PASS | rc 0 |
| `ctest --test-dir build-host` / `build-asan` | PASS | 各 8/8 |
| `sha256sum -c docs/frozen.sha256` | PASS | 全部 OK |
| 上板运行、pytest | NOT_RUN | 未授权烧录/串口 |
| 权威 astyle_py | NOT_RUN | 需联网安装 |

用例期望值溯源（CRC 向量、`g_frame_a/b`、0x41 心跳、LinkManager 转移）由复审方逐条对照公共头与 `test/host` 冻结用例核对并用 `zlib.crc32` 验算；验收方抽读源码未发现矛盾。

## 本批次门禁/规则修订（用户 2026-11-11 授权）

- `.clang-tidy`：`GlobalVariablePrefix` g_ → s_，删除 `StaticVariablePrefix`（与 AGENTS.md §2 对齐）。
- 新增目录配置：`test/host/.clang-tidy`、`main/.clang-tidy`、`components/rtk_bt/.clang-tidy`、`components/rtk_os/.clang-tidy`、`test_apps/rtk_pure/.clang-tidy`（每项放宽均有注释说明）。
- `.clangd`：补 `bugprone-signed-bitwise` 屏蔽及 GCC 专有选项移除。
- `tools/tidy_check/tidy_check.py`：G5b 门禁脚本，覆盖全部自有代码。
- `docs/acceptance.md` G5b 行改为调用该脚本；`docs/frozen.sha256` 已同步（含 `test/host/.clang-tidy`、TIDY-FIX 授权的 `static_task.hpp:87`）。

## 未完成

- TIDY-FIX（DeepSeek）尚未安排独立复审；本次 G5b 0 发现是对其结果的自动化验证，但行为等价性未经第二方审查。
- `test_apps/rtk_pure/build-*`、`build-review-*`、`build-accept-*` 等构建目录保留在工作树中，未清理。
