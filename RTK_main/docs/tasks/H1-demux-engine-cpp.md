# H1 分流引擎 C++ 实现（BATCH-H，ADR-0005）

执行方：pi + codex/gpt-6.1-sol（thinking high）。

## 交付
- `components/stream_demux/src/demux.cpp`：实现 `demux/demux.hpp` 中 `Demux::init/feed/abort`，严格遵循 ADR-0004 §3 参考语义与不变量 I1–I4。私有状态通过 `struct Engine`（在本文件定义，头文件已声明 `friend struct Engine`）的静态成员函数访问；不得在头文件里加成员。
- 可选自测只能放 `test/stream_demux_dev/h1/`（不交付），不得改 `test/stream_demux/`（H3）与 `test/host/`。
- 报告 `docs/reports/H1.md`。

## 要求
- 只包含 `demux/demux.hpp` 与 `<algorithm>`/`<cstring>` 等允许的标准头；无堆、无递归、无 VLA、无浮点；栈 O(1)（不得开 buf 级栈数组）；回放原地完成（`std::memmove`）。
- 回放中途某解码器在 p < len 处得到 kFrame：输出该帧，把 buf[p..len) 当作新输入重新处理；回调期间 buf 内容保持该帧不变直到回调返回。
- 防重入：回调中对同一实例调用 init/feed/abort 返回 `Error::kReentry` 且不破坏状态。未 init 成功时 feed/abort 返回 `kInvalidArg`。
- init 校验：解码器表 1..kMaxDecoders、无空指针、每个描述符 name/first_byte/step 非空且 max_frame ∈ 1..65535、buf.size() ≥ 最大 max_frame；任一失败 → kInvalidArg 并回到未初始化状态（decoder_count_ = 0）。
- 未知字节：连续的合并后回调 `sink_->on_unknown`；帧回调前先冲刷积压的未知字节；来自 buf 的未知字节回调后再 memmove。
- need 契约违规（0 或 len+need > max_frame）按 kReject 处理并计入该解码器 rejects；len+need > buf 容量另计 overflows。
- 性能：空闲态每字节一次位图查表；累积态按 need 整段 memcpy，每段只调用一次 step。
- C 版 `reference/c-demux-v1/components/rtk_demux/rtk_demux.c` 是你（同模型）写过并通过验收的实现，可按 C++ 规范移植。

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
