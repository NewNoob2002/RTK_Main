# MT1：频率保存一致性（RU2-02）与 t13 测试同步

- 分支：`feat/mt1-maint`，基线 `2ae43a9`；任务卡 `docs/tasks/MT1-rate-save-t13-sync.md`
- 说明：`docs/reports/U2-review.md` 不存在，按 `docs/tasks/U2-ublox-rate.md` 与 T13F 报告 §3 实施。

## 改动文件

- `main/modules/gnss/GnssModule.hpp`、`GnssModuleProbe.cpp`、`GnssModuleCmds.cpp`：新增 `persistRateWithOut()`，
  非运行态 `cmdSetRate` 与两阶段 `tickPendingApply` 共用；rate 保存成功、out 保存失败时把 RAM rate 改回旧值并补偿写回 NVS rate；
  补偿失败 `FW_LOGE`，依赖启动自愈。无新增数据成员。
- `tools/main_host_check/shim/NvsShim.c`、`nvs.h`：新增按 key 的写失败注入 `nvs_shim_add_key_fail/clear_key_fail`（含 `nvs_set_blob`）。
- `tools/main_host_check/GnssResetCheck.cpp`：新增用例（见下）。
- `tools/t13_host_check/shim/FakeIdf.c`、`FakeRtos.c`、`shim/inc/freertos/FreeRTOS.h`：测试同步修复。
- `docs/PROGRESS.md`：§3 两条更新。

## 卡片 A：结论

- 启动自愈（已核实）：F9P 的 `restoreOutputPeriods` → `alignF9pOutputConfig(rate)` 按 NVS 中 rate 重新对齐 out 并经
  `persistAlignedF9pOutput` 落盘，因此“新 rate + 旧 out”会自愈（`safeDefaults` 或用户 NVS 未就绪时不落盘，仅 RAM 对齐）。
  非 F9P（UM980/CM680）的 out 与 rate 无关（`restoreLegacyOutputPeriods` 仅按接收机支持周期钳位，`cmdSetRate` 不写 out），不存在不一致。
- 非运行态用例处于型号未知态（kReset 后 kPowerUp，`start()` 清空 `detected_`），不触发 F9P 的 out 联动；
  若控制器在 kOff/kFailed 仍保留 F9P 型号，该分支同样走共用的 `persistRateWithOut`，out 联动与补偿由运行态两阶段路径覆盖。
  非运行态用例验证 rate 单键保存、out 不被改写、rate 写失败时 NVS 与 RAM 都回滚（RAM 通过再次 `saveKeys` 读回 rover_cfg_ 验证）。
- 新增 host 用例（`verifyF9pRateOutputAndRollback` 5b/5c/6b、`verifyNonRunningRateSave`）：out 写失败→RAM 回滚、NVS rate 补偿回旧值、out 不变、`lastApply==kRestored`；
  补偿也失败→日志计数 +1、NVS 残留新 rate；从该残留启动→RMC 周期对齐为 1000 ms 且 NVS out 被修正。
  对照：临时禁用补偿后 5b 用例失败（`nvsRate == 1U`），已还原。

## 卡片 B：结论

- 根因：`esp_spp_disconnect()` 在返回前递增 `s_disconnect_return_count`，早于 worker 随后的 `spp_server_ctrl_settle_locked`，
  用例过早注入 CLOSE/OPEN，`ctrl[].submitting` 仍为 true。
- 修复（仅测试 shim）：返回计数改由线程局部标记 + 临界区退出钩子在调用线程完成随后第一段临界区后递增；
  钩子在 `fake_reset()` 中经 `fake_crit_exit_hook_install()` 一次性安装（`_Atomic` 指针，release/acquire），
  `fake_reset()` 同时清除本线程遗留的 pending 标记（审查 Important 1 / Nit 2）；未改 `components/spp_server`，未削弱用例。
- 复现命令：ASan 构建 + 24 个空转进程负载，`seq N | xargs -P 24 t13_host_check disconnect_reuse_across_slots`。
  修复前 300 次失败 2 次；修复后 1000 次失败 0 次（`reject_rearm_after_close` 另 300 次 0 失败）。

## 验证

| 项 | 结果 |
| --- | --- |
| `tools/ci/run_gates.sh --prefix mt1 --base b14825c` | PASS（43 passed, 0 failed，含全部 ASan、format、doc_check、G3 IDF 各配置、G7b/G7c） |
| 资源 | `idf.py size`：IRAM、.data、.bss 与基线构建（`../../RTK_Main/.../mainv-idf`，同 2ae43a9）完全一致，DRAM 增量 0 B，IRAM 增量 0；镜像 +288 B（flash；rtk_main.bin 1586176 B 对基线构建 1585888 B）；无运行期堆 |
| TSan（`-fsanitize=thread -g -O1` 构建 `tools/t13_host_check`） | 46 项中 45 PASS、无 `fake_crit_exit_hook` 相关报告；`t13_init_start_timeout` 报 `s_spp` 数据竞争（`SppServerInit.c:41` 与 `SppServerWorker.c:126`），在基线 `2ae43a9` 同样构建下完全相同地失败，为既有问题，未在本卡处理 |
| ASan t13 | 46/46 PASS；`disconnect_reuse_across_slots` 1000 次 `-P 64` 0 失败 |
| 真机/串口/烧录 | 实现阶段 NOT_RUN（任务禁止）；验收 2026-11-26 P2 v1.41（ZED-F9P）：`gnss rate 5` 后 `gnss.out.gga=200`，复位后 12 s 内 gga=41 条（5 Hz 保持）、`gnss.out.gga=200`；`gnss rate 1` 恢复 `gnss.out.gga=1000` |

## 偏离与未解决

- 无冻结文件改动；`docs/PROGRESS.md` 按卡片授权改动。
- NVS 两键写入仍非事务，补偿写入也失败时靠 F9P 启动自愈（真机未测）。
