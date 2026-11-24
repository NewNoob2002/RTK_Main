# T13F：配对事件按到达顺序处理报告

- 分支：`fix/t13-pairing-order`（基线 `main` HEAD：`3bbd6d270884ac6092ef9f10b825f074711495ae`）
- Profile：`esp-idf`（依据：`sdkconfig.defaults`、`idf_component_register`、`AGENTS.md` §2）
- 任务卡：`docs/tasks/T13F-pairing-order.md`
- 授权与冻结边界：未修改任何冻结文件（`sha256sum -c docs/frozen.sha256` PASS）；未执行 `git commit`/`push`、烧录、串口连接或联网下载。

## 1. 改动文件

| 文件 | 变更摘要 |
| --- | --- |
| `components/spp_server/private_include/SppServerPriv.h` | 在 `spp_server_ctrl_t` 新增 `uint32_t seq`（到达序号），在 `spp_server_state_t` 新增 `uint32_t ctrl_seq`（单调递增计数器）；更新 `spp_server_ctrl_process()` 注释说明按到达顺序处理。公共接口头未变。 |
| `components/spp_server/SppServerInit.c` | 在 `spp_server_objects_create()` 持锁复位区清零 `s_spp.ctrl_seq = 0U`。 |
| `components/spp_server/SppServerSession.c` | 在 `spp_server_ctrl_add()` 为新入表项与 `closed -> rearmed` 项分配 `++s_spp.ctrl_seq`；新增 `spp_server_ctrl_seq_before()`（无符号回绕安全比较 `(lhs - rhs) > (UINT32_MAX / 2U)`）、`spp_server_ctrl_ready_locked()` 与 `spp_server_ctrl_claim_oldest_locked()`；将 `spp_server_ctrl_process()` 改为每轮至多 `SPP_SERVER_CTRL_MAX` 步、每步锁内选取序号最小（最早到达）的就绪槽位处理，避免空槽复用或遍历游标越过前槽时后到事件插队。 |
| `tools/t13_host_check/T13HostCheck.c` | 新增确定性回归场景 `sc_gap_pairing_order_after_slot_free` 并注册为 `gap_pairing_order_after_slot_free`。 |
| `tools/t13_host_check/CMakeLists.txt` | 在 `T13_SCENARIOS` 中注册 `gap_pairing_order_after_slot_free`（CTest 用例名 `t13_gap_pairing_order_after_slot_free`）。 |
| `docs/PROGRESS.md` | 更新 §3 门禁项：移除已由 T13F 根治的 `t13_gap_pairing_window`，保留另案根因的 `t13_disconnect_reuse_across_slots` 记录并注明已定位的测试同步竞态根因。 |
| `docs/reports/T13F-pairing-order.md` | 本任务报告。 |

### 设计说明

- 生产代码改动严格限定在 `components/spp_server` 内，无堆分配、不新增锁或 RTOS 对象，不改变任何冻结接口。
- 序号比较使用纯无符号 32 位算术 `(lhs - rhs) > (UINT32_MAX / 2U)`，无有/无符号混用与窄化转换；因控制表深度 `SPP_SERVER_CTRL_MAX == 4`，任意两个在途项序号差远小于 $2^{31}$，跨 `UINT32_MAX -> 0` 回绕时严格保序。
- 已有在途同句柄 `REJECT` 的幂等重复添加保留原 `seq`；仅在 `closed == true` 被新 `SRV_OPEN` 重挂（`rearmed`）时刷新为新到达序号。

## 2. 确定性回归测试（修复前 FAIL / 修复后 PASS）

新增用例：`t13_gap_pairing_order_after_slot_free`（`tools/t13_host_check/T13HostCheck.c:853-894`）。

构造方法（零 `sleep`，完全确定性）：
1. 打开两个会话（`0x11`、`0x22`）占满会话表，使用 `FAKE_GATE_DISCONNECT` 暂停 worker。
2. 在 BTC FIFO 中依次注入：
   - `SRV_OPEN(0x33)`（无空槽，落入 `ctrl[0]` 成为 `REJECT`）；
   - `CFM_REQ(s_bda)`（进入 `SPP_SERVER_PAIR_CONFIRM` 窗口，落入 `ctrl[1]`）；
   - 同步 `CLOSE(0x33)`（等待 BTC 顺序执行完 `CFM_REQ` 与 `CLOSE`，释放 `ctrl[0]`）；
   - `PIN_REQ(other)` + 同步 `CLOSE(0x99)` 屏障（确保后到的 `PIN_REQ` 落入刚释放的 `ctrl[0]`）。
3. 放行 worker，断言先到的 `CFM(s_bda)` 占有配对确认窗口、后到的 `PIN(other)` 立即被拒绝，且 `spp_server_pair_decide(true)` 放行 `s_bda`。

