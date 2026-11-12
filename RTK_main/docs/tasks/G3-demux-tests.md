# G3 分流器黑盒测试（BATCH-G，ADR-0004）

执行方：pi + antigravity/gemini-3.8-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0004-stream-demux.md`（全文，§3 与 §8 验收门禁是测试依据）、`components/rtk_demux/include/rtk_demux.h`、`rtk_demux_decoders.h`、`components/rtk_proto/include/rtk_proto_demux.h`、`rtk_proto_frame.h`。

**只依据头文件与 ADR 写测试，不要阅读 `rtk_demux.c`、`rtk_demux_decoders.c`、`rtk_proto_demux.c` 的实现**（它们由 G1/G2 并行编写，可能尚不存在）。

## 交付：`test/demux/` 独立 host CMake 工程
- `CMakeLists.txt`：C11、`CMAKE_C_EXTENSIONS OFF`；Unity 取自 `$IDF_PATH/components/unity/unity/src`（参照 `test/host/CMakeLists.txt` 的写法）；被测源文件：`components/rtk_demux/rtk_demux.c`、`rtk_demux_decoders.c`、`components/rtk_proto/rtk_proto_frame.c`、`rtk_proto_demux.c`；严格告警：`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes`；`enable_testing()` + 每个测试可执行文件一个 `add_test`。
- 测试辅助：`demux_gen.c/.h`——用你自己独立实现的 CRC（位运算版即可，不要复制被测代码）生成各协议合法帧：RTCM3、NMEA、UBX、Unicore 二进制、Unicore Hash（CRC-32 与 XOR 两种）、产品帧（用 `rtk_proto_frame.h` 的 `rtk_proto_frame_build()`）。辅助函数自身要有已知向量测试（CRC-24Q `"123456789"`=`0xCDE703`，IEEE CRC-32=`0xCBF43926`）。
- `test_demux_engine.c`：用测试内自定义的简单解码器（如固定长度、`need` 契约违规、同首字节两个解码器）测试引擎：init 参数校验、空闲未知字节、FRAME/NEED_MORE/REJECT 流程、同首字节按优先级回放、全部拒绝后起点前移、回放中途成帧后剩余字节再处理、overflow、契约违规、重入（回调中调 feed/abort/init → `RTK_DEMUX_ERR_REENTRY`）、`abort`、`busy`、统计各字段、`reset_stats`。
- `test_demux_decoders.c`：每个内置解码器：正例、每个早拒绝点、校验错、长度超限、大小写十六进制、NMEA 与 Unicore Hash 的非法字符。
- `test_demux_stream.c`：属性测试（固定种子的 xorshift PRNG，种子写进失败信息）：
  - I1 分块无关：同一流 ≥1000 轮随机切分（含 1 字节切分与整块），帧序列与未知字节拼接结果一致；
  - I2 字节守恒：帧字节数 + 未知字节数 == 输入字节数（feed 全部 + abort 之后）；
  - 混合流：随机多协议帧 + 随机垃圾（垃圾中刻意插入 `D3`、`AA 44`、`$`、`#`、`B5 62` 等假前导，其后紧跟真帧），abort 后所有真帧按序恢复且内容逐字节相同；
  - 蓝牙场景：`{rtk_proto_demux_decoder, rtk_demux_dec_rtcm3}` 交错流；GNSS 场景：`{nmea, rtcm3, unicore_bin, unicore_hash}` 与 `{nmea, rtcm3, ubx}`；产品帧与 Unicore 二进制同挂一个实例时按第 3 字节区分。
- 报告 `docs/reports/G3.md`：测试清单、实际命令与结果。被测实现尚不存在或失败时，如实列出失败用例与你认为的原因（引擎/解码器/测试本身），**不要为了通过而放宽断言**；你不能修改被测源文件。

## 命令
```sh
. ~/esp/esp-idf/export.sh >/dev/null
cmake -S test/demux -B build-demux && cmake --build build-demux && ctest --test-dir build-demux --output-on-failure
cmake -S test/demux -B build-demux-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build build-demux-asan && ctest --test-dir build-demux-asan --output-on-failure
```
测试源文件头：`Generated-by: pi + antigravity/gemini-3.8-flash`，人工负责人 `UNKNOWN（待人工审查签字）`。

## 禁止
修改冻结文件、`components/` 下任何文件、`test/host/`；烧录、联网安装、git commit。
