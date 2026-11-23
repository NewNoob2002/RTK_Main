# RTK_main 工程规则（给实现 Agent）

本文件是本目录下所有代码改动的项目级规则，优先级高于通用默认值。架构见 `docs/adr/ADR-0002-cpp-esp-idf.md` 与 **`docs/adr/ADR-0003-c-cpp-split.md`（C/C++ 分工，优先于 ADR-0002）**。字节流分流器见 `docs/adr/ADR-0004-stream-demux.md`（语义）与 **`docs/adr/ADR-0005-stream-demux-cpp.md`（C++ 实现、组件命名，优先于 ADR-0004）**。应用层框架（模块、执行器、指令路由、服务、控制台、重连）见 **`docs/adr/ADR-0007-app-services-split.md`**，与旧描述冲突时以它为准。其后的决策：ADR-0008 代码组织、ADR-0009 追踪/控制台/日志、ADR-0010 与 **ADR-0012** GNSS（驱动以 0012 为准）、ADR-0011 数据通路与内存。同一主题新 ADR 优先于旧 ADR。

## 1. 已核实的事实

| 项 | 值 | 来源 |
| --- | --- | --- |
| 芯片 | ESP32（经典款，Xtensa LX6 双核），rev ≥ 3.1，8 MB Flash，PSRAM `UNKNOWN`（不得依赖） | `../../E1-5.4/sdkconfig` |
| SDK | ESP-IDF v6.1，路径 `~/esp/esp-idf-v6.1`（`IDF_PATH`）；C++ 默认 gnu++26，异常/RTTI 默认关闭 | `git describe`；`tools/cmake/build.cmake:208`；`Kconfig:560,580` |
| 工具链 | xtensa-esp-elf / riscv32-esp-elf GCC 15.2 | `~/.espressif/tools` |
| 蓝牙 | Bluedroid，仅经典蓝牙 SPP，最多 2 会话；`esp_spp_write` 深拷贝；对端地址在 `srv_open.rem_bda` | `reference/c-v1/docs/reports/T04.md`（附 IDF 文件:行号） |
| 经典蓝牙目标 | 只有 esp32 可用（esp32s31 为 preview，且无控制器库） | `soc_caps.h`；`components/bt/controller/` |
| 参考 | `../../E1-5.4`（Arduino 旧固件，只读）；`reference/c-v1/`（C 版实现，只读，不参与构建） | — |
| 板级引脚 | 已使用：UART0 console（2 Mbaud）；GNSS UART2 RX16/TX17（Kconfig `RTK_GNSS_*`，ADR-0010）。目标板 P2 v1.41：板型 ID 分压 GPIO35，电源键 GPIO4 模拟保持、GPIO25 检测；E1 测试板：GNSS 电源 GPIO25、电源保持 GPIO26（ADR-0015，`main/board/BoardProfile.cpp`）。其余 5.4 引脚见 `reference/c-v1/ADR-0001-architecture.md` §6，未经 ADR 不得驱动 | `main/Kconfig.projbuild`、`main/board/BoardProfile.cpp` |

查不到的硬件事实写 `UNKNOWN` 并停下报告，不得猜测寄存器、引脚或 API。

## 2. 语言与规范（profile：esp-idf；C++ 等级 T2）

