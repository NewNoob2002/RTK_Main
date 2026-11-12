# ADR-0002 C++20 + esp-idf profile 重构（取代 ADR-0001 的语言、规范与分层部分）

- 状态：**Approved 2026-11-10；§3–§5 的语言分工部分被 ADR-0003 修订**（协议核心、SPP 驱动、NVS 改为 C）。用户选择“Full C++ port”与“astyle_py via uvx”。
- Profile：`esp-idf`（取代 `mcu-rtos`）。依据：`sdkconfig.defaults`、`idf_component_register`、`project.cmake`。
- 评审深度：Delta。产品范围、协议、链路策略、任务预算沿用 ADR-0001（`reference/c-v1/ADR-0001-architecture.md`）。本 ADR 只改语言、规范、组件划分和测试方式。

## 1. 变更原因

| 原因 | 说明 |
| --- | --- |
| 协议复用 | 报警器固件同样基于 ESP32，可直接链接 `rtk_proto`（帧编解码与 0x40/0x41 编解码），两端共用一份经测试的实现 |
| 抽象 | `Transport` 接口让 `LinkService` 与 SPP 解耦，将来 BLE/UART/ESP-NOW 通道可以复用同一套命令分发；FreeRTOS、NVS、蓝牙回调用 RAII 和静态蹦床封装 |
| 正确性 | C 版的越界读（QA-A 发现 1）属于“手工下标拼装”这类错误。C++ 版强制经 `ByteWriter`/`ByteReader` 做有界读写，`FixedStr<N>` 表达定宽字段，从类型层面排除这类错误 |
| 可测性 | C 版 app_link.c 中“事件→解析→角色→分发→写回”的胶水逻辑没有任何测试；C++ 版移入纯逻辑 `LinkService`，用假传输在 host 上测试 |
| 规范 | 本地技能已新增 `esp-idf` profile：ESP-IDF Style Guide 取代 BARR-C 的格式/命名层 |

## 2. 事实与核实

| 项 | 内容 | 来源 |
| --- | --- | --- |
| C++ 方言 | IDF v6.1 默认 `-std=gnu++26` | `tools/cmake/build.cmake:208` |
| 异常 / RTTI | 默认关闭 | `Kconfig:560`、`Kconfig:580`（`default n`）；`sdkconfig.defaults` 中显式固定 |
| 工具链 | xtensa-esp-elf GCC 15.2（`esp-15.2.0_20251204`） | `~/.espressif/tools` |
| 经典蓝牙目标 | `SOC_BT_CLASSIC_SUPPORTED`：esp32、esp32s31。esp32s31 在 v6.1 为 preview（`IDF_ENV_BRINGUP`），且源码树中没有 `lib_esp32s31` 控制器库 | `components/soc/*/soc_caps.h`；`components/bt/controller/` |
| Astyle | IDF 官方规则：`--style=otbs --attach-namespaces --attach-classes --indent=spaces=4 --pad-header --pad-oper --unpad-paren --max-continuation-indent=120`，astyle_py 3.4.7 | `tools/ci/astyle-rules.yml`、`.pre-commit-config.yaml:212-217` |
| SPP 行为 | 写入时深拷贝；最多 2 个会话；对端地址在 `srv_open.rem_bda` | `reference/c-v1/docs/reports/T04.md`（附 IDF 文件:行号） |

## 3. 资源等级与 C++ 特性（cpp/features.md）

ESP32：8 MB Flash，520 KB SRAM，双核，归入 **T2**。本工程自有代码比 T2 更严格：

| 规则 | 本工程取值 |
| --- | --- |
| 标准 | C++20 子集（IDF 以 gnu++26 编译，不使用 C++23 及以后才有的库特性） |
| 异常 / RTTI | 禁用（CPP-001/002） |
| 堆 | 按 `docs/CODING-STYLE.md` 分级：ISR、蓝牙回调、链路任务每轮路径、发送任务禁止分配；启动期一次性分配允许；非实时路径允许限量分配并处理失败。使用堆的源文件须登记 G7b 白名单且文件头标档位；C 文件保留 `malloc`/`free` 禁令，placement new 不分配堆。IDF 蓝牙栈自身用堆，不在自有代码约束内。**2026-11-12 由 ADR-0008 §8 修订** |
| 容器 | 优先用 `std::array`、`std::span`、`std::optional`、`std::string_view`、`FixedStr<N>`；`std::vector`/`std::string` 仅按上述堆分级与白名单使用（关闭异常时分配失败会 abort；`reserve` 自身也可能失败，需恢复时使用 nothrow 存储）。不引入 ETL（需要联网下载组件，且本阶段不需要）；2026-11-12 随 ADR-0008 §8 同步 |
| 多态 | 只有 `Transport` 一处运行期边界：受保护非虚析构 + `final` 实现（CPP-011） |
| 全局对象 | `constinit` 或 `constexpr` 构造（CPP-020）；构造函数不访问硬件、RTOS 或日志（CPP-010） |
| 函数局部静态 | 只允许常量初始化的 `static constinit`，不生成 guard（CPP-009） |
| 禁止 | `std::function`、`<map>`/`<list>`/`<deque>`/`<unordered_*>`、iostream、`shared_ptr`、`<thread>`/`<mutex>`；`<vector>`/`<string>` 改按上述堆分级使用（2026-11-12 由 ADR-0008 §8 修订） |

