# T11 rtk_proto：Status、字节读写、帧、报警编解码

```yaml
task: T11-proto
observable_outcome: >
  test_bytes、test_frame、test_alarm_codec 三个 host 测试全部 PASS（普通构建和 ASan/UBSan 构建都要通过）；
  idf.py build 0 warning；clang-tidy -p build-host 对 rtk_proto/*.cpp 0 发现。
frozen_inputs:
  - components/rtk_proto/include/**
  - docs/protocol/phone_protocol.md
  - test/host/**
  - reference/c-v1/components/{proto,alarm}/*.c（行为参考）
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/rtk_proto/{status,bytes,frame,alarm_codec}.cpp, docs/reports/T11.md]
affected_checks:
  - cmake --build build-host --target test_bytes test_frame test_alarm_codec，并逐个运行
  - 同样的三个目标在 build-asan 中运行
  - idf.py build
  - clang-format --dry-run --Werror、clang-tidy -p build-host
stop_conditions: [测试与头文件契约矛盾（写入报告，不改测试）]
hardware_gap: 无
```

实现要点：
- `crc32`：标准反射算法，可以用 `consteval`/`constexpr` 生成 256 项表（Flash 约 1 KB，可接受），也可以逐位计算，在报告中说明选了哪种。
- 帧组装与解析只经 `ByteWriter`/`ByteReader` 读写多字节字段；不对缓冲区做 `reinterpret_cast`。
- `FrameParser::feed` 严格按头文件注释中的状态机实现；`buf_` 下标在任何输入下都 < kMaxFrame。
- 注意：test/host 的 CMake 会一起编译 rtk_core 的四个 .cpp。T10 中留下的空实现会导致 rtk_core 的符号未定义，所以本任务只构建上面列出的三个目标。
