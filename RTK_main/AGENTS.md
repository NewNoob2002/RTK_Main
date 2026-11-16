# RTK_main 工程规则（给实现 Agent）

本文件是本目录下所有代码改动的项目级规则，优先级高于通用默认值。架构见 `docs/adr/ADR-0002-cpp-esp-idf.md` 与 **`docs/adr/ADR-0003-c-cpp-split.md`（C/C++ 分工，优先于 ADR-0002）**。字节流分流器见 `docs/adr/ADR-0004-stream-demux.md`（语义）与 **`docs/adr/ADR-0005-stream-demux-cpp.md`（C++ 实现、组件命名，优先于 ADR-0004）**。应用层框架（模块、执行器、指令路由、服务、控制台、重连）见 **`docs/adr/ADR-0007-app-services-split.md`**，与旧描述冲突时以它为准。

## 1. 已核实的事实

| 项 | 值 | 来源 |
| --- | --- | --- |
| 芯片 | ESP32（经典款，Xtensa LX6 双核），rev ≥ 3.1，8 MB Flash，PSRAM `UNKNOWN`（不得依赖） | `../../E1-5.4/sdkconfig` |
| SDK | ESP-IDF v6.1，路径 `~/esp/esp-idf`；C++ 默认 gnu++26，异常/RTTI 默认关闭 | `git describe`；`tools/cmake/build.cmake:208`；`Kconfig:560,580` |
| 工具链 | xtensa-esp-elf / riscv32-esp-elf GCC 15.2 | `~/.espressif/tools` |
| 蓝牙 | Bluedroid，仅经典蓝牙 SPP，最多 2 会话；`esp_spp_write` 深拷贝；对端地址在 `srv_open.rem_bda` | `reference/c-v1/docs/reports/T04.md`（附 IDF 文件:行号） |
| 经典蓝牙目标 | 只有 esp32 可用（esp32s31 为 preview，且无控制器库） | `soc_caps.h`；`components/bt/controller/` |
| 参考 | `../../E1-5.4`（Arduino 旧固件，只读）；`reference/c-v1/`（C 版实现，只读，不参与构建） | — |
| 板级引脚 | 本阶段只用 UART0（console）。其余 5.4 引脚见 `reference/c-v1/ADR-0001-architecture.md` §6，不得驱动 | — |

查不到的硬件事实写 `UNKNOWN` 并停下报告，不得猜测寄存器、引脚或 API。

## 2. 语言与规范（profile：esp-idf；C++ 等级 T2）

