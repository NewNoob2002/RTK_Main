# T05 NVS 持久化

```yaml
task: T05-store
observable_outcome: components/store/store_nvs.c 实现 store_nvs.h；idf.py build 0 warning。
frozen_inputs: [components/store/include/store_nvs.h]
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/store/CMakeLists.txt, components/store/store_nvs.c, docs/reports/T05.md]
affected_checks: [idf.py build, barrc check.sh store_nvs.c]
stop_conditions: [需要擦除 nvs 以外的分区]
hardware_gap: 掉电一致性由 HIL-5 验证
```

实现要点：
- 每次读写 `nvs_open` → 操作 → `nvs_close`，所有返回值检查；写后 `nvs_commit`。
- `store_get_alarms`：blob 长度必须是 6 的倍数且 ≤ `STORE_ALARM_MAX*6`，否则视为损坏：返回 `ESP_ERR_INVALID_SIZE`，`*p_count=0`。
- `store_set_sn`：长度 0 或 > `STORE_SN_MAX` 返回 `ESP_ERR_INVALID_ARG`。
