# T03 命令分发（纯 C）

```yaml
task: T03-cmd
observable_outcome: test/host 四个测试可执行文件全部 PASS（ctest 0 失败）；idf.py build 0 warning。
frozen_inputs:
  - components/cmd/include/cmd_disp.h
  - docs/protocol/phone_protocol.md §3–§5
  - test/host/test_cmd_disp.c
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/cmd/cmd_disp.c, docs/reports/T03.md]
affected_checks:
  - cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
  - idf.py build
  - barrc check.sh cmd_disp.c
stop_conditions: [协议文档与测试矛盾]
hardware_gap: 与真实手机 App 的兼容性由 HIL-2 验证
```

实现要点：
- 用 `switch (msg_id)` + 每个命令一个 `static` 处理函数，单函数 ≤ 100 行。
- 应答 payload 在栈上或 ctx 内构造，经 `proto_frame_build` 写入 `p_reply->buf`；`proto_response_type()` 决定 type。
- 字符串字段用“按字段宽度拷贝、不足补 0”的辅助函数（`strnlen` 不在 C99，自己写有界循环）。
- 0x44 查询应答长度 = 1 + 6 × 数量。
- 5.4 保留 ID 与未知 ID 都返回 `CMD_RES_NO_REPLY`，不生成帧。
