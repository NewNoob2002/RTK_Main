# MT1 · RU2-02 改频率 NVS 一致性 + t13_disconnect_reuse 测试同步

基于 main（`b14825c` 之后的本卡提交）开分支 `feat/mt1-maint`，worktree `../RTK_Main-mt1`。两项均为已定位根因的遗留，互不依赖。

## A RU2-02：改频率时 rate/out 分两次保存
- 现状：`main/modules/gnss/GnssModuleCmds.cpp:362-372`（非运行态直接落盘）与 `:426` 起（两阶段提交 `tickPendingApply`）先 `saveKeys(rate)` 再 `saveKeys(out)`；rate 成功、out 失败时 RAM 回滚旧频率，NVS 留下“新 rate + 旧 out”。来源 `docs/reports/U2-review.md`（RU2-02，commit `4ed2191`）。
- `StorageModule::saveKeys`（`main/modules/storage/StoreKeys.cpp:423`）逐键写，不具备事务性，单次调用两键也不能消除半写。
- 要求：
  1. 先确认开机路径 `restoreOutputPeriods → alignF9pOutputConfig`（`GnssModuleProbe.cpp:592-598`）是否已对“新 rate + 旧 out”自愈（F9P 与非 F9P/CM680 分别说明），用主机测试固定该结论。
  2. 保存失败时 NVS 必须回到变更前一致状态：out 保存失败后把旧 rate 写回（补偿写），补偿写也失败时记录 `FW_LOGE` 并依赖第 1 条的开机自愈；不得留下 RAM/NVS 不一致而无日志。两处路径共用一个辅助函数，避免重复。
  3. 主机测试：注入 out 键保存失败（现有 storage 假件/故障注入手段优先），断言 RAM 回滚、NVS rate 恢复旧值、`last_apply_` 为失败；补偿写失败分支有日志断言或计数。
- 不改：NVS 键名/布局、`docs/protocol/`、频率与周期的合法域。

## B `t13_disconnect_reuse_across_slots` 偶发失败
- 根因见 `docs/reports/T13F-pairing-order.md` §3：`tools/t13_host_check/shim/FakeIdf.c` 的 `esp_spp_disconnect()` 在返回前就递增 `s_disconnect_return_count` 并唤醒，测试在 worker 执行 `spp_server_ctrl_settle_locked` 前就发 CLOSE/OPEN。
- 要求：只改测试替身/测试同步（例如提供“worker 已结算该控制项”的等待点，或在 disconnect 返回后等待控制表 `submitting` 清除的可观测条件），**不改** `components/spp_server` 生产代码与其安全契约（`sc_reject_rearm_after_close` 保护不得弱化）。
- 验证：修复前在 ASan + 负载下能复现（记录命令与次数），修复后同条件 ≥200 次全过。

## 验收与约束
- 全量门禁 `tools/ci/run_gates.sh --prefix mt1 --base b14825c` 全通过（含 ASan）。
- DRAM 增量 ≤ 64 B，IRAM 0，无运行期堆。
- 授权修改：`main/modules/gnss/*`、`main/modules/storage/*`（仅为故障注入/测试接口所需）、`tools/main_host_check/*`、`tools/t13_host_check/*`、`test/*`、`docs/PROGRESS.md` §3 对应两条、`docs/reports/MT1-rate-save-t13-sync.md`（新建）。
- 不 push、不烧录、不连串口、不改 main；完成后提交（报告含改动、资源、测试、复现记录、NOT_RUN 项），单独一行输出 MT1-FINISHED。
