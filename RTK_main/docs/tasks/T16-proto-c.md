# T16 rtk_proto：C 协议核心 + C++ 外观（ADR-0003）

```yaml
task: T16-proto-c
observable_outcome: >
  host 8 个可执行文件全部 PASS（普通构建与 ASan/UBSan 构建，93 用例）；idf.py build 0 warning；
  C 文件 clang-tidy --config-file=.clang-tidy-c 0 发现，C++ 文件 clang-tidy 0 发现；rtk_core/*.cpp 未改动。
frozen_inputs:
  - components/rtk_proto/include/**（C 头 rtk_proto_frame.h、rtk_proto_alarm.h；C++ 外观头）
  - test/host/**
  - docs/adr/ADR-0003-c-cpp-split.md
  - reference/c-v1/components/proto/proto_frame.c、reference/c-v1/components/alarm/alarm_msg.c（经 QA-A 验收，移植来源）
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - components/rtk_proto/rtk_proto_frame.c、rtk_proto_alarm.c（新）
  - components/rtk_proto/frame.cpp、alarm_codec.cpp（改写为外观）
  - components/rtk_proto/CMakeLists.txt
  - docs/reports/T16.md
affected_checks: [build-host ctest, build-asan ctest, idf.py build, clang-format, clang-tidy（C 与 C++ 各自配置）]
stop_conditions: [C 头、C++ 外观头与测试三者矛盾]
hardware_gap: 无
```

起点状态：FrameParser 的私有成员已改为 `rtk_proto_parser_t parser_`，因此当前 frame.cpp 无法编译、host 构建缺少两个 .c 文件；这是本任务要修复的，属于预期状态。

要点：
1. C 实现从 reference/c-v1 移植：符号改为 `rtk_proto_*`/`RTK_PROTO_*`，布局改为 IDF 风格（clang-format），去掉 BARR-C 的 `p_`/`g_` 前缀；新增 `rtk_proto_parser_reset`、`rtk_proto_alarm_decode_status`、`rtk_proto_alarm_encode_heartbeat`；不移植 `alarm_src_*`（已由 C++ `AlarmSource` 承担）。CRC 逐位或查表均可，报告说明。
2. `frame.cpp`：`crc32`、`response_type`、`build_frame`、`FrameParser::reset/feed` 全部转调 C 函数；`build_frame` 先检查 `payload.size() > kMaxPayload` 再窄化为 `uint16_t`；C 错误码映射为 `Status::kInvalidArg`/`kNoSpace`；`FrameView` 从 `rtk_proto_frame_t` 逐字段拷贝。
3. `alarm_codec.cpp`：四个函数转调 C 函数，结构逐字段转换。不得在 C++ 中重复实现编解码逻辑。
4. CMakeLists：SRCS 同时含 .c 与 .cpp；告警组按 AGENTS.md §3 用生成器表达式区分 C/C++。
