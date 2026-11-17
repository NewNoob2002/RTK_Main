# Q3：门禁覆盖补齐（g7b 堆检查纳入 gnss、tidy 覆盖全部自有源）

状态：ISSUED 2026-11-17，基线 `feat/gnss` HEAD（见 `git log -1 -- docs/tasks/Q3-gate-coverage.md`）。
实施：omp + workbuddy/deepseek-v4.1-flash；规划/验收：Claude。

## 0 背景与已定决定（Q3-T0，与本卡同一提交）

Q2 报告 §3 留下两个门禁空洞；Claude 复查时发现 tidy 的空洞比报告说的大：`tools/tidy_check/tidy_check.py`
用 `main/*.cpp` 非递归通配，`main/app|data|support|modules/**` 全部没有被检查；`components/{app_fw,cli,stream_demux,bt_reconnect,gnss}`、
`components/link_core/services` 也不在列表里。Claude 用临时脚本对这些文件试跑：52 个文件，去重后 159 条发现，分布：

| 文件 | 条数 | 主要类别 |
| --- | --- | --- |
| components/gnss/src/NmeaParser.cpp | 67 | magic-numbers（字段号等）、signed-bitwise（整型提升）、function-size（parse_gga/parse_gsv）、const |
| components/cli/src/Console.cpp、Reply.cpp、Out.cpp | 57 | vararg、do-while（FW_LOGx 宏）——已由 T0 的 `components/cli/.clang-tidy` 屏蔽 |
| main/app/BootFlow.cpp | 9 | misc-const-correctness |
| main/modules/gnss/GnssModule.cpp | 8 | 7 个未使用常量（:70-79）、`on_command` function-size |
| components/gnss/src/FrameTag.hpp | 8 | C 数组、do-while、指针算术、signed-bitwise |
| components/gnss/include/gnss/Types.hpp | 5 | `1U << 0` 的 signed-bitwise——已由 T0 的 `IgnorePositiveIntegerLiterals` 消除 |
| 其余（Registry 命名、UnicoreReceiver:187、DebugKeys.hpp:57、ConsoleSetup、DebugCmds） | 各 1 | |

T0 已定（不要改）：
- **g7b 删除析构例外**：gnss 的 `U _ZdlPvj` 来自虚析构的删除析构 `D0Ev`，不是分配。决定**细化规则**而不是登记白名单
  （白名单按文件放行，会把同文件里将来真实的 `new` 一起放掉）。规则见 `docs/acceptance.md` G7b。
- **自有目录由列举得出**：g7b 的自有目标文件 = `main` + `components/` 下全部组件目录，不再手写清单。
- **tidy 覆盖全部自有源**，脚本自检覆盖率（`docs/acceptance.md` G5b）。gnss 用 `test/gnss` 全驱动构建的编译数据库
  （IDF 构建只启用 Unicore/u-blox，ComNav 不在 IDF 数据库里）。
- 根 `.clang-tidy`：`bugprone-signed-bitwise.IgnorePositiveIntegerLiterals: true`（及 hicpp 同名项），整型提升造成的有符号操作数仍报告。
- `components/cli/.clang-tidy`：屏蔽 vararg、do-while（理由见文件）。
- `components/link_core/include/core/Types.hpp` 文件头已补（Q2 留项）。

**冻结**：`AGENTS.md`、`docs/**`（`docs/reports/` 除外）、`components/*/include/**`、所有 `.clang-*`、`docs/frozen.sha256`、
`partitions.csv`、`sdkconfig.defaults` 一个字节都不改。不 `git add/commit/stash/reset/checkout`。不烧录。
需要改冻结文件才能解决的发现：停下，写进报告"留给 Claude"，附建议改法。