| 阶段 | 构建 | 命令 | 结果 | 关键输出 |
| --- | --- | --- | --- | --- |
| 修复前 | 普通 | `builds/t13f-preflight-t13/t13_host_check gap_pairing_order_after_slot_free` | **FAIL**（预期） | `Pair request from A1:02:03:04:05:06`（后到 PIN 抢占窗口）；`FAIL ...:885/887/888/891`（4 checks failed，exit 1） |
| 修复前 | ASan/UBSan | `ASAN_OPTIONS=detect_leaks=0 builds/t13f-preflight-t13-asan/t13_host_check gap_pairing_order_after_slot_free` | **FAIL**（预期） | 同上：4 checks failed，exit 1 |
| 修复后 | 普通 | `builds/t13f-preflight-t13/t13_host_check gap_pairing_order_after_slot_free` | **PASS** | `Pair request from AA:BB:CC:DD:EE:FF, code 000000`；`PASS gap_pairing_order_after_slot_free` |
| 修复后 | ASan/UBSan | `ASAN_OPTIONS=detect_leaks=0 builds/t13f-preflight-t13-asan/t13_host_check gap_pairing_order_after_slot_free` | **PASS** | 同上：`PASS gap_pairing_order_after_slot_free` |

## 3. `t13_disconnect_reuse_across_slots` 偶发失败排查

### 结论

**不是同一根因**，不由 T13F 覆盖。

### 代码分析与复现证据

1. **与控制表排序无关**：在 `sc_disconnect_reuse_across_slots`（`tools/t13_host_check/T13HostCheck.c:1026-1045`）中，控制表自始至终只登记过 1 条条目（`ctrl[0]`：`SPP_SERVER_CTRL_REJECT` for `0x11`），不存在两条控制项竞争处理顺序的问题。
2. **真实根因是测试替身与 worker 结算之间的同步竞态**：
   - `FakeIdf.c:1208` 中，`esp_spp_disconnect()` 在函数返回前就执行 `++s_disconnect_return_count` 并唤醒等待线程。
   - `T13HostCheck.c` 中 `fake_wait_disconnect_returns(2, 1000U)` 返回后，测试主线程立刻执行 `fake_emit_close(0x11U)` 与 `open_session(0x11U)`，此时 worker 线程在高负载/ASan 下可能尚未执行 `SppServerSession.c` 中锁内的 `spp_server_ctrl_settle_locked(slot, err)`（即 `ctrl[0].submitting` 仍为 `true`）。
   - `spp_server_ctrl_drop_reject(0x11U)` 见 `submitting == true`，按安全契约只置 `ctrl[0].closed = true` 并保留 `ctrl[0].in_use = true`（防止 SDK 断开未结算时复用同数字句柄，该行为本身正是 `sc_reject_rearm_after_close` 验证的必选保护）。
   - 紧随其后的 `open_session(0x11U)` 命中 `spp_server_reject_pending_locked(0x11U) == true`，被驱动拒绝并重挂 `ctrl[0]`（触发第 3 次 `esp_spp_disconnect`），导致 `open_session(0x11U)` 在 `wait_event(SPP_SERVER_EVT_OPEN, 0x11U, 1000U)` 处超时失败。
3. **压测复现验证**：在修复前 ASan 构建下并行压测 100 次（`xargs -P 8` 叠加 IDF 编译负载），第 35 次精确复现：
   - `FAIL T13HostCheck.c:101: wait_event(SPP_SERVER_EVT_OPEN, handle, 1000U)`
   - `FAIL T13HostCheck.c:1043: spp_server_disconnect(0x11U) == ESP_OK`
   - 同时后续的 `fake_wait_disconnect_returns(3, 1000U)` 却通过了，直接证实第 3 次断开正是来自被重挂的 `REJECT`。
4. **`docs/PROGRESS.md` 处理**：因 `t13_gap_pairing_window` 根因已由 T13F 彻底修复，而 `t13_disconnect_reuse_across_slots` 属于上述独立测试同步竞态（且任务卡要求生产改动限于 `components/spp_server`），故从 `docs/PROGRESS.md` §3 删除了 `t13_gap_pairing_window`，保留并精确更新了 `t13_disconnect_reuse_across_slots` 的待处理记录以便另开卡修正测试同步。

## 4. 门禁与验证结果