- **规范入口**：编码规范以 `docs/CODING-STYLE.md` 为准；本文件只保留硬性门禁与授权边界。命名空间/include 根遵循 ADR-0008 §9；用途命名不放宽下述依赖隔离。
- **语言分工（ADR-0003）**：`device_proto` 的协议核心、`spp_server` 的 SPP 驱动、`nvs_store` 用 **C（C11 子集）**；`device_proto` 的 C++ 外观、`link_core`、`rtos`、`spp_server` 的 `SppTransport` 适配器、`main` 用 **C++20 子集**。不引入 Arduino、ETL 或任何 managed component。
- **C 文件规则**：布局同 C++（`.clang-format`）；公共符号/宏带组件前缀（`dev_proto_*`、`DEV_PROTO_*`、`spp_server_*`、`nvs_store_*`），文件作用域静态 `s_`，类型 `_t`；不用 `p_`/`g_`/`b_` 前缀。遵守 BARR-C 安全核心：`switch` 必有 `default`、不混用有/无符号、窄化转换写注释、变量使用前初始化、不把缓冲区强转为结构体指针、多字节字段逐字节小端读写、内部函数 `static`、不用 `malloc`/`free`、不用 `goto`。C 头文件 `#pragma once` + `extern "C"`，只含 C 构造。C 代码不调用 C++。
- **以下各条针对 C++ 文件**（C 文件只适用其中的布局、错误检查与并发条款）：
- **布局**：以仓库 `.clang-format` 为准（函数定义花括号换行，控制语句、类、命名空间花括号同行；4 空格；100 列，ADR-0013 §2.1）；头文件 `#pragma once`。门禁见 §5，不用 astyle。
- **组件命名（ADR-0008 §9）**：组件按用途命名，不带产品前缀；`spp_server`、`device_proto`、`nvs_store`、`rtos`、`link_core`、`cli` 的 C++ 命名空间/include 根分别为 `spp` / `spp/`、`proto` / `proto/`、无 C++ 接口、`rtos` / `rtos/`、`core` / `core/`、`cli` / `cli/`。`stream_demux`、`app_fw`、`bt_reconnect` 分别用 `demux` / `demux/`、`fw` / `fw/`、`reconnect` / `reconnect/`；这三个通用组件不得依赖产品组件或 IDF/FreeRTOS 头。`main` 使用 `app`，嵌套协议空间保留，不得引入顶层 `alarm`、`link`、`sync`。公共基础错误码位于 `app_fw` 的 `fw/Status.hpp`，类型与辅助函数为 `fw::Status`、`fw::isOk`、`fw::toString`；C 文件名与符号前缀取组件前缀（`dev_proto_*`、`DEV_PROTO_*`、`spp_server_*`、`SPP_SERVER_*`、`nvs_store_*`、`NVS_STORE_*`）；产品名相关的 Kconfig 选项（`RTK_*`）、NVS 命名空间 `"rtk"` 保持不变；蓝牙名格式为 `<product>-<SN>`（N1）。
- **命名（ADR-0013 §2.1，readable-cpp）**：命名空间见上条；类型 `CamelCase`；C++ 函数、方法、变量、参数、数据成员 `lowerCamelCase`（C 代码、C 接口与 `extern "C"` 仍为 `snake_case`）；私有/受保护成员后缀 `_`；常量与枚举值 `kCamelCase`；文件作用域可变静态加 `s_` 前缀并放进匿名命名空间；宏全大写。不用 `p_`/`b_` 这类类型前缀。
- **禁止**：异常、RTTI、`dynamic_cast`、`typeid`；`std::function`；`<map>`、`<list>`、`<deque>`、`<unordered_*>`；iostream；`shared_ptr`；`<thread>`/`<mutex>`；`volatile` 用于同步；位域映射寄存器；`ESP_ERROR_CHECK` 用于可恢复错误；`(void)` 丢弃 `esp_err_t`。
- **堆内存门禁**：`new`/`delete`/`malloc`/`free`、`<vector>`、`<string>` 按 `docs/CODING-STYLE.md` 的堆内存分级使用，须登记 `tools/g7b_check/heap_allowlist.txt`；placement new 不分配堆。ISR、蓝牙回调、链路任务每轮路径、发送任务禁止分配；只允许启动期一次性分配或非实时路径限量分配，并遵守失败处理要求。C 文件仍沿用上面的 `malloc`/`free` 禁令。
- **允许**：`std::array`、`std::span`、`std::optional`、`std::string_view`、`<algorithm>` 中不分配的算法、`constexpr`/`constinit`、`enum class`、RAII、模板（实例数要少）、`core::FixedStr<N>`。
- **类设计**：构造函数 `constexpr`、不失败、不访问硬件/RTOS/日志；硬件初始化放在显式的 `[[nodiscard]] init()`（CPP-010）。全局对象 `constinit`（CPP-020）。独占资源的类删除拷贝。虚函数只用于 ADR 指定的扩展点（`Transport`、`demux::Sink`、`gnss::Receiver` 驱动层次）：每个非叶子类（含 ADR 允许派生的家族类）受保护非虚析构，不写公有虚析构，不 `new`；只实例化 `final` 叶子类，实例为 `constinit` 静态对象；继承不超过两层。
- **错误**：失败类型按 `docs/CODING-STYLE.md` 规则 1–4 选择（`bool` / `fw::Status` / `fw::Result`，`esp_err_t` 只在 IDF 边界），一律 `[[nodiscard]]`；新代码不用 `const char*` 表示失败原因。IDF 调用一律检查返回值（ESP-004），可用 `esp_check.h` 宏。ISR、C 回调蹦床、析构函数、移动操作标 `noexcept`。
- **并发（ESP-001/003）**：SMP 临界区用 `portMUX_TYPE` + `taskENTER_CRITICAL(&mux)`，临界区内不调用 FreeRTOS API、不阻塞。IDF 任务栈单位是**字节**。任务一律静态创建、显式绑核，并在注释中写明理由。蓝牙回调中不阻塞、不解析、不打印数据内容。
- **类型转换**：窄化、符号变化、指针↔整数、枚举↔整数、`void *` 恢复类型的 `static_cast` 在注释中说明为什么安全，拓宽且同符号的不强制（细则见 `docs/CODING-STYLE.md` §4.5）；不用 C 风格转换（`-Wold-style-cast`）。
- **注释**：用中文，必要的英文术语保留；解释原因、约束和副作用；每个公共函数注明线程/ISR 安全性。不注释掉代码。
- **调试输出（ADR-0009）**：分三条通道，不得混用。
  - **实时路径**（链路任务、发送任务、蓝牙回调、ISR、执行器每轮）：只用 `fw::trace()`（C 驱动经 `spp_server_set_trace_hook`），不格式化、不产出文本；禁止新增 `ESP_LOG`/`printf`。
  - **组件/模块日志**：C++ 用 `fw/Log.hpp` 的 `FW_LOGE/W/I/D`，C 用 `fw/LogC.h` 的 `FW_CLOG*`；每个组件可在私有头定义一行自己的宏（如 `GNSS_LOGW`）；级别与内容规则见 CODING-STYLE §5.1、ADR-0009 §2.12。每类别日志级别默认 I，与 trace 开关（`debug trace`）分开设置（`debug log`）。`fw::tlog` 只保留给已有调用点，新代码不用。**禁止 `ESP_LOG*`**（含启动代码），只允许 `esp_log_level_set`/`esp_log_set_vprintf` 控制 IDF 自身日志；例外：`test_apps/rtk_spp_peer`（对端测试固件，不链接 `app_fw`）可用 `ESP_LOG*`。
  - **控制台输出**：一律经 `cli::outPrintf`/`cli::outWrite`（ADR-0009 §2.8），不直接 `printf`/`puts`；命令最后一行固定 `OK` 或 `ERR …`（`cli::fail`），子命令用 `cli::Verb` + `cli::dispatch`。
