# G-baud-save 实现报告

- 任务卡：`docs/tasks/G-baud-save.md`
- 分支：`feat/gnss-optisens-q1`（基线 `215851d`，构建前缀 `builds/gq-*`，叠加于 `G-optisens-family` 与 `Q1-gga-quality-gate` 之上）
- 结论：实现完成，除 `G-optisens-family` 授权的 2 个冻结头触发的 `G1-frozen` 外，全部自动门禁（34/35）`PASS`；HIL 待 Architect 在实板执行（`NOT_RUN`）。本卡零冻结文件改动。

## 1. 改动清单

| 文件 | 改动摘要 |
| --- | --- |
| `main/modules/gnss/GnssModule.hpp` | 新增 `boot_baud_save_in_flight_` 状态位，跟踪启动切速成功后发起的一次性接收机保存维护。 |
| `main/modules/gnss/GnssModule.cpp` | `startController()` 显式复位 `boot_baud_in_flight_` 与 `boot_baud_save_in_flight_`。 |
| `main/modules/gnss/GnssModuleMaintenance.cpp` | `completeBootBaudMaintenance()` 在启动切速验证成功（`kOk`）并调用 `syncBootHint()`（仅更新 `gnss.cache`，不写 `gnss/baud`）后，复用 `controller().requestSave()` 发起一次模块 Flash 保存并置 `active_maintenance_ = Maintenance::kSave`；新增 `logBootBaudSaveResult()` 处理结果（`kOk` 记 I 日志、`kUnsupported` 静默跳过、`kNack`/`kTimeout`/`kIoError` 记 W 日志与 `GnssTrace::kStepNack`，**不**计入 `baud_fail`、**不**回退波特率、不覆盖控制台手动维护状态）。 |
| `main/modules/gnss/GnssModuleMode.cpp` | `pollWorkMode()` 等待 `boot_baud_pending_ || boot_baud_in_flight_ || boot_baud_save_in_flight_`，避免启动模式恢复与切速后的 `SAVECONFIG` 交叠。 |
| `main/modules/gnss/GnssModulePpp.cpp` | `canStartPhonePpp()` 增加 `!boot_baud_in_flight_ && !boot_baud_save_in_flight_`，确保启动 PPP/0x06 重放严格排在启动切速与保存之后。 |
| `tools/main_host_check/GnssResetCheck.cpp` | 新增 `s_bootBaudMode` / `s_verifiedAfterBaud` / `s_saveAfterVerify` 验证 `SAVECONFIG` 严格在切速 `VERSIONA` 验证之后发出且不写 `gnss/baud`；更新 `testGnssFactoryReset()` 与 `testGnssTargetBaudPolicy()` 用例 1/2/3/4a/4b/8 断言（发生切速时恰好 1 次 `SAVECONFIG`，已在目标速率时 0 次）；新增用例 9 验证 `SAVECONFIG` NACK 时 `baud_fail` 仍为 0、保持 460800 且状态 `running`。 |

## 2. 互斥与驱动行为核查

- **仅在发生切速且验证成功时保存**：`alignBootBaud()` 若 `detected().baud == target`，仅 `syncBootHint()` 后直接返回，不触发保存；切速超时或 NACK 时不触发保存。
- **不写 NVS `gnss/baud`**：`completeBootBaudMaintenance()` 只调用 `syncBootHint()` 更新 `gnss.cache`，不写用户覆盖键 `gnss/baud`；用户 `gnss baud N` 与 `gnss freset yes` 行为不变。
- **与其他维护互斥**：
  - 保存复用 `controller().requestSave()`，`Controller` 处于 `kMaintenance`，`active_maintenance_ = Maintenance::kSave`；
  - `GnssWorkMode::pollWorkMode()` 与 `GnssModulePpp::canStartPhonePpp()` 均显式等待 `boot_baud_save_in_flight_` 清零；
  - `cmdSetRate`/`cmdSetOut`/`cmdBaud` 在 `kMaintenance(kSave)` 下按既有逻辑自动延后（`apply_deferred_`/`baud_deferred_`），待保存结束后自动执行；
  - `cmdMaintenance`（手动 `save`/`freset`）在非 `kRunning` 时返回 `kBusy`，`save_queue_` 仅在 `kRunning` 消费，零交叠。
- **多驱动支持**：完全复用各驱动既有的 `saveStep()`（Unicore/Optisens CM680 发 `SAVECONFIG\r\n`，u-blox/ComNav 沿用已有保存步骤；无 `saveStep()` 返回 `kUnsupported` 时静默跳过）。

## 3. 门禁与验证结果

执行命令：
```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
. "$IDF_PATH/export.sh" >/dev/null
tools/ci/run_gates.sh --base 215851d --prefix gq
```

| 门禁 | 结果 | 备注 |
| --- | --- | --- |
| `G1-frozen` | `FAIL`（预期） | 仅 `G-optisens-family` 授权的 2 个冻结头（`Types.hpp`、`UnicoreReceiver.hpp`）；本卡零冻结文件改动 |
| `G2-host` / `G2b-host-asan` | `PASS` | — |
| `G2-app_fw` / `G2b-app_fw-asan` | `PASS` | — |
| `G2-services` / `G2b-services-asan` | `PASS` | — |
| `G2-stream_demux` / `G2b-stream_demux-asan` | `PASS` | — |
| `G2-bt_reconnect` / `G2b-bt_reconnect-asan` | `PASS` | — |
| `G2-cli` / `G2b-cli-asan` | `PASS` | — |
| `G2-gnss` / `G2b-gnss-asan` | `PASS` | — |
| `G2-gnss-all` / `G2b-gnss-all-asan` | `PASS` | — |
| `G2-nvs_store` / `G2b-nvs_store-asan` | `PASS` | — |
| `G2-web_proto` / `G2b-web_proto-asan` | `PASS` | — |
| `G2-main_host_check` / `G2b-main_host_check-asan` | `PASS` | 含冷启切速后 1 次 `SAVECONFIG`（晚于验证）、缓存命中 0 次、`SAVECONFIG` NACK 不计 `baud_fail` 且保速 `running` |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | `PASS` | — |
| `G5-format` | `PASS` | — |
| `G6-doc_check` | `PASS` | — |
| `G3-idf` | `PASS` | `rtk_main.bin` = 906192 B，0 warning |
| `G3-idf-trace-off` | `PASS` | 0 warning |
| `G3-idf-production` | `PASS` | 0 warning |
| `G3-idf-console-none` | `PASS` | 0 warning |
| `G3b-rtk_pure-esp32` / `esp32c3` | `PASS` | 0 warning |
| `G5b-tidy` | `PASS` | 0 warning / 0 error |
| `G7b-runtime` | `PASS` | 无堆新增、无异常/RTTI/`__cxa_guard_*` |
| HIL（实板） | `NOT_RUN` | 按授权边界不烧录、不连接 `/dev/ttyACM0`，交 Architect 验证 `gnss freset yes` 后首次复位切速+保存及再次冷上电首候选 460800 命中 |

## 4. 偏离与未解决问题

- 无契约偏离；无阻塞问题。
