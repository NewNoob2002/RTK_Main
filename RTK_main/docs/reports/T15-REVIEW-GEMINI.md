# T15 独立复审报告（Gemini）

- 复审范围：`test_apps/rtk_pure/CMakeLists.txt`、`test_apps/rtk_pure/sdkconfig.defaults`、`test_apps/rtk_pure/main/CMakeLists.txt`、`test_apps/rtk_pure/main/test_main.cpp`、`test_apps/rtk_pure/main/test_proto_c.c`、`test_apps/rtk_pure/pytest_rtk_pure.py`（`docs/reports/T15.md` 仅作参考，所有结论均经独立构建、符号检查、代码推演与工具验证）。
- 复审约束：对工程代码与冻结文件完全只读；独立构建目录使用 `test_apps/rtk_pure/build-review-gemini-esp32` 与 `test_apps/rtk_pure/build-review-gemini-esp32c3`，未执行烧录、串口连接或 pytest 运行。

## 1. 复审发现

### [Medium] `tidy_check.py`（G5b）在 `test_apps/rtk_pure/main/test_main.cpp` 上失败，报 15 条 `TEST_CASE` 宏展开错误

- **文件:行号**：`test_apps/rtk_pure/main/test_main.cpp:28`、`test_apps/rtk_pure/main/test_main.cpp:46`、`test_apps/rtk_pure/main/test_main.cpp:69`、`test_apps/rtk_pure/main/test_main.cpp:93`、`test_apps/rtk_pure/main/test_main.cpp:117`（关联配置 `test_apps/rtk_pure/.clang-tidy:5-13`）
- **证据**：
  1. 执行 `python3 -I tools/tidy_check/tidy_check.py --idf-build build-t14-review --test-app-build test_apps/rtk_pure/build-review-gemini-esp32` 返回退出码 `1`（`G5b FAIL`），`test_apps/rtk_pure/main/test_proto_c.c` 为 `0` 条，而 `test_apps/rtk_pure/main/test_main.cpp` 报 `15` 条错误（5 处 `TEST_CASE` 各触发 3 条）：
     - `cppcoreguidelines-avoid-c-arrays`
     - `cppcoreguidelines-pro-bounds-array-to-pointer-decay`
     - `misc-const-correctness`（`pointee of variable 'test_fn_' of type 'test_func[1]' (aka 'void (*[1])()') can be declared 'const'`）
  2. 对照 IDF `~/esp/esp-idf/components/unity/include/unity_test_runner.h:77-94`（第 81 行 `static test_func test_fn_[]` 与第 85 行 `.fn = test_fn_`），上述 3 类诊断全部来自 `TEST_CASE` 宏内部展开。在临时编译数据库中仅额外屏蔽这 3 项检查后对 `test_main.cpp` 运行 `clang-tidy`，退出码为 `0`（0 发现），表明用户编写的测试逻辑本身无 clang-tidy 违规。
  3. `test_apps/rtk_pure/.clang-tidy` 不在 `T15-test-app.md` 交付列表中（且其存在未在 `T15.md` 中说明），目前已屏蔽 `do-while`/`union-access`/`vararg`/`signed-bitwise` 等 IDF/Unity 宏展开项，但遗漏了 `TEST_CASE` 宏在 C++ 下展开触发的上述 3 项。
- **建议修复**：在 `test_apps/rtk_pure/.clang-tidy` 的 `Checks:` 中增加排除 `-cppcoreguidelines-avoid-c-arrays`、`-cppcoreguidelines-pro-bounds-array-to-pointer-decay`、`-misc-const-correctness`，并注释说明仅针对 `unity_test_runner.h` 中 `TEST_CASE` 宏展开产生的 `test_fn_`（或按需由仓库门禁维护者在对应评审批次统一收口）。

### [Low]（待确认）新源文件溯源头模型字段与 `AGENTS.md §2` 字面模板不一致

- **文件:行号**：`test_apps/rtk_pure/main/test_main.cpp:2`、`test_apps/rtk_pure/main/test_proto_c.c:2`、`test_apps/rtk_pure/pytest_rtk_pure.py:2`
- **证据**：
  - `AGENTS.md §2` 规定：`每个新源文件头注释写 Generated-by: omp + DeepSeek-V4.1-Flash，人工负责人 UNKNOWN（待人工审查签字）。不得添加 Signed-off-by。`
  - 上述三个新文件均写为 `Generated-by: omp + codex/gpt-6.1-sol` 与 `Owner: UNKNOWN（待人工审查签字）`（`Owner:` 格式与工程 `main/`、`components/` 既有源码一致，且均未添加 `Signed-off-by`）。
  - `docs/reports/T15.md` §4 说明该偏离是依据用户补充指令 `T15-impl-brief.md` 改为实际模型，但仓库工作树中不存在 `T15-impl-brief.md`，独立复审缺少该上游指令原文以核实授权。
