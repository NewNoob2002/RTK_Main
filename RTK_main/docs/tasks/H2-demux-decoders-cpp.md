# H2 解码器与 CRC C++ 实现（BATCH-H，ADR-0005）

执行方：pi + workbuddy/deepseek-v4.1-flash（thinking high）。

## 交付
- `components/stream_demux/src/crc.cpp`：`demux/crc.hpp` 的两个函数；两张 256 项表用 `constexpr` 函数在编译期生成（`static constexpr std::array`），不得手抄表。核对值 `"123456789"` → `0xCDE703` / `0x2DFD2D88`。
- `components/stream_demux/src/decoders/{rtcm3,nmea,ubx,unicore_bin,unicore_hash}.cpp`：每个文件只实现对应头文件的 `first_byte` 与 `step`，规则见 ADR-0004 §4 与各头文件注释。文件之间不得互相依赖（共用部分只能是 `demux/crc.hpp`）；文件内辅助函数放匿名命名空间。
- `components/rtk_proto/demux_frame.cpp`：`rtk/proto/demux_frame.hpp` 的 `first_byte`、`step`、`view`；规则与 `rtk_proto_parser_feed`（`components/rtk_proto/rtk_proto_frame.c`）完全一致，CRC 复用 `rtk::proto::crc32`。
- `components/rtk_proto/CMakeLists.txt`：SRCS 加 `demux_frame.cpp`，加 `REQUIRES stream_demux`，并注释原因（ADR-0005）。不得改其他已有行。
- 报告 `docs/reports/H2.md`：每个解码器的早拒绝点、每个 CRC/校验的已知向量核对表（可沿用 `reference/c-demux-v1/docs/G2.md` 的向量）。
- 可选自测只能放 `test/stream_demux_dev/h2/`（不交付）。

## 要求
- step 必须是纯函数（契约见 `demux/decoder.hpp`）：长度已知后一次要求剩余全部字节；NMEA/Hash 不知长度时 need = 1；need 永不为 0，len+need 永不超过 kMaxFrame（超出时 kReject）。
- 不得读越界：只访问 `frame[0..frame.size())`。
- C 版 `reference/c-demux-v1/components/rtk_demux/rtk_demux_decoders.c` 与 `.../rtk_proto/rtk_proto_demux.c` 是你（同模型）写过并通过验收的实现，可按 C++ 规范移植。

## 共同要求
- 先完整阅读：`AGENTS.md`、`docs/adr/ADR-0005-stream-demux-cpp.md`、`docs/adr/ADR-0004-stream-demux.md`（§3 语义、§4 协议规则仍有效）、`components/stream_demux/include/demux/**`、`components/rtk_proto/include/rtk/proto/demux_frame.hpp`。
- C 版参考实现（已通过验收，只读）：`reference/c-demux-v1/`，含 G1–G3 报告。可借鉴算法与测试向量，但必须写成符合 AGENTS.md §2 的 C++20 子集，不得包含任何 C 版头文件。
- 冻结文件（AGENTS.md §4，含上述所有头文件、ADR、本任务卡、`components/stream_demux/CMakeLists.txt` 与 `Kconfig`）一个字节都不能改；发现接口矛盾或无法实现时停下，在报告中写证据。
- 源文件头：`// Generated-by: pi + <你的模型>`，`// 人工负责人：UNKNOWN（待人工审查签字）`。注释用中文。每处 `static_cast` 注释说明范围安全的理由。
- 可移植性自检（报告逐项 PASS/FAIL）：对你交付的每个 .cpp 执行
  `g++ -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough -Wundef -Wswitch-default -Wcast-align -Wnull-dereference -Icomponents/stream_demux/include -Icomponents/rtk_proto/include -c <文件> -o /dev/null`，
  以及同样参数加 `-m32`、以及 `arm-none-eabi-g++ ... -mcpu=cortex-m0plus -mthumb -Os`。（zsh 中变量不分词，用 `bash -c` 跑。）
- 禁止：烧录/monitor/连接串口、联网下载或安装、git commit/push、修改他人任务的文件。
- 结束时写报告 `docs/reports/<任务号>.md`（改动文件、实际执行命令与结果 PASS/FAIL/NOT_RUN/BLOCKED、偏离、未解决问题），最后一行输出 `<任务号> DONE` 或 `<任务号> BLOCKED: 原因`。