## 4. 组件划分

| 组件 | 依赖 | 内容 | host 可测 |
| --- | --- | --- | --- |
| `rtk_proto` | 无（标准库） | `Status`、`ByteWriter/Reader`、帧编解码、报警消息编解码。**供报警器固件复用** | 是 |
| `rtk_core` | rtk_proto | `LinkManager`、`AlarmSource`、`CommandDispatcher`、`LinkService`、`Transport` 接口 | 是 |
| `rtk_os` | freertos、esp_timer | `esp_err_t→Status` 映射、`now_ms()`、静态任务/队列/StreamBuffer/事件组/portMUX 守卫等 RAII 包装 | 否 |
| `rtk_bt` | bt、rtk_core、rtk_os | `SppServer final : Transport`（仅 ESP32） | 否 |
| `rtk_store` | nvs_flash、rtk_core、rtk_os | `SettingsStore`（SN、白名单） | 否 |
| `main` | 以上全部、console | 组合根 `app_main.cpp`（`extern "C"`）、链路任务、控制台 | 否 |

依赖方向：`main → rtk_bt/rtk_store → rtk_os → IDF`；`rtk_bt/rtk_store → rtk_core → rtk_proto`。`rtk_proto`、`rtk_core` 不得包含任何 IDF 或 FreeRTOS 头文件（门禁 G6）。

## 5. 编码规范（ESP-006 + cpp/coding.md）

- 布局：ESP-IDF Astyle（otbs；函数定义花括号换行，控制语句、类、命名空间花括号同行；4 空格；120 列）。日常检查用 `.clang-format`（近似规则），**权威门禁是 astyle_py 3.4.7 + IDF 规则文件**。
- 头文件：`#pragma once`（ESP-006）。纯 C++ 头不加 `extern "C"`；只有 `app_main` 与 SDK 回调蹦床使用 `extern "C"`/静态函数。
- 命名：命名空间 `rtk::<组件>`；类型 `CamelCase`；函数、变量 `lower_case`；私有成员后缀 `_`；常量 `kCamelCase`；文件作用域可变静态 `s_`（ESP-006），尽量放在匿名命名空间中；宏全大写。
- 错误：可恢复错误返回 `[[nodiscard]] rtk::Status`。`esp_err_t` 只在 `rtk_os::from_esp_err` 映射。IDF 调用用 `ESP_RETURN_ON_ERROR` 等宏传播；`ESP_ERROR_CHECK` 只用于不可恢复的启动错误（ESP-004）。
- 并发：链路任务独占 `LinkService`、`LinkManager`、`CommandDispatcher`、`AlarmSource`；控制台通过命令队列请求。SMP 临界区用 `portMUX_TYPE` 守卫（ESP-001），不用 `volatile` 同步。
- BARR-C 安全核心中不冲突的条目仍然适用：每个 `switch` 有 `default`、比较不混用有符号与无符号、窄化转换加注释说明范围、变量使用前初始化。

## 6. 测试与门禁

| 层 | 方式 |
| --- | --- |
| 纯逻辑 | `test/host`：Unity（IDF 自带副本）+ C++20 + `-fno-exceptions -fno-rtti` + 严格告警，另跑一遍 ASan/UBSan。74 个用例在实现前冻结 |
| 双架构编译（ESP-008） | 完整固件只能构建 `esp32`（经典蓝牙）。`test_apps/rtk_pure` 只链接 `rtk_proto`、`rtk_core`，分别为 `esp32`（Xtensa）和 `esp32c3`（RISC-V，非 preview）构建，作为 RISC-V 门禁 |
| 上板 | `test_apps/rtk_pure` 用 pytest-embedded 运行，固件做 HIL；需要开发板，未授权时记 `NOT_RUN` |
| C++ 运行时 | `tools/g7b_check/g7b_check.py`：自有目标文件中异常、RTTI 符号为 0；堆符号按 `tools/g7b_check/heap_allowlist.txt` 的精确源文件路径分级放行并列出，未登记则 FAIL（**2026-11-12 由 ADR-0008 §8 修订**，规范见 `docs/CODING-STYLE.md`）。全 ELF 的异常/RTTI 符号仍只允许 IDF NVS 经 libstdc++ 引入的 6 个白名单符号且拉入链固定（2026-11-11 修订，见 `docs/acceptance.md` G7b）；`cpp_cost.py check --tier T2` 只报告；IDF 蓝牙栈的 heap 类只报告 |

## 7. 对 ESP-008 的偏离

偏离：产品固件依赖经典蓝牙，只能构建 `esp32`。缓解：可移植的纯逻辑经 `test_apps/rtk_pure` 在 RISC-V 目标上构建；`rtk_bt` 以 `SOC_BT_CLASSIC_SUPPORTED` 作编译期守卫，非经典蓝牙目标给出明确的 `#error`。esp32s31 正式支持经典蓝牙控制器后重新评估。

## 8. 迁移

C 版实现（BATCH-A 已通过独立验收；BATCH-B 只有实现方自报的结果）移入 `reference/c-v1/`，只读，不参与构建。移植时以 C 版的行为和 IDF 核实结论为参考，以冻结的 C++ 头文件和测试为准。