- **建议修复**：**待确认**（缺失证据：`T15-impl-brief.md` 原文或用户确认记录）。若用户确曾要求如实署名为 `codex/gpt-6.1-sol`，可保留现状；否则应按 `AGENTS.md §2` 模板对齐。

### [Low] `test_apps/rtk_pure/main/test_main.cpp` 直接使用 `uint8_t` 但未显式 `#include <cstdint>`

- **文件:行号**：`test_apps/rtk_pure/main/test_main.cpp:4`（使用位置：`test_main.cpp:30,31,41,48,50,60,64`）
- **证据**：第 4–9 行仅包含了 `<array>`、`<span>`、`"rtk/core/link_manager.hpp"`、`"rtk/proto/bytes.hpp"`、`"unity.h"`，未包含 `<cstdint>`，第 30、50、64 行的 `std::array<uint8_t, ...>` 依赖传递包含生效（对照冻结测试 `test/host/test_bytes.cpp:3` 显式包含了 `#include <cstdint>`）。
- **建议修复**：在 `test_apps/rtk_pure/main/test_main.cpp` 头文件区补充 `#include <cstdint>`。

## 2. 已核对无问题的项

1. **契约符合性与依赖边界（G3b / T15-test-app.md）**：
   - `test_apps/rtk_pure/CMakeLists.txt:4-5` 使用 `set(EXTRA_COMPONENT_DIRS ../../components/rtk_proto ../../components/rtk_core)` 与 `set(COMPONENTS main)`，且第 7 行将 `SDKCONFIG` 定向到 `${CMAKE_BINARY_DIR}/sdkconfig`，隔离双架构配置。
   - 核对两个独立构建产物的 `project_description.json`（`build_components` 与 `build_component_info`）：工程自有组件仅引入 `rtk_proto`、`rtk_core` 与测试应用 `main`，未引入 `rtk_bt`、`rtk_os`、`rtk_store` 或产品根目录 `main`；未触及板级引脚或硬件外设。
   - 文件划分符合任务卡：C 用例在 `test_proto_c.c`（只包含 `rtk_proto_frame.h`、`rtk_proto_alarm.h`、`unity.h`，无 C++ 头与调用），C++ 用例及 `app_main` 在 `test_main.cpp`。
   - 用例数量满足“各至少两个”：C 用例 7 个（CRC 2 个、帧组包/解析/恢复 3 个、0x41 心跳 2 个），C++ 用例 5 个（`ByteWriter` 越界粘滞 2 个、`LinkManager` 配对/待定/窗口过期 3 个），共 12 个。

