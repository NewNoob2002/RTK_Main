# T04 Bluedroid SPP 多会话驱动

```yaml
task: T04-bt-spp
observable_outcome: >
  components/bt_spp/bt_spp.c 实现 bt_spp.h 全部函数；idf.py build 0 warning；
  报告中列出对 IDF v6.1 源码核实的三个问题的答案及文件:行号。
frozen_inputs:
  - components/bt_spp/include/bt_spp.h
  - ~/esp/esp-idf/examples/bluetooth/bluedroid/classic_bt/bt_spp_acceptor/main/main.c（参考，只读）
  - ~/esp/esp-idf/components/bt/host/bluedroid/api/include/api/esp_spp_api.h
owner: omp/DeepSeek-V4.1-Flash
outputs: [components/bt_spp/CMakeLists.txt, components/bt_spp/bt_spp.c, docs/reports/T04.md]
affected_checks: [idf.py build, barrc check.sh bt_spp.c]
stop_conditions:
  - 核实结果显示 IDF v6.1 SPP 服务端不能同时接受 2 个会话
  - esp_spp_write 不拷贝数据且无法安全保证缓冲区寿命
hardware_gap: 连接/吞吐/拥塞行为只能由 HIL-1..HIL-4 证明
```

必须先核实并写入报告（引用 IDF 源码 文件:行号）：
1. `esp_spp_write()` 是否深拷贝数据（看 `esp_spp_api.c` 与 `btc_spp.c` 的 arg deep copy）。
2. 服务端在 `ESP_SPP_SRV_OPEN_EVT` 后是否继续监听第二个连接（`new_listen_handle` 与 `max_session`）。
3. 对端地址在哪个事件参数中（`srv_open.rem_bda`）。

实现要点：
- 初始化顺序参照官方示例：`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` → `esp_bt_controller_init` → `enable(ESP_BT_MODE_CLASSIC_BT)` → `esp_bluedroid_init_with_cfg` → `enable` → 注册 GAP/SPP 回调 → `esp_spp_enhanced_init`（`ESP_SPP_MODE_CB`）→ 在 `ESP_SPP_INIT_EVT` 中 `esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "SPP_SERVER")`、设置设备名、`esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE)`。
- GAP：SSP IO 能力 `ESP_BT_IO_CAP_NONE`；`ESP_BT_GAP_CFM_REQ_EVT` 自动确认（与 5.4 免 PIN 行为一致）。
- 会话表：静态数组 `BT_SPP_MAX_SESSIONS`，每会话 `xStreamBufferCreateStatic`（缓冲区静态分配）、拥塞标志、`_Atomic` 或临界区保护的 in_use。
- 事件队列：`xQueueCreateStatic`，深度 16；回调中 `xQueueSend(..., 0)`，满时计数并记日志（回调不得阻塞）。
- `ESP_SPP_DATA_IND_EVT`：`xStreamBufferSend(..., 0)`，未写入部分计入 `BT_SPP_EVT_RX_DROP`。
- `bt_spp_write`：拥塞时用事件组/任务通知等待 `ESP_SPP_CONG_EVT` 解除，有界超时。
- 设备名不得含 `\n`。
- 日志 tag `"bt_spp"`；不在回调中打印数据内容。