- **溯源**：按 `docs/CODING-STYLE.md`，源文件第一行说明用途，第二行起说明线程/调用约束（谁调用、可否阻塞、是否 ISR 安全）；使用堆的文件另注明档位。文件头不写 `Generated-by` 或人工负责人；生成来源由提交者记录在 git 提交 trailer：`Generated-by: pi + <模型>`（模型以任务卡为准）。不得添加 `Signed-off-by`；该规则不授予 git commit/push 权限。

## 3. 组件与依赖

| 组件 | 可 REQUIRES | 禁止 |
| --- | --- | --- |
| `components/stream_demux`（通用 C++20，ADR-0005） | 无 | 任何 IDF/FreeRTOS/平台头、产品组件 |
| `components/device_proto`（C 核心 + C++ 外观） | stream_demux（ADR-0005，仅 `DemuxFrame.cpp`）、app_fw（C++ 基础错误码） | 任何 IDF/FreeRTOS 头 |
| `components/app_fw`（通用 C++20，ADR-0007） | 无 | 任何 IDF/FreeRTOS/平台头、产品组件 |
| `components/bt_reconnect`（通用 C++20，ADR-0007 §5） | 无 | 任何 IDF/FreeRTOS/平台头、产品组件 |
| `components/link_core` | device_proto、app_fw | 任何 IDF/FreeRTOS 头 |
| `components/rtos` | device_proto、app_fw、freertos、esp_timer、log、esp_system（看门狗） | link_core 以上 |
| `components/gnss`（通用 C++20，ADR-0012） | stream_demux、app_fw | 任何 IDF/FreeRTOS/平台头、main |
| `components/web_proto`（通用 C++20，ADR-0017 §2.5） | app_fw | 任何 IDF/FreeRTOS/平台头、main |
| `components/settings`（通用 C++20，ADR-0018 §2.3） | app_fw | 任何 IDF/FreeRTOS/平台头、main |
| `components/cli`（ADR-0007） | console、esp_system、driver/串口相关 IDF 组件、app_fw（框架基座） | app_fw 以外的自有组件（保持可复用） |
| `components/spp_server`（C 驱动 + C++ 适配器） | link_core、rtos、app_fw、bt | nvs_store、main；C 驱动文件不得包含任何 C++ 头 |
| `components/nvs_store`（C） | nvs_flash、log、app_fw（框架基座，仅 `fw/LogC.h`） | app_fw 以外的自有组件 |
| `main` | 以上全部、console、esp_system | — |

