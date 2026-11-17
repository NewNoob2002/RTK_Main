# ADR-0003 C/C++ 按特性分工（修订 ADR-0002 §3、§4、§5）

- 状态：**Approved 2026-11-10**。用户要求“不需要所有部分都用 C++，按 C/C++ 的特性规划”，并批准下述分工；组件名保留 `rtk_` 前缀（方案 A）。
- Profile：`esp-idf`（不变）。评审深度：Delta。协议、链路策略、任务预算、测试门禁沿用 ADR-0001/0002。

## 1. 分工原则

| 用 C | 用 C++ |
| --- | --- |
| 字节级编解码，没有对象状态或只有一个 POD 状态 | 有状态、有不变量的对象（会话表、分发器、服务） |
| 直接对接 ESP-IDF 的 C API 与 C 回调（Bluedroid、NVS、驱动） | 运行期多态边界（`Transport`） |
| 需要被报警器固件复用（`ExternalSpeak` 是纯 C 工程） | RAII 管理 FreeRTOS 资源、作用域锁 |
| 未来的板级驱动（GNSS UART、电台、LED、电池 I2C），与 IDF 示例和驱动 API 风格一致 | 组合根：对象装配、任务、控制台 |

C 与 C++ 的边界只有一种形式：C 头文件（`#pragma once` + `extern "C"`，只含 C 构造）被 C++ 包含；C++ 侧用薄适配器把 `esp_err_t`/C 结构映射为 `rtk::Status`/C++ 类型（cpp/coding.md §8，CPP-013/014）。C 代码不调用 C++。

## 2. 组件分工

| 组件 | 语言 | 公共接口 | 说明 |
| --- | --- | --- | --- |
| `rtk_proto` | **C 核心 + C++ 外观** | C：`rtk_proto_frame.h`、`rtk_proto_alarm.h`；C++：`rtk/proto/Frame.hpp`、`AlarmCodec.hpp`（委托给 C）、`Bytes.hpp`、`Status.hpp` | 协议逻辑只实现一次（C），两端共用。C 实现由 `reference/c-v1` 中经 QA-A 验收的 `proto_frame.c`/`alarm_msg.c` 移植（报警状态源已在 C++ `AlarmSource` 中，不再移植） |
| `rtk_core` | C++ | 不变（ADR-0002） | LinkManager、AlarmSource、CommandDispatcher、LinkService、Transport。BATCH-C 已通过验收，**不改代码** |
| `rtk_os` | C++ | `EspStatus.hpp` + 实现方在 T13 起草的 RAII 头 | 只供 C++ 代码使用（main、rtk_bt 适配器） |
| `rtk_bt` | **C 驱动 + C++ 适配器** | C：`rtk_bt_spp.h`；C++：`rtk/bt/SppTransport.hpp`（`SppTransport final : Transport`） | C 驱动由 `reference/c-v1/components/bt_spp/bt_spp.c` 移植（PIN 改为 1234）。适配器无状态，只转调与映射错误 |
| `rtk_store` | **C** | `rtk_store.h` | 由 `reference/c-v1/components/store/store_nvs.c` 移植。main 直接调用，`BtAddr` ↔ `uint8_t[6]` 的转换在 main 中完成 |
| `main` | C++ | — | 组合根、链路任务、控制台（ADR-0002 的 T14 要求不变） |
| 未来：`rtk_gnss`、`rtk_radio`、`rtk_power`、`rtk_led` | C 驱动（需要状态机/聚合时上层用 C++） | 预留 | 不在本阶段 |

作废：ADR-0002 中的 `rtk/bt/spp_server.hpp`、`rtk/store/settings_store.hpp` 及 DeepSeek 的 C++ 草稿，移入 `reference/cpp-v2-draft/`。

## 3. 规范

| 项 | C（`.c`/`.h`） | C++（`.cpp`/`.hpp`） |
| --- | --- | --- |
| 方言 | C11 子集（host 测试 `-std=c11`；目标由 IDF 以 gnu23 编译，不使用 C23 特性） | C++20 子集（不变） |
| 布局 | ESP-IDF Style（ESP-006）：otbs、4 空格、120 列、`#pragma once`；`.clang-format` 同一份 | 同左 |
| 命名 | 公共符号与宏带组件前缀：`rtk_proto_*`/`RTK_PROTO_*`、`rtk_bt_spp_*`、`rtk_store_*`；文件作用域静态 `s_`；类型 `_t` 结尾；**不用** BARR-C 的 `p_`/`g_`/`b_` 前缀 | cpp/coding.md（不变） |
| 安全规则 | BARR-C 安全核心：每个 `switch` 有 `default`；不混用有符号与无符号；窄化转换写注释；变量使用前初始化；不对缓冲区强转结构体指针；多字节字段逐字节小端读写；有界循环；文件内函数 `static` | CPP-001…020 |
| 内存 | 不用 `malloc`/`free`；状态放静态存储或调用方提供 | 不变（无堆） |
| 错误 | 纯 C 模块用本模块枚举；IDF 相关模块返回 `esp_err_t`，用 `ESP_RETURN_ON_ERROR` 等传播（ESP-004） | `rtk::Status`，`esp_err_t` 只在 `rtk::os::fromEspErr` 映射 |
| 静态分析 | `clang-tidy --config-file=.clang-tidy-c`（无 C++ 命名规则） | `.clang-tidy`（不变） |

## 4. 测试

- `test/host` 增加 `rtk_proto_c` 静态库与两个 C 测试（`test_proto_frame_c`、`test_proto_alarm_c`，19 个用例，只链接 C 库，证明报警器可单独使用 C 接口）。C++ 测试 74 个不变，C++ 外观经同一组测试验证。合计 8 个可执行文件，93 个用例。
- `test_apps/rtk_pure`（T15）同时编译 C 与 C++ 两部分，esp32 与 esp32c3 双架构。

## 5. 对已验收代码的影响

- `rtk_core/*.cpp`：不改。
- `rtk_proto/Frame.cpp`、`AlarmCodec.cpp`：改写为调用 C 接口的薄外观（行为由 74 个 C++ 用例约束不变）。`Bytes.cpp`、`Status.cpp` 不改。
- `rtk_os`：DeepSeek 起草的 RAII 头未经验收，在 T13 中完成并由 QA 审查。

## 勘误 2026-11-17（组件改名后的当前名称）

本节只订正名称与路径事实，不改变本 ADR 的语言分工决定；改名依据 ADR-0008 §9。随后 ADR-0005 又改变了分流器的语言分工
（`stream_demux` 用 C++），以 ADR-0005 为准。

| 本文旧名 | 当前名称 / 路径 |
| --- | --- |
| `rtk_proto`（C 核心 + C++ 外观） | `device_proto`：C 头 `components/device_proto/include/DevProtoFrame.h`、`DevProtoAlarm.h`；C++ 外观 `include/proto/Frame.hpp`、`AlarmCodec.hpp` |
| `rtk_core`（C++） | `link_core`：`components/link_core/include/core/` |
| `rtk_os`（C++） | `rtos`：`components/rtos/include/rtos/`、`fw/Status.hpp` 里的 `rtos::fromEspErr` |
| `rtk_bt`（C 驱动 + C++ 适配器） | `spp_server`：C 头 `include/SppServer.h`、`boot_include/SppServerStorage.h`；适配器 `include/spp/SppTransport.hpp` |
| `rtk_store`（C） | `nvs_store`：`components/nvs_store/NvsStore.c` |
