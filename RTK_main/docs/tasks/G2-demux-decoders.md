# G2 内置解码器（BATCH-G，ADR-0004）

执行方：pi + workbuddy/deepseek-v4.1-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0004-stream-demux.md`（重点 §2、§4）、`components/rtk_demux/include/rtk_demux.h`、`rtk_demux_decoders.h`、`components/rtk_proto/include/rtk_proto_demux.h`、`rtk_proto_frame.h`。

## 交付
1. `components/rtk_demux/rtk_demux_decoders.c`：`rtk_demux_dec_rtcm3`、`_nmea`、`_ubx`、`_unicore_bin`、`_unicore_hash` 五个 const 描述符及其 `first_byte`/`step`（`static`）。
2. `components/rtk_proto/rtk_proto_demux.c`：`rtk_proto_demux_decoder` 与 `rtk_proto_frame_view()`；规则与 `rtk_proto_frame.c` 的 `rtk_proto_parser_feed` 一致，CRC 复用公开的 `rtk_proto_crc32()`。**不得修改** `rtk_proto_frame.c` 等已验收文件。
3. `components/rtk_proto/CMakeLists.txt`：SRCS 加 `rtk_proto_demux.c`，`REQUIRES rtk_demux`（ADR-0004 已修订依赖表）。
4. 报告 `docs/reports/G2.md`：每种 CRC/校验的已知向量与核对方法、各解码器的 `.text/.rodata` 大小（xtensa：`xtensa-esp32-elf-gcc` 或 `idf.py size-components`，任一可用即可）。

## 要求
- `step` 是纯函数（契约见 `rtk_demux.h`）：只读 `frame[0..len)`，无静态可变状态。每个早拒绝点尽早 REJECT；长度字段已知后一次性返回剩余全部 `need`；校验在帧尾整段计算。
- NMEA / Unicore Hash 不知道长度：逐字节 `need = 1`，每次只检查新到的最后一个字节的合法性（O(1)），到 `\n` 再整体校验；超过 max_frame 前必须 REJECT。
- CRC：CRC-24Q（RTCM）与 NovAtel 风格 CRC-32（Unicore：反射 0xEDB88320、初值 0、不取反）可用 256 项 `static const` 表。用你自己写的独立 Python 位运算实现生成已知向量并在报告中给出；标准核对值：CRC-24/LTE-A（=CRC-24Q）`"123456789"` → `0xCDE703`；`rtk_proto_crc32` `"123456789"` → `0xCBF43926`。
- AGENTS.md §2 的 C 文件规则；多字节字段逐字节读；源文件头 `Generated-by: pi + DeepSeek-V4.1-Flash`，人工负责人 `UNKNOWN（待人工审查签字）`。
- 自测（可选、不交付）只能放在 `test/demux_dev/g2/`。

## 自检命令
同 G1 的三条编译命令，对两个 .c 各跑一遍（rtk_proto_demux.c 加 `-Icomponents/rtk_proto/include`）。G3 的 `test/demux` 就绪后跑普通与 ASan 构建，在报告中给结果；不要改 G3 的测试，有争议写证据。

## 禁止
修改冻结文件、`rtk_demux.c`（G1）、`test/demux/`（G3）、已验收源文件；烧录、联网安装、git commit。
