# MW1 — WiFi 模式堆诊断与内存回收

- 分支/工作树：`feat/mw1-wifi-heap`，`../RTK_Main-mw1`，基线 `8454efc` 之后的本卡提交
- 门禁：`tools/ci/run_gates.sh --prefix mw1 --base <本卡提交>`（需 `export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`）
- 报告：`docs/reports/MW1-wifi-heap-diag.md`；完成时输出 `MW1-FINISHED`
- 参考：`docs/reports/MEM-compare-v31.md`（静态对比与节省估算）

## 背景
整机（P2 v1.41 + CM680，固件 `8454efc` console 档）WiFi 模式、PC 连网页实测：free 41,996–43,712 B，min 29,436 B；静态估算应约 72 KB，约 28–30 KB 无法从代码解释。网页只显示 free/min，整机无串口，无法定位。

## 范围
### A 诊断（先做，便于前后对比）
1. WebSocket `status.dev` 增加：`heap_largest`（`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`）、`heap_int_free`/`heap_int_min`（MALLOC_CAP_INTERNAL）。
2. 新增按需诊断（例如 `{"t":"diag"}` 请求或 status 低频附带，二选一并写明理由）：各任务名 + 栈大小 + 高水位（`uxTaskGetStackHighWaterMark`，任务表固定上限，无堆分配）、`heap_caps_get_info` 主要字段、BootArena 各分区用量（若已有统计接口则复用）。
3. 网页 System 卡显示 largest；诊断数据可只在 JSON 中，不强制 UI。
4. 更新 web 协议文档（`components/web_proto` 及对应 docs），保持旧字段不变、只增字段。

### B 回收
1. `main/app/BootFlow.cpp:210` WiFi 配置模式改用 `esp_bt_mem_release(ESP_BT_MODE_BTDM)`（释放 controller + Bluedroid `_bt_data`/`_bt_bss`，约 23,048 B）；保持“本次启动禁止 esp_bt_* 调用”约束，确认无任何 WiFi 路径触碰 BT 静态区（含 bt_config/NVS 读取、SPP 组件 init 防护）。
2. `web.frames` 帧池 16 → 10（`main/modules/web/WebModule.hpp:35-40`，`kRxMailboxCap`/`kTxMailboxCap`/`kScratchFrames`），保证 `kTxReservedCtrlSlots` 语义；host 测试覆盖满池/控制帧保留。
3. `CONFIG_RTK_CONSOLE_UART_TX_BUF` 8192 → 2048（`sdkconfig.defaults:64`），确认长打印（`sys tasks`/trace dump）不丢字节、只阻塞。

### 不做
- **`rtcm.pipe` 在 WiFi 模式保留**（用户 2026-11-26：WiFi 模式后续要接 4G/NTRIP/MQTT/WiFi-STA 差分，必须预留）。
- 不改 trace 档位、不改 console 档位、不改 WiFi/LwIP sdkconfig 参数（另行实测后决定）。

## 验收
- 门禁全绿；host 测试新增用例在基线上失败或覆盖新字段。
- 资源：WiFi 模式静态 DRAM 减少、IRAM 0 增长；报告列出 `idf.py size` 前后与 BootArena 前后。
- 报告给出 HIL 步骤：用户通过 WiFi OTA 升级整机（无串口），我用 `ws://192.168.10.12/ws` 读 free/min/largest/任务高水位，前后对比；目标 free 较基线 +≥25 KB。
- 若诊断显示 30 KB 缺口来自其他位置，在报告中列出证据，不在本卡顺手改。