- **规范入口**：编码规范以 `docs/CODING-STYLE.md` 为准；本文件只保留硬性门禁与授权边界。命名空间/include 根遵循 ADR-0008 §9；用途命名不放宽下述依赖隔离。
- **语言分工（ADR-0003）**：`device_proto` 的协议核心、`spp_server` 的 SPP 驱动、`nvs_store` 用 **C（C11 子集）**；`device_proto` 的 C++ 外观、`link_core`、`rtos`、`spp_server` 的 `SppTransport` 适配器、`main` 用 **C++20 子集**。不引入 Arduino、ETL 或任何 managed component。
- **C 文件规则**：布局同下（ESP-006）；公共符号/宏带组件前缀（`dev_proto_*`、`DEV_PROTO_*`、`spp_server_*`、`nvs_store_*`），文件作用域静态 `s_`，类型 `_t`；不用 `p_`/`g_`/`b_` 前缀。遵守 BARR-C 安全核心：`switch` 必有 `default`、不混用有/无符号、窄化转换写注释、变量使用前初始化、不把缓冲区强转为结构体指针、多字节字段逐字节小端读写、内部函数 `static`、不用 `malloc`/`free`、不用 `goto`。C 头文件 `#pragma once` + `extern "C"`，只含 C 构造。C 代码不调用 C++。
- **以下各条针对 C++ 文件**（C 文件只适用其中的布局、错误检查与并发条款）：
- **布局（ESP-006）**：ESP-IDF Astyle 风格：函数定义花括号换行，控制语句、类、命名空间花括号同行；4 空格；120 列；`#pragma once`。日常用 `clang-format --style=file` 检查（`.clang-format` 是近似规则）。
- **组件命名（ADR-0008 §9）**：组件按用途命名，不带产品前缀；`spp_server`、`device_proto`、`nvs_store`、`rtos`、`link_core`、`cli` 的 C++ 命名空间/include 根分别为 `spp` / `spp/`、`proto` / `proto/`、无 C++ 接口、`rtos` / `rtos/`、`core` / `core/`、`cli` / `cli/`。`stream_demux`、`app_fw`、`bt_reconnect` 分别用 `demux` / `demux/`、`fw` / `fw/`、`reconnect` / `reconnect/`；这三个通用组件不得依赖产品组件或 IDF/FreeRTOS 头。`main` 使用 `app`，嵌套协议空间保留，不得引入顶层 `alarm`、`link`、`sync`。公共基础错误码位于 `app_fw` 的 `fw/Status.hpp`，类型与辅助函数为 `fw::Status`、`fw::is_ok`、`fw::to_string`；C 文件名与符号前缀取组件前缀（`dev_proto_*`、`DEV_PROTO_*`、`spp_server_*`、`SPP_SERVER_*`、`nvs_store_*`、`NVS_STORE_*`）；产品名相关的 Kconfig 选项（`RTK_*`）、NVS 命名空间 `"rtk"` 与蓝牙名保持不变。
- **命名（cpp/coding.md）**：命名空间见上条；类型 `CamelCase`；函数、变量、参数 `lower_case`；私有成员后缀 `_`；常量与枚举值 `kCamelCase`；文件作用域可变静态加 `s_` 前缀并放进匿名命名空间；宏全大写。不用 `p_`/`b_` 这类类型前缀。
- **禁止**：异常、RTTI、`dynamic_cast`、`typeid`；`std::function`；`<map>`、`<list>`、`<deque>`、`<unordered_*>`；iostream；`shared_ptr`；`<thread>`/`<mutex>`；`volatile` 用于同步；位域映射寄存器；`ESP_ERROR_CHECK` 用于可恢复错误；`(void)` 丢弃 `esp_err_t`。
- **堆内存门禁**：`new`/`delete`/`malloc`/`free`、`<vector>`、`<string>` 按 `docs/CODING-STYLE.md` 的堆内存分级使用，须登记 `tools/g7b_check/heap_allowlist.txt`；placement new 不分配堆。ISR、蓝牙回调、链路任务每轮路径、发送任务禁止分配；只允许启动期一次性分配或非实时路径限量分配，并遵守失败处理要求。C 文件仍沿用上面的 `malloc`/`free` 禁令。
- **允许**：`std::array`、`std::span`、`std::optional`、`std::string_view`、`<algorithm>` 中不分配的算法、`constexpr`/`constinit`、`enum class`、RAII、模板（实例数要少）、`core::FixedStr<N>`。
- **类设计**：构造函数 `constexpr`、不失败、不访问硬件/RTOS/日志；硬件初始化放在显式的 `[[nodiscard]] fw::Status init()`（CPP-010）。全局对象 `constinit`（CPP-020）。独占资源的类删除拷贝。多态只用于 `Transport` 与 `demux::Sink`（ADR-0005）：受保护非虚析构 + `final`（CPP-011）。
- **错误**：可恢复错误返回 `[[nodiscard]] fw::Status`。`esp_err_t` 只在 `rtos::from_esp_err` 映射。IDF 调用一律检查返回值（ESP-004），可用 `esp_check.h` 宏。ISR、C 回调蹦床、析构函数、移动操作标 `noexcept`。
- **并发（ESP-001/003）**：SMP 临界区用 `portMUX_TYPE` + `taskENTER_CRITICAL(&mux)`，临界区内不调用 FreeRTOS API、不阻塞。IDF 任务栈单位是**字节**。任务一律静态创建、显式绑核，并在注释中写明理由。蓝牙回调中不阻塞、不解析、不打印数据内容。
- **窄化转换**：每处 `static_cast` 都在注释中说明为什么数值范围安全；不用 C 风格转换（`-Wold-style-cast`）。
- **注释**：用中文，必要的英文术语保留；解释原因、约束和副作用；每个公共函数注明线程/ISR 安全性。不注释掉代码。
- **调试输出（ADR-0009）**：链路任务、发送任务、蓝牙回调中禁止新增 `ESP_LOG`/`printf`；运行时诊断一律经 `fw::trace()`（C 驱动经 `spp_server_set_trace_hook`），由 main 的 DebugModule 低优先级任务格式化输出。控制台命令最后一行固定 `OK` 或 `ERR …`（`cli::fail`），子命令用 `cli::Verb` + `cli::dispatch`。控制台文本输出一律经 `cli::out_printf`/`cli::out_write`（ADR-0009 §2.8，整行写 UART），不直接 `printf`/`puts`。非实时任务（如 io 执行器里的 GNSS 模块）的人类可读诊断文本走 `fw::tlog`/`fw::tlog_hex`，句子格式见 ADR-0009 §2.11；实时路径（链路任务、发送任务、蓝牙回调）只用 `fw::trace`，不产出文本。
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
| `components/cli`（ADR-0007） | console、esp_system、driver/串口相关 IDF 组件 | 其他自有组件（保持报警器可复用） |
| `components/spp_server`（C 驱动 + C++ 适配器） | link_core、rtos、app_fw、bt | nvs_store、main；C 驱动文件不得包含任何 C++ 头 |
| `components/nvs_store`（C） | nvs_flash、log | 其他自有组件 |
| `main` | 以上全部、console、esp_system | — |

