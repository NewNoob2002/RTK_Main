# H3 分流器黑盒测试 C++ 工程（BATCH-H，ADR-0005）

执行方：pi + antigravity/gemini-3.8-flash（thinking high）。

## 交付
- `test/stream_demux/`：独立 host CMake 工程（C++20，Unity 取自 `$IDF_PATH/components/unity/unity/src`，参照 `test/host/CMakeLists.txt` 的写法与 RTK_CXX_STRICT 告警集，加 `-fno-exceptions -fno-rtti`）。通过 `add_subdirectory(../../components/stream_demux ...)` 使用组件的独立 CMake 目标 `stream_demux`；产品帧解码器直接编译 `components/rtk_proto/demux_frame.cpp` + 它依赖的 rtk_proto 源文件（`rtk_proto_frame.c`、`frame.cpp`、`bytes.cpp`、`status.cpp` 按需）。
- 移植 `reference/c-demux-v1/test/demux` 的全部用例与随机流生成器（`demux_gen` 改写为 C++），只依据冻结头与 ADR，不看 H1/H2 的实现。
- 新增：`StaticDemux`（kBufSize、index_of、init/feed 转发）、`constinit` 全局实例、未 init 调用 feed/abort 返回 kInvalidArg、init 参数校验各分支、`Sink::on_unknown` 默认实现、描述符地址即身份（回调里 `&decoder == &demux::rtcm3::kDecoder`）、CRC 核对值。
- 按 `STREAM_DEMUX_<协议>` 开关裁剪：关闭某协议时测试工程仍能配置构建，该协议相关用例不编译（用 CMake 传入的编译定义判断）。
- 报告 `docs/reports/H3.md`：用例清单与覆盖的 ADR-0005 验收门禁 1 各项。

## 运行
```sh
cmake -S test/stream_demux -B build-sdemux && cmake --build build-sdemux && ctest --test-dir build-sdemux --output-on-failure
cmake -S test/stream_demux -B build-sdemux-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build build-sdemux-asan && ctest --test-dir build-sdemux-asan --output-on-failure
cmake -S test/stream_demux -B build-sdemux-norx -DSTREAM_DEMUX_UBX=OFF && cmake --build build-sdemux-norx && ctest --test-dir build-sdemux-norx
```
H1/H2 未完成前先确保测试工程能配置；实现完成后跑通。测试失败时判断是实现缺陷还是测试缺陷：实现缺陷写进报告（含最小复现），不得放宽断言迁就实现；测试缺陷自己修。

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
