# ADR-0005 分流器改为 C++ 实现、每解码器一个文件、通用组件命名（修订 ADR-0004）

- 状态：**Approved 2026-11-11**。用户决定：“全部用 C++ 重写，解码器拆成每个一个文件……除了跟 esp 绑定相关的组件以 rtk_* 前缀命名外，其他的组件可以根据实际情况来进行命名，这样可以用于区分和后续其他工程复用”。
- 修订范围：ADR-0004 §2 的语言/接口/组件名、§4 的描述符名称、§8 分工。**ADR-0004 §3 引擎参考语义、不变量 I1–I4、§4 各协议判定规则与 max_frame、§5–§7 方向不变，继续有效。**
- 修订 ADR-0003：“字节级编解码用 C”不再适用于分流器；`rtk_proto` 既有 C 核心不动。
- 修订 2026-11-11（H4）：`StaticDemux::index_of` 改用模板实参等价性比较（H3 报告：GCC 16 `-fsanitize=undefined` 下跨 TU `inline constexpr` 对象地址比较不是常量表达式），接口不变。
- C 版实现（BATCH-G，已通过 G4 验收）归档到 `reference/c-demux-v1/`，作参考实现与测试向量来源。

## 1. 组件与命名

| 项 | 决定 |
| --- | --- |
| 组件 | `components/stream_demux`（与 ESP 无关的通用组件不加 `rtk_` 前缀） |
| 命名空间 | `demux`；各解码器 `demux::<协议>`（`rtcm3`、`nmea`、`ubx`、`unicore_bin`、`unicore_hash`）；CRC `demux::crc` |
| 公共头 | `include/demux/decoder.hpp`、`demux.hpp`、`static_demux.hpp`、`crc.hpp`、`decoders/<协议>.hpp`（冻结） |
| 源文件 | `src/demux.cpp`（引擎）、`src/crc.cpp`（两张 constexpr 256 项表）、`src/decoders/<协议>.cpp`（每协议一个） |
| 构建 | 同一份 `CMakeLists.txt`：`ESP_PLATFORM` 下 `idf_component_register` + Kconfig `CONFIG_STREAM_DEMUX_<协议>`；否则 `add_library(stream_demux)` + `option(STREAM_DEMUX_<协议>)`，供其他工程/MCU 复用 |
| 产品帧解码器 | 仍在 `rtk_proto`（与 ESP 无关但属产品协议）：`rtk/proto/demux_frame.hpp`，命名空间 `rtk::proto::demux_frame`，`rtk_proto` REQUIRES `stream_demux` |

命名规则（写入 AGENTS.md）：直接绑定 ESP-IDF/产品的组件用 `rtk_` 前缀与 `rtk::<组件>` 命名空间；与平台、产品都无关、可被其他工程复用的组件按功能命名（组件名 = 目录名，命名空间取简短功能名），不得依赖任何 `rtk_*` 组件或 IDF/FreeRTOS 头。

## 2. 接口要点（以冻结头为准）

- `demux::Decoder`：`{name, max_frame, first_byte, step}`，函数指针均 `noexcept`；`step(std::span<const uint8_t>, std::size_t& need)`。去掉 C 版 `proto_id`：**描述符地址即身份**（`inline constexpr` 变量全程序唯一地址）。
- 每个解码器头文件导出 `first_byte`、`step`、`kMaxFrame` 与 `inline constexpr Decoder kDecoder`，加减协议只需加减头文件/源文件，不改引擎。
- `demux::Sink`：`on_frame(const Decoder&, uint8_t index, span)`、`on_unknown(span)`（默认丢弃）。**作为 CPP-011 的第二个授权多态边界**（受保护非虚析构，实现类建议 `final`）；理由：回调需绑定对象状态，`std::function` 禁用，`void*` 不类型安全，模板化 Sink 会让引擎按 Sink 类型多实例化。每帧一次虚调用开销可忽略。
- `demux::Demux`：`constexpr` 默认构造（可 `constinit`），默认成员初值使“未初始化存储”问题不存在（修复 C 版 G1 阻塞项 1）；`init(span<const Decoder* const>, span<uint8_t>, Sink&)`、`feed(span)`、`abort()`、`busy()`、`stats()`、`reset_stats()`；错误 `enum class Error {kOk, kInvalidArg, kReentry}`（通用组件不依赖 `rtk::Status`）。内部实现通过 `friend struct Engine`（定义在 `demux.cpp`）访问私有状态，冻结头不随实现变。
- `demux::StaticDemux<const Decoder&... Ds>`：解码器集合为模板参数，缓冲区 = 最大 `max_frame`（`std::array`），数量 1..8 编译期检查，`index_of<D>()` 编译期下标。
- `demux::crc`：`crc24q`、`crc32_novatel`，可分段累计；核对值 `"123456789"` → `0xCDE703` / `0x2DFD2D88`（已用 Python 逐位实现核对）。