2. **用例期望值溯源与目标端运行正确性推演**：
   - **C CRC32（2 条）**（`test_proto_c.c:10-25`）：`0xCBF43926U` 与分段累加、空/NULL 保留种子 `0x12345678U` 可溯源至 `rtk_proto_frame.h:79-82`、`phone_protocol.md §1` 及 `test/host/test_proto_frame_c.c:54-62`，并经 Python `zlib.crc32` 验算一致。对照 `rtk_proto_frame.c:203-216` 逐行推演，在 32 位目标上必 PASS，无恒真断言。
   - **C 帧编解码与恢复（3 条）**（`test_proto_c.c:28-110`）：`expected` 字节序列分别与 `test/host/test_proto_frame_c.c:9-14` 的 `g_frame_a`（24 B 空帧，CRC `69 17 6e 7e`）、`g_frame_b`（26 B 设置应答，CRC `c0 20 da 74`）逐字节一致，并经 Python `zlib.crc32` 验算通过；缓冲区差 1 字节返回 `RTK_PROTO_ERR_SPACE` 且不改写 `out_len`、篡改 payload 触发 `RTK_PROTO_PARSE_CRC_ERR` 且 `crc_errors == 1U` 后恢复正常解析，可溯源至 `rtk_proto_frame.h:90-108` 与 `test/host/test_proto_frame_c.c:75-110,186-199`。对照 `rtk_proto_frame.c:102-330` 推演必 PASS，局部栈占用约 350 B，远低于 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584`。
   - **C 0x41 心跳（2 条）**（`test_proto_c.c:113-142`）：`seq=5`、`battery_pct=80`、`local_state=0` 编码为 `{0x05, 0x00, 0x50, 0, 0, 0, 0, 0}`，可溯源至 `phone_protocol.md §5`、`rtk_proto_alarm.h:44-60` 与 `test/host/test_proto_alarm_c.c:72-99`；短缓冲 `0xEE` 哨兵不被写入与空指针保护对照 `rtk_proto_alarm.c:115-141` 推演必 PASS。
   - **C++ `ByteWriter` 越界粘滞（2 条）**（`test_main.cpp:28-66`）：溯源至 `bytes.hpp:14-36` 与 `test/host/test_bytes.cpp:30-74`。对照 `bytes.cpp:17-123` 推演，先写入 2 字节后再写 2 字节（剩余 1 字节）触发粘滞失败，后续 `u8`/`u32`/`zeros`/`fixed_str`/`bytes` 均被忽略且 `span` 外哨兵 `0xEE` 及尾部 `0x77` 保持不变，必 PASS。
   - **C++ `LinkManager` 配对流程（3 条）**（`test_main.cpp:69-134`）：溯源至 `link_manager.hpp:15-88` 与 `test/host/test_link_manager.cpp:129-196`。对照 `link_manager.cpp:44-271` 推演：路径 1 验证初始无手机时未知设备获 `kPhone`、开窗后 `0x41` 晋升 `kAlarm`、重复 HELLO 不重复加白名单、断开重连直认 `kAlarm`；路径 2 验证手机在线时开窗接入得 `kPending`、HELLO 晋升 `kAlarm` 且保留手机角色、断开重连保留双角色；路径 3 验证 `500 + kPairWindowMs - 1` 仍开放、`500 + kPairWindowMs` 关闭，随后发 HELLO 返回 `kNone`、`actions.count == 1` 且白名单为空。3 条用例均独立构造栈上对象，无跨用例状态污染，必 PASS。

3. **Unity 运行方式与 `pytest_rtk_pure.py`**：
   - `test_main.cpp:139-144` 通过 `extern "C" void app_main(void)` 依次调用 `UNITY_BEGIN()`、`unity_run_all_tests()`、`UNITY_END()`，不进入交互菜单 `unity_run_menu()`。
   - `test_apps/rtk_pure/main/CMakeLists.txt:5` 使用 `WHOLE_ARCHIVE`，防止无显式符号引用的 `test_proto_c.c.obj` 被归档链接丢弃。经检查 `rtk_pure.elf` 与 `rtk_pure.map`：
     - `esp32`：`__libc_init_array`（`.ctors`）包含 `test_main.cpp.obj`（`0x14` = 5 项）与 `test_proto_c.c.obj`（`0x1c` = 7 项），12 个 `test_reg_helper_*` 与 `test_func_*` 全部进入 ELF；`setUp`/`tearDown` 链接自 `unity_compat.c`。
     - `esp32c3`：`.init_array` 包含 `test_main.cpp.obj`（`0x14` = 5 项）与 `test_proto_c.c.obj`（`0x1c` = 7 项），12 个用例注册与函数体全部进入 ELF。
   - 对照 IDF `unity_runner.c:181-186` 与 `unity.c:2236-2261`，`UNITY_END()` 会打印标准汇总（`12 Tests 0 Failures 0 Ignored \nOK`）并调用 `unity_flush()`（`esp_rom_output_tx_wait_idle`）排空串口输出。
   - `test_apps/rtk_pure/pytest_rtk_pure.py:9-13` 使用 `@pytest.mark.generic` 与 `@idf_parametrize('target', ['esp32', 'esp32c3'], indirect=['target'])` 对双目标参数化，并调用 `dut.expect_unity_test_output(timeout=60)`，与非交互自动执行的 Unity 输出格式完全匹配；Python 语法检查通过。

4. **规范、编译选项、运行时符号与 `sdkconfig.defaults`**：
   - **双架构零告警**：`esp32` 与 `esp32c3` 干净构建均为 0 compiler warning、0 error。
   - **语言标准与告警选项**：`compile_commands.json` 与 `-E -dM` 预处理宏（`__STDC_VERSION__ == 201112L`、`__cplusplus == 202002L`）确认 `test_proto_c.c` 最终生效 `-std=c11`，`test_main.cpp` 最终生效 `-std=c++20`；`main/CMakeLists.txt:9-12` 按 `AGENTS.md §3` 分语言配置告警选项（因 IDF 全局传 `-Wno-error=extra`，实际与工程 `main/CMakeLists.txt` 等其他组件行为一致）。
   - **无堆、无异常/RTTI**：
     - 两目标自有 12 个目标文件（`main/*.obj` 2 个、`rtk_proto/*.obj` 6 个、`rtk_core/*.obj` 4 个）经原始符号与反修饰 `nm` 检查，`operator new/delete`、`malloc/free/calloc/realloc`、`__cxa_*`、`_Unwind_*`、`typeinfo`、`__gxx_personality_v0` 均为 **0**。
     - 两目标完整 `rtk_pure.elf` 中异常与 RTTI 符号均为 **0**；`malloc`/`free`/`calloc` 全部源自 `esp_libc/libesp_libc.a(heap.c.obj)`（由 `log` / `picolibc` 等 IDF 基础组件引入）。
   - **规范细节**：源码无禁用容器/特性；中文注释清楚；无类型前缀 `p_`/`b_`；无未加说明的窄化强制转换；格式检查 `clang-format --dry-run --Werror` 0 差异；全部行宽 `<= 120` 列。
   - **`sdkconfig.defaults`**：仅包含 `CONFIG_COMPILER_CXX_EXCEPTIONS=n` 与 `CONFIG_COMPILER_CXX_RTTI=n` 两项必要约束；两个目标生成的 `sdkconfig` 均已验证两者为 `not set`，未引入芯片专属配置。

## 3. 实际执行的命令与结果

以下构建与工具链检查均先执行 `. ~/esp/esp-idf/export.sh >/dev/null`（目标构建加 `export IDF_COMPONENT_MANAGER=0`）。

| 检查项 | 命令 | 结果 | 关键证据 |
| --- | --- | --- | --- |
| 冻结文件校验 | `sha256sum -c docs/frozen.sha256` | PASS | 115/115 项全部 `OK` |
| 格式检查 | `clang-format --style=file --dry-run --Werror test_apps/rtk_pure/main/test_main.cpp test_apps/rtk_pure/main/test_proto_c.c` | PASS | 退出码 `0`，0 处格式差异 |
| Python 语法检查 | `python3 -c "import ast, glob; [ast.parse(open(p).read()) for p in glob.glob('test_apps/rtk_pure/*.py')]; print('PY_SYNTAX_OK')"` | PASS | 输出 `PY_SYNTAX_OK` |
| ESP32 干净构建 | `cd test_apps/rtk_pure && idf.py -B build-review-gemini-esp32 set-target esp32 build` | PASS | 退出码 `0`，0 warning/error；`rtk_pure.bin` `0x212f0` B（剩余 87%） |
| ESP32-C3 干净构建 | `cd test_apps/rtk_pure && idf.py -B build-review-gemini-esp32c3 set-target esp32c3 build` | PASS | 退出码 `0`，0 warning/error；`rtk_pure.bin` `0x21ba0` B（剩余 87%） |
| 组件/标志/配置核查 | 检查双架构 `project_description.json`、`compile_commands.json`、`sdkconfig` | PASS | 仅含 `main`、`rtk_core`、`rtk_proto`；生效 `-std=c11`/`-std=c++20`；异常/RTTI 未启用 |
| 符号与注册核查 | 对双架构 12 个自有 `.obj` 与 `rtk_pure.elf` 执行 `xtensa-esp32-elf-nm` / `riscv32-esp-elf-nm` 并核对 `rtk_pure.map` | PASS | 自有 `.obj` 堆/异常/RTTI 为 0；ELF 异常/RTTI 为 0；12 个 `TEST_CASE` 构造器及 `.ctors`/`.init_array` 全部保留 |
| clang-tidy（G5b） | `python3 -I tools/tidy_check/tidy_check.py --idf-build build-t14-review --test-app-build test_apps/rtk_pure/build-review-gemini-esp32` | FAIL | `test_proto_c.c` 0 条；`test_main.cpp` 15 条（均在 `TEST_CASE` 宏展开行，退出码 1） |
| 排除宏项 tidy 复核 | 临时编译数据库加 `--checks=-cppcoreguidelines-avoid-c-arrays,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-misc-const-correctness` 检查 `test_main.cpp` | PASS | 退出码 `0`，0 发现 |
| 协议向量独立验算 | Python `zlib.crc32` 独立计算 `123456789`、`g_frame_a`、`g_frame_b` 校验和及行宽检查 | PASS | `0xCBF43926`、`69176e7e`、`c020da74` 全部一致；所有文件行宽 ≤ 120 |
| Host 回归基线 | `ctest --test-dir build-host --output-on-failure` | PASS | 8/8 可执行文件通过（0.01 s） |
| 上板运行与 pytest | — | NOT_RUN | 按复审禁止项未执行烧录、串口连接与 pytest 运行 |
| 权威 astyle_py | — | NOT_RUN | 按规范禁止联网下载/安装 |