| 检查项 | 命令 | 结果 | 备注 |
| --- | --- | --- | --- |
| 全量 CI 门禁 | `ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix t13f --base 3bbd6d270884ac6092ef9f10b825f074711495ae` | **PASS** | **43 passed, 0 failed**（日志目录 `builds/t13f-logs/`） |
| 冻结文件校验 | `sha256sum -c --quiet docs/frozen.sha256`（含于 `G1-frozen`） | **PASS** | 0 差异 |
| 格式门禁 | `clang-format --style=file --dry-run --Werror ...`（含于 `G5-format`） | **PASS** | 0 差异 |
| 文档注释门禁 | `python3 -I tools/doc_check/doc_check.py --base 3bbd6d270884ac6092ef9f10b825f074711495ae`（含于 `G6-doc_check`） | **PASS** | 改动函数 6 个，不合格 0 个 |
| 静态检查 | `G5b-tidy`（含于 `run_gates.sh`） | **PASS** | 0 告警/错误 |
| 运行时与依赖门禁 | `G7b-runtime`、`G7c-no-cli-deps`（含于 `run_gates.sh`） | **PASS** | 无新增堆符号/受禁运行时 |
| `t13_host_check-asan` 连跑 20 次 | 连续 20 次 `ASAN_OPTIONS=detect_leaks=0 ctest --test-dir builds/t13f-t13_host_check-asan --output-on-failure` | **PASS** | **20/20 passed, 0 failed**（含新增 `t13_gap_pairing_order_after_slot_free` 与全部 46 项 t13/k2 用例） |
| 硬件实测（HIL） | — | **NOT_RUN** | 遵守 AGENTS.md §6：不烧录、不碰串口 |

`run_gates.sh --prefix t13f` 分项明细（43/43 PASS）：
- `G1-frozen`：PASS
- `G2-*` + `G2b-*-asan`（`host`、`app_fw`、`services`、`stream_demux`、`bt_reconnect`、`cli`、`gnss`、`gnss-all`、`nvs_store`、`web_proto`、`settings`、`ota`、`main_host_check`、`t13_host_check`，共 28 项）：全部 PASS
- `G5-format`、`G6-doc_check`、`G5b-tidy`、`G7b-runtime`、`G7c-no-cli-deps`（共 5 项）：全部 PASS
- `G3-idf`、`G3-idf-trace-off`、`G3-idf-wifi-off`、`G3-idf-ota-off`、`G3-idf-production`、`G3-idf-console-none`、`G3-idf-none-wifi`、`G3b-rtk_pure-esp32`、`G3b-rtk_pure-esp32c3`（共 9 项，均 0 warning）：全部 PASS

## 5. 体积差

对比同基线未改动构建 `builds/t13f-base-idf` 与改动后构建 `builds/t13f-idf`（均基于 ESP-IDF v6.1，`esp32`）：

| 指标 | 基线（`t13f-base-idf`） | 修复后（`t13f-idf`） | 增量 |
| --- | ---: | ---: | ---: |
| `rtk_main.bin` 磁盘大小 | 1,528,272 B | 1,528,336 B | **+64 B**（含 16 B 对齐，槽余量 22%） |
| `idf.py size` Total image size | 1,528,162 B | 1,528,218 B | **+56 B** |
| Flash Code（`.text`） | 1,094,648 B | 1,094,704 B | **+56 B** |
| Flash Data（`.rodata` 等） | 265,324 B | 265,324 B | **+0 B** |
| IRAM | 116,139 B（余 14,933 B） | 116,139 B（余 14,933 B） | **+0 B** |
| DRAM（`.data` + `.bss`） | 96,763 B（`.bss` 44,752 B） | 96,787 B（`.bss` 44,776 B） | **+24 B**（余 27,793 B） |

`cpp_cost.py diff builds/t13f-base-idf/rtk_main.elf builds/t13f-idf/rtk_main.elf` 关键摘要：
- `text: 1225695 -> 1225751 (+56)`，`data: 317119 -> 317119 (+0)`，`bss: 44782 -> 44806 (+24)`
- `flash: 1542814 -> 1542870 (+56)`，`ram: 361901 -> 361925 (+24)`
- 主要符号增量：`spp_server_ctrl_process +116 B`、`spp_server_ctrl_add +12 B`、`s_spp +20 B`（4 个控制槽 `seq` + `ctrl_seq`，共 5 × 4 B）。

## 6. 未验证项与风险

- **真机双设备并发配对 HIL**：`NOT_RUN`（受 `AGENTS.md` §6 限制，未烧录与连接硬件）。逻辑与跨线程时序已由 host 驱动级回归（`t13_gap_pairing_window` 与 `t13_gap_pairing_order_after_slot_free`）及 20 轮 ASan/UBSan 覆盖。
- **`uint32_t` 序号回绕端到端测试**：`NOT_RUN`（`t13_host_check` 为黑盒测试，不直接修改 `s_spp.ctrl_seq` 私有字段）；回绕逻辑 `(lhs - rhs) > (UINT32_MAX / 2U)` 为纯无符号 32 位算术，已通过静态代码审查与 `-Wall -Wextra -Werror` 编译检查。
- **`t13_disconnect_reuse_across_slots` 高负载偶发失败**：如 §3 所述为测试替身与 worker 结算的独立同步竞态，非本卡范围，建议另开卡在 `t13_host_check` 修正同步屏障。