先读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/acceptance.md`（G5b、G7b）、`docs/reports/Q2.md` §3、本卡。定位代码用 `codegraph explore`。

## 1 任务

### T1 g7b：自有目录列举 + 删除析构例外

`tools/g7b_check/g7b_check.py`：
1. `OWN_COMPONENT_DIRS` 改为运行时列举：`main` + `ROOT/components/*/`（只取含 `CMakeLists.txt` 的目录）。打印列举结果。
2. 删除析构例外：对含 `_ZdlPv*`（只限 operator delete，不含 `_Zda*`，除非证据表明数组删除析构也会出现）的目标文件，
   用 `xtensa-esp32-elf-objdump -r <obj>` 取重定位表，按 `RELOCATION RECORDS FOR [<section>]` 分段：
   该符号的**每一条**重定位都位于名字匹配 `^\.text\..*D0Ev$` 的段 → 豁免，记入 `deleting-dtor exempt: <源文件>: <D0 函数>` 列表并打印；
   否则照旧按白名单裁决。段名依赖 `-ffunction-sections`（IDF 默认开启），在脚本注释里写明；若某目标文件没有逐函数段（只有 `.text`），不豁免。
   objdump 路径与 nm 一样可由参数指定（`--objdump`，默认 `xtensa-esp32-elf-objdump`）。
3. 现有 1b 段（守卫检查扫全部编译数据库对象）保留，但自有目录列举后它可能多余；保持行为不变即可，不要删检查范围。
4. 负向证据（临时改动，做完恢复，最终 diff 不留痕；报告给命令与输出）：
   a) gnss 某 `.cpp` 普通函数里写 `delete` 一个指针 → FAIL；
   b) gnss 某 `.cpp` 里 `new` 一个对象 → FAIL；
   c) 恢复后 PASS，且输出列出 UbloxReceiver/UnicoreReceiver 的删除析构豁免。

### T2 tidy_check：全覆盖 + 覆盖率自检

`tools/tidy_check/tidy_check.py`：
1. 新参数 `--gnss-build`（默认 `builds/gnss`）：`test/gnss` 全驱动配置的 CMake 构建目录（需要 `compile_commands.json`；
   若 `test/gnss` 的 CMake 没开 `CMAKE_EXPORT_COMPILE_COMMANDS`，用命令行 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`，不改冻结文件）。
   `components/gnss/src/**/*.cpp` 用这个数据库，host 语言标准。
2. ESP 侧：`main/**/*.cpp`（递归）、`components/{app_fw,cli,stream_demux,bt_reconnect}/src/*.cpp`、`components/link_core/services/*.cpp`
   及其他所有未覆盖的自有 `.c/.cpp`；纯组件若在 host 数据库里也有条目，选哪边由你按"能完整解析、配置与目标一致"决定并在报告说明。
3. 覆盖率自检：列举 `components/**`、`main/**` 下全部 `.c/.cpp`（排除各组件 `test/` 与第三方/生成文件），未被任何 job 覆盖且不在
   脚本内 `EXCLUDED = {路径: 理由}` 表中的 → 打印并以退出码 2 结束。已知需排除：`main/modules/_template/**`（模板，不参与构建）——
   确认它确实不在任何构建里再排除。
4. 更新脚本文件头的用法说明。

### T3 修掉全部发现

用扩展后的 tidy_check 跑到 0 发现。原则：
- 优先改代码（具名常量、拆函数、加 const、`static_cast<uint32_t>` 先于位运算等），行为不变；
- 协议字段号/偏移用具名常量，集中在文件内的匿名命名空间或已有常量区，不要散落；
- `readability-function-size`：拆成私有辅助函数；拆不动的写具名 `NOLINT(readability-function-size)` + 理由，并在报告列出；
- `GnssModule.cpp:70-79` 未使用常量：先查是否只在某配置（TRACE 关/开、驱动开关）下用到；真未使用就删，配置相关就挪进对应 `#if`；
- `Registry.cpp` 的 `kReceivers` 命名：参考 Q2 对 `kXxxReceiver` 的处理，保持一致；
- 只能 NOLINT 的写具名 NOLINT + 理由；不得新增目录级屏蔽（`.clang-*` 冻结）。
- 公共头（`components/*/include/**`）里的发现：停下，留给 Claude。
- 改动的函数补 doxygen（§6.2）。

## 2 门禁（全部跑，报告逐条写命令与结果）

构建目录一律 `builds/q3*`。
1. host：AGENTS §5 每个目录普通 + ASan/UBSan；`test/gnss` 全部驱动组合；`tools/main_host_check`、`tools/t13_host_check`。
2. `idf.py -B builds/q3 build` 0 warning；TRACE 关再构建一份；镜像字节与 `builds/q2` 对比（应只有微小变化，解释原因）。
3. `cpp_cost.py --prefix xtensa-esp32-elf- diff builds/q2/rtk_main.elf builds/q3/rtk_main.elf`。
4. `g7b_check --build builds/q3` PASS（gnss 已纳入）；T1 负向证据。
5. `tidy_check --host-build builds/q3-host --idf-build builds/q3 --gnss-build builds/q3-gnss --test-app-build <rtk_pure 构建>` 0 发现，覆盖率自检通过，报告给出文件总数。
6. clang-format 改动文件 0 差异；`tools/doc_check` PASS。
7. `sha256sum -c docs/frozen.sha256` 全 OK。
8. `git diff --stat` 不含 golden `*.inc`、`test/host/**`。

## 3 交付

报告 `docs/reports/Q3.md`：每个 T 的改动（文件:行）、取舍、NOLINT 清单（位置+理由）、门禁结果表（PASS/FAIL/NOT_RUN/BLOCKED）、
留给 Claude 的项。HIL 由 Claude 做。不提交。完成后终端打印 `Q3 DONE`。