## 3. 语言规范

AGENTS.md §2 C++ 规则全部适用（C++20 子集；无异常/RTTI/堆/`std::function`/容器；`static_cast` 注释范围；`[[nodiscard]]`；ESP-IDF 风格布局）。额外：
- 只依赖标准头 `<array> <algorithm> <cstddef> <cstdint> <cstring> <optional> <span>`；无浮点、无递归、无 VLA、栈 O(1)。
- 必须能被 host `g++ -std=c++20`、`g++ -m32` 与 `arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os -fno-exceptions -fno-rtti` 零告警编译。
- 溯源：新源文件头写 `Generated-by: pi + <模型>`、人工负责人 `UNKNOWN（待人工审查签字）`。

## 4. 本批次分工（BATCH-H）

| 任务 | 内容 | 执行 | 理由 |
| --- | --- | --- | --- |
| H1 引擎 | `src/demux.cpp`（ADR-0004 §3 全部语义） | pi + `codex/gpt-6.1-sol` high | 回放重扫与重入是最难的算法部分；该模型已写过 C 版 |
| H2 解码器 | `src/crc.cpp`、5 个 `src/decoders/*.cpp`、`rtk_proto/demux_frame.cpp` 与 `rtk_proto/CMakeLists.txt` 接入 | pi + `workbuddy/deepseek-v4.1-flash` high | 规格明确、量大；该模型写过 C 版解码器 |
| H3 测试 | `test/stream_demux/`：独立 host CMake + Unity，C++ 黑盒测试，移植 `reference/c-demux-v1/test/demux` 全部用例与生成器 | pi + `antigravity/gemini-3.8-flash` high | 与实现隔离；写过 C 版测试 |
| H4 验收 | 交叉审查、host 普通 + ASan/UBSan、`-m32`/arm 交叉编译、`idf.py build`、冻结校验 | 架构方（Claude） | 验收独立 |

### 验收门禁

1. `test/stream_demux` 普通与 ASan/UBSan 构建全部通过，C++ 严格告警（`test/host` 的 RTK_CXX_STRICT 集）零告警；覆盖不少于 C 版 28 个用例的全部场景（含 I1 随机切分 ≥1000 轮、I2、重入、overflow、契约违规、abort、同首字节冲突、假前导恢复、产品帧 vs Unicore 二进制），另加 `StaticDemux`（缓冲区定长、`index_of`）、`constinit` 全局实例、未 init 调用返回 `kInvalidArg`、`Sink::on_unknown` 默认实现。
2. 每个源文件 × {`g++`, `g++ -m32`, `arm-none-eabi-g++ cortex-m0plus -Os`} 零告警；报告 xtensa 下各文件 `.text/.rodata`。
3. 独立 CMake 下关闭任一 `STREAM_DEMUX_<协议>` 仍能构建（测试工程对应用例跳过或按开关裁剪）。
4. `idf.py build` 通过；`test/host` 8/8 回归；`sha256sum -c docs/frozen.sha256` 通过。
