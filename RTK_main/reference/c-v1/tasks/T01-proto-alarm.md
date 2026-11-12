# T01 帧编解码与报警消息（纯 C）

```yaml
task: T01-proto-alarm
observable_outcome: >
  test/host 中 test_proto_frame 与 test_alarm_msg 全部 PASS；idf.py build 仍 0 warning。
frozen_inputs:
  - components/proto/include/proto_frame.h
  - components/alarm/include/alarm_msg.h
  - docs/protocol/phone_protocol.md
  - test/host/CMakeLists.txt, test/host/test_proto_frame.c, test/host/test_alarm_msg.c
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/proto/proto_frame.c, components/alarm/alarm_msg.c, docs/reports/T01.md]
affected_checks:
  - cmake -S test/host -B build-host && cmake --build build-host --target test_proto_frame test_alarm_msg
  - ./build-host/test_proto_frame && ./build-host/test_alarm_msg
  - idf.py build
  - barrc check.sh 对两个 .c
stop_conditions:
  - 测试与头文件契约矛盾（写入报告，不改测试）
  - 需要动态内存才能实现
hardware_gap: 无
```

实现要点：
- CRC-32 用 256 项查表（`static const uint32_t` 表，多项式 0xEDB88320），或逐位算法；不得依赖 IDF `esp_rom_crc32_le`（纯 C 模块要求 host 可编译）。
- 解析器严格按 `proto_parser_feed` 注释中的状态机；所有多字节字段按小端逐字节组装，不得把缓冲区强转成结构体指针（对齐与 BARR-C 5.5）。
- `alarm_msg_encode_status` 按字节写，不用 `memcpy` 结构体。
- 注意：host 测试 CMake 会同时编译 link_mgr.c 与 cmd_disp.c；若 T02/T03 尚未完成，可只构建本任务的两个目标（见 affected_checks）。