**框架基座（Q2 决定）**：`app_fw` 是框架基座，任何自有组件都可以 REQUIRES 它；`app_fw` 自身不依赖任何组件。表中“禁止其他自有组件”一律不含 app_fw。

纯组件（stream_demux、device_proto、link_core、app_fw、bt_reconnect、gnss、web_proto、settings）必须能在 host 上用 g++ 编译并有 host 测试。C/C++ 文件名一律 `PascalCase`（CODING-STYLE §3.1）；改名须同步所有 host 测试 CMake。
ADR-0007 新增的纯组件（app_fw、bt_reconnect）与 link_core 的 `services/`、`command_router` 按 `stream_demux` 的 CMakeLists 模式（`if(ESP_PLATFORM)` 分支）同时支持 IDF 与 host，各自的 host 测试放在 `test/app_fw`、`test/bt_reconnect`、`test/services`（由实现方新建，不冻结；须支持普通与 ASan/UBSan 构建）。

自有组件的 `CMakeLists.txt` 加编译选项（C 文件用 `$<$<COMPILE_LANGUAGE:C>:...>` 区分）：
- 纯 C（device_proto 的 .c）：`-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wimplicit-fallthrough -Wswitch-default -Wundef -Wstrict-prototypes -Wmissing-prototypes`
- IDF 依赖的 C（spp_server、nvs_store 的 .c）：`-Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes`
- 纯组件：`-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef`
- IDF 依赖组件与 main：`-Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough`。IDF 宏会触发 `-Wold-style-cast`、`-Wconversion`、`-Wundef`，这三项不开，并在 CMakeLists 注释中写明原因。
- **调试阶段告警即错误**：每个自有组件、`main`、`test_apps/*/main` 的 CMakeLists 在上述选项之后调用 `rtk_strict_warnings(<target>)`（`cmake/strict_warnings.cmake`，开关 `CONFIG_RTK_STRICT_WARNINGS`，默认 y）。它把 IDF 默认降级的 `unused-function/unused-variable/unused-but-set-variable/deprecated-declarations/extra` 改回错误，并重新打开 `-Wunused-parameter`、`-Wsign-compare`。新增组件必须调用它。`.clangd` 的 ESP 段追加同一组选项，`.clang-tidy`/`.clang-tidy-c` 为 `WarningsAsErrors: '*'`，LSP 中出现的告警与构建同等对待。不得用 `-Wno-error`、`#pragma GCC diagnostic` 或关掉该开关来通过构建；确需豁免时用具名 `NOLINT(<check>)` 或 `[[maybe_unused]]` 并写明理由。

## 4. 冻结输入（不得修改）

由 Architect/QA 编写，实现方**一个字节都不能改**；发现矛盾或无法实现时停下，在报告中写明证据：

