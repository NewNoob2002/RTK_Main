# T02 会话角色管理（纯 C）

```yaml
task: T02-link
observable_outcome: test/host/test_link_mgr 全部 PASS；idf.py build 0 warning。
frozen_inputs:
  - components/link/include/link_mgr.h
  - test/host/test_link_mgr.c
  - docs/adr/ADR-0001-architecture.md §3
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/link/link_mgr.c, docs/reports/T02.md]
affected_checks: [build-host test_link_mgr, idf.py build, barrc check.sh link_mgr.c]
stop_conditions: [测试与头文件规则矛盾]
hardware_gap: scatternet 实际行为由 HIL-3 验证
```

实现要点：
- 所有时间比较写成 `(uint32_t)(now_ms - t) > LIMIT`，以容忍回绕。
- 规则按头文件注释的编号顺序实现；被断开的会话立即从表中移除。
- 白名单比较地址 6 字节全等；加入时不重复。
