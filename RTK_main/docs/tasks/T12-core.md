# T12 rtk_core：AlarmSource、LinkManager、CommandDispatcher、LinkService

```yaml
task: T12-core
observable_outcome: >
  六个 host 测试全部 PASS（ctest 0 失败；普通构建与 ASan/UBSan 构建）；idf.py build 0 warning；
  clang-tidy -p build-host 对 rtk_core/*.cpp 0 发现。
frozen_inputs:
  - components/rtk_core/include/**
  - docs/protocol/phone_protocol.md §3–§5
  - test/host/**
  - reference/c-v1/components/{link,cmd}/*.c 与 reference/c-v1/main/app_link.c（行为参考）
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/rtk_core/{alarm_source,link_manager,command_dispatcher,link_service}.cpp, docs/reports/T12.md]
affected_checks: [build-host ctest, build-asan ctest, idf.py build, clang-format, clang-tidy]
stop_conditions: [协议文档、头文件与测试三者矛盾]
hardware_gap: 与真实手机 App、报警器的兼容性由 HIL 验证
```

实现要点：
- LinkManager：时间比较一律写 `static_cast<uint32_t>(now - t) > limit`；规则按头文件注释的编号顺序实现。
- CommandDispatcher：`switch (msg_id)` 加每个命令一个私有/匿名命名空间处理函数，单个函数 ≤ 100 行。应答 payload 用栈上的 `std::array` 配合 `ByteWriter` 构造，再交给 `proto::build_frame`。字符串字段用 `ByteWriter::fixed_str(info.xxx.view(), N)`。
- LinkService：严格按头文件注释中的 4 步处理；断开当前句柄后丢弃剩余字节；释放槽位时把该解析器的错误计数累加到 `retired_*`。
- 不使用堆，不使用 IDF 头文件。