纯组件（stream_demux、device_proto、link_core）必须能被 `test/host/CMakeLists.txt` 在 host 上用 g++ 编译。文件名已在该 CMake 中固定，不得改名；C/C++ 文件名一律 `PascalCase`（CODING-STYLE §3.1）。
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

校验：`sha256sum -c docs/frozen.sha256`。

## 5. 命令

```sh
. ~/esp/esp-idf/export.sh >/dev/null        # 每个新 shell 先执行
idf.py set-target esp32 && idf.py build     # 首次；之后 idf.py build
idf.py size && idf.py size-components
cmake -S test/host -B builds/host && cmake --build builds/host && ctest --test-dir builds/host --output-on-failure
cmake -S test/host -B builds/asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  && cmake --build builds/asan && ctest --test-dir builds/asan --output-on-failure
clang-format --style=file --dry-run --Werror <改动的 .cpp/.hpp>
clang-tidy -p builds/host <device_proto、link_core 的 .cpp>      # 只对纯组件
clang-tidy -p builds/host --config-file=.clang-tidy-c <device_proto 的 .c>
```

构建目录约定：`idf.py` 主工程用默认 `build/`；其余所有构建（host、ASan、测试工程、并行任务）一律放 `builds/<名称>/`，不得在仓库根或源码目录下新建 `build-*`。

函数注释以 `docs/CODING-STYLE.md` §6.2 为准：每个函数（含 static、私有方法）写 `/** @brief ... @param name: ... @retval ... */`，标签英文、正文中文。门禁 `python3 -I tools/doc_check/doc_check.py --base <任务基线提交>`：本次新增或改动的函数必须全部合格（该工具用本机缓存的 lizard，不联网）。

格式以 `clang-format --style=file`（仓库 `.clang-format`）为唯一门禁（用户 2026-11-13 决定）。IDF 的 astyle 规则与 `.clang-format` 冲突（续行缩进、`T {x}` 空格），**不得用 astyle 改写本仓库文件**，仅可作参考。

## 6. 授权边界

- 允许：在 `RTK_main/` 内创建或修改非冻结文件、本地构建、host 测试、静态检查。
- **禁止**：`idf.py flash`、`monitor`、`erase-flash`、任何 `efuse` 命令、连接串口设备；修改 `../../E1-5.4`、`../ExternalSpeak`、`reference/`；安装软件包或联网下载（包括 `uvx`、`pip`、`idf.py add-dependency`、组件管理器拉取）；git commit/push。
- 构建失败时不得用关闭告警、删除或跳过测试、修改冻结文件的方式“修复”。

## 7. 报告

每个任务结束时写 `docs/reports/<任务号>.md`：改动文件、实际执行的命令和结果（逐项 `PASS/FAIL/NOT_RUN/BLOCKED`）、与契约的偏离、未解决问题。