- `AGENTS.md`、`sdkconfig.defaults`、`partitions.csv`、`.clang-format`、`.clang-tidy`、`.clang-tidy-c`、`.editorconfig`、`.gitignore`
- `docs/**`（`docs/reports/` 除外）
- `components/*/include/**`（公共头即接口契约）。唯一例外：实现方可以在 `components/rtos/include/rtos/` 下**新增** RAII 包装头；已有头仍然冻结
- 组件私有头放在组件的 `src/` 或 `private_include/`，不冻结；`main/*.hpp` 不冻结
- `test/host/**`
- `reference/**`

校验：`sha256sum -c docs/frozen.sha256`。`docs/frozen.sha256` 只由 Architect（Claude）重算；实现方与审计方（包括允许修改文档的审计任务卡）一律不改，冻结文件有变化时在报告中列出，由 Architect 验收后重算。

## 5. 命令

```sh
# 系统 Python 为 3.14，需指定 IDF 自带的 3.13 环境，否则 export.sh 失败
export IDF_PATH=~/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.1_py3.13_env
. "$IDF_PATH"/export.sh >/dev/null                         # 每个新 shell 先执行
idf.py -B builds/<任务> build                               # 0 warning；另用 -DSDKCONFIG_DEFAULTS 加 CONFIG_RTK_TRACE_ENABLE=n 再编一份
idf.py -B builds/<任务> size
# host 测试：以下每个目录都要普通构建 + ASan/UBSan 构建各跑一次
#   test/host test/app_fw test/services test/stream_demux test/bt_reconnect test/cli test/gnss
#   test/nvs_store tools/main_host_check tools/t13_host_check
#   test/gnss 另做全驱动构建：-DRTK_GNSS_DRV_COMNAV=ON -DRTK_GNSS_DRV_UBLOX=ON
cmake -S <目录> -B builds/<任务>-<名> -G Ninja && cmake --build builds/<任务>-<名> && ctest --test-dir builds/<任务>-<名> --output-on-failure
cmake -S <目录> -B builds/<任务>-<名>-asan -G Ninja \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer -g" \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"   # 无 RTTI，须关 vptr 检查
python3 -I tools/g7b_check/g7b_check.py --build builds/<任务>   # 堆分配白名单
sha256sum -c docs/frozen.sha256                              # 冻结输入未被改动
clang-format --style=file --dry-run --Werror <改动的 C/C++ 文件>
python3 -I tools/doc_check/doc_check.py --base <任务基线提交>
```

合并到 `main` 前由 Architect 运行 `tools/ci/run_gates.sh --base <分支基线>`（本地 CI，一次跑完上述全部门禁，日志在 `builds/ci-logs/`）。

构建目录约定：所有构建（idf 主工程、host、ASan、测试工程）一律放 `builds/<名称>/`，不得在仓库根或源码目录下新建 `build*`。

函数注释以 `docs/CODING-STYLE.md` §6.2 为准：每个函数（含 static、私有方法）写 `/** @brief ... @param name: ... @retval ... */`，标签英文、正文中文。门禁 `python3 -I tools/doc_check/doc_check.py --base <任务基线提交>`：本次新增或改动的函数必须全部合格（该工具用本机缓存的 lizard，不联网）。

格式以 `clang-format --style=file`（仓库 `.clang-format`）为唯一门禁（用户 2026-11-13 决定）。IDF 的 astyle 规则与 `.clang-format` 冲突（续行缩进、`T {x}` 空格），**不得用 astyle 改写本仓库文件**，仅可作参考。

## 6. 授权边界

- 允许：在 `RTK_main/` 内创建或修改非冻结文件、本地构建、host 测试、静态检查。
- **禁止**：`idf.py flash`、`monitor`、`erase-flash`、任何 `efuse` 命令、连接串口设备；修改 `../../E1-5.4`、`../ExternalSpeak`、`reference/`；安装软件包或联网下载（包括 `uvx`、`pip`、`idf.py add-dependency`、组件管理器拉取）；git commit/push。
- 构建失败时不得用关闭告警、删除或跳过测试、修改冻结文件的方式“修复”。

## 7. 报告

每个任务结束时写 `docs/reports/<任务号>.md`：改动文件、实际执行的命令和结果（逐项 `PASS/FAIL/NOT_RUN/BLOCKED`）、与契约的偏离、未解决问题。
