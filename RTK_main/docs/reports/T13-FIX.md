# T13 修复报告（复审 A–F 与追加反馈）

日期：2026-11-10。实现者：omp + DeepSeek-V4.1-Flash。人工负责人：UNKNOWN（待人工审查签字）。

本报告记录 BATCH-E 复审 A–F 与后续交付复审的修复。按协调约定（主窗口指示）：前一轮先不并发编辑、等主窗口检查结果；主窗口授权后，本窗口已执行定向 host 构建、22 场景 ctest、ASan/UBSan、目标构建与格式检查，结果见文末。

## 结论

- 22/22 驱动级回归 **PASS**；ASan/UBSan 下 22/22 **PASS**；`idf.py build` **PASS**（`rtk_bt_spp.c` 实际重编译，0 warning/0 error）。
- `clang-format --style=file --dry-run --Werror components/rtk_bt/rtk_bt_spp.c` **PASS**；冻结校验 **PASS**；冻结补丁 `git apply --check` **PASS**（未应用）。
- 后续用户已明确批准冻结例外：主窗口已应用 #5（`static_event_group.hpp`）修复与 #6（`static_task.hpp`）注释更正，并更新两条冻结哈希；真实头的行为、Xtensa 实例化、格式与冻结校验均通过，最终独立结论见 `T13-FIX-REVIEW.md`。下方补丁保留为提交审阅时的记录。

## 改动文件

| 文件 | 内容 |
|---|---|
| `components/rtk_bt/rtk_bt_spp.c` | A–E、追加 1–4 的驱动修复 |
| `tools/t13_host_check/shim/fake_idf.c`、`fake_idf.h` | 修 `fake_dispatch` 缺参；INIT/START 门闩与同步顺序；`esp_spp_write` 入口门闩与返回计数；GAP 事件注入与应答计数；短写模式；等待/唤醒辅助 |
| `tools/t13_host_check/shim/fake_rtos.c`、`shim/inc/freertos/FreeRTOS.h` | 确定性读暂停钩子（在缓冲锁之前） |
| `tools/t13_host_check/t13_host_check.c` | 22 个场景；C11 atomic 完成标志；删除假前提与 sleep 猜时序 |
| `tools/t13_host_check/CMakeLists.txt` | 场景表 22 项；为测试目标补 `shim/` include 目录（`fake_idf.h` 在 shim 根，不移动头文件） |
| `/tmp/rtk-t13-frozen-fix.patch` | 冻结 #5/#6 补丁（已按 E 修正，未应用） |
| `docs/reports/T13-FIX.md` | 本报告 |

## 逐项修复

### A. `rtk_bt_spp_wait_stage` 按值收 status（致命）

- 改为按阶段枚举等待，事件到达后**在同一把 portMUX 下读取实际阶段状态**：`rtk_bt_spp_stage_bit`（`rtk_bt_spp.c:1088`）、`rtk_bt_spp_wait_stage`（`:1103`）。
- 回调先写状态、再置位：`rtk_bt_spp_handle_init`（`:699`）、`rtk_bt_spp_handle_start`（`:713`）。
- 发起每个阶段前复位该阶段状态并清完成位：INIT/START 在 `rtk_bt_spp_init:1223` 附近，START 在 `rtk_bt_spp_bringup:1139–1145`，UNINIT 在 `rtk_bt_spp_spp_deinit:1176–1178`。同步到达与异步到达两种顺序均成立。
- INIT 与 START 都校验；START 阶段异步失败单独回归（`init_start_status_fail`）。

### B. write 的 0 预算先于会话确认

- `rtk_bt_spp_write:1403–1411`：参数 → 初始化状态 → `session_acquire`（未知句柄 `NOT_FOUND`）→ 0 预算返回 `ESP_ERR_TIMEOUT` 且不提交。
- 回归：`unknown_handle_errors`（测试 `:688`）同时断言未知句柄 + 0 预算 → `NOT_FOUND`，已知会话 + 0 预算 → `TIMEOUT` 且 `fake_write_call_count()==0`。

### C. 分块共用整次预算

- 新增 `rtk_bt_spp_budget_left`（`:175`）。
- `rtk_bt_spp_write_chunk:1291` 认领前、`:1333` 复制前各检查一次；复制前失败会把 `RESERVED` 交还 `IDLE`。
- `rtk_bt_spp_write:1419` 在每块开始前检查，块间预算耗尽不再提交下一块。
- 回归：`write_chunk_budget_shared`（测试 `:658`）第一块与预算同值（400 ms），只提交 1 块、总字节 512、超时；第一块是否进入 IDF 用 `fake_wait_write_calls` 确认。

### D. GAP 回调不直接投递阻塞 API

- 新增静态控制表 `s_ctrl` 与 `rtk_bt_spp_ctrl_add`（`:368`）：回调只复制 bda/句柄并在临界区外 `xTaskNotifyGive`。
- `rtk_bt_spp_gap_cb:941` 只登记 PIN/CFM；发送任务 `rtk_bt_spp_ctrl_process:408` 在任务上下文调用 `esp_bt_gap_pin_reply`/`esp_bt_gap_ssp_confirm_reply`。
- SRV_OPEN 无安全空槽（`rtk_bt_spp_handle_srv_open:729`）登记 `REJECT`，由同一任务调用 `esp_spp_disconnect`；回调内不调用。
- `ctrl_process` 在执行拒绝前用 `rtk_bt_spp_session_find` 复查句柄；CLOSE（`:751`）会撤销尚未取走的同句柄拒绝（`rtk_bt_spp_ctrl_drop_reject`）。取走后的极小窗口无法撤销，已在注释说明。
- 回归：`gap_replies_from_task`（测试 `:709`）断言应答计数、bda 原样、`fake_blocking_api_from_btc()==false`；`open_no_slot_rejects_from_task` 断言断开来自任务上下文。

### E. 冻结补丁语义与注释

- `static_event_group.hpp`：成功时返回 `xEventGroupWaitBits` 的**完整快照 `got`**，只在等待条件未满足时返回 0；接口注释不再承诺只返回请求位。
- `static_task.hpp`：删除 Generic FreeRTOS 推测，只陈述已核实事实（IDF 目标 `StackType_t` 为 `uint8_t`，返回值本来就是字节数；乘法是恒等换算）。
- 补丁内容见文末；文件在 `/tmp/rtk-t13-frozen-fix.patch`，`git apply --check` 通过，**未应用**。

### F. 测试框架编译与数据竞争

- `fake_idf.c` 的 `ESP_SPP_UNINIT_EVT` 投递补上第三个 `sync=false` 参数；其余 `fake_dispatch` 调用点已核对。
- `fake_idf.h` 在 `shim/` 根，CMake 只传播了 `shim/inc`：为 `t13_host_check` 目标补 `PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim"`，未移动头文件、未改无关文件。
- writer/reader/init 的跨线程完成标志由 `volatile bool` 改为 `atomic_bool`（release/acquire）。
- 假 IDF 的 `s_write_block_ms` 在锁内快照，避免竞争。

## 追加复审 1：删除“延后接纳”，改为拒绝

- 删除 `s_pending`、`rtk_bt_spp_pending_*`、`rtk_bt_spp_worker_next_wait` 与 1 s 重试逻辑；`rtk_bt_spp_session_alloc` 只在 BTC 的 SRV_OPEN 中调用。
- 没有“users==0 且无在途发送且非 owner”的安全空槽时登记 `REJECT`，由发送任务断开。不再出现 CLOSE 后的幽灵会话，也不会在待接入期间静默丢 DATA。
- 回归：`slot_held_rejects_new_connect`（测试 `:270`）先占满两个槽，用 shim 的确定性读暂停挂住**真实 `rtk_bt_spp_read`**，交错 CLOSE 与新 OPEN；断言新连接被任务上下文拒绝、读者仍读到原数据（未被 reset）、退出后不补发 OPEN。

## 追加复审 2：短写、匿名 WRITE_EVT、全局单提交

- `rtk_bt_spp_settle_write:796`：`status != SUCCESS` 或 `len != tx_len` 都按 `ESP_FAIL`；成功路径 len 是 RFCOMM 实际接受长度（`bta_jv_act.c:2412–2420` → `btc_spp.c:1136`）。
- 全驱动同一时刻至多一个未完成 IDF 提交：`s_tx_inflight`/`s_tx_inflight_cancelled`（`:171–172`）。`rtk_bt_spp_worker_submit:603` 在临界区认领；完成/提交失败/取消释放；释放后非阻塞唤醒 worker 重新扫描排队请求。
- 匿名错误只允许 `handle==0`；见追加 3 的状态判别。带有效句柄但不属于当前在途请求的迟到成功/失败一律忽略（`handle_write:815`）。
- 回归：`write_stale_failure_ignored`（`:626`）、`write_unmatched_no_resource`（`:574`）。

## 追加复审 3：CLOSE 是取消完成（本轮新增）

证据：CB 模式 CLOSE 先 `spp_free_slot` → `free_slot_data` → `fixed_queue_free` 清掉未完成 TX，再通知 CLOSE（`btc_spp.c:1188–1204`、`:277–298`、`:121–124`），不逐条合成 WRITE；BTA 关闭会清空 port/handle 并可能清 `p_cback`（`bta_jv_act.c:367–379`、`:431–436`），`p_cback` 为空时不回调（`:2413–2416`）。因此不能把“迟到 WRITE”当作释放 owner 的唯一条件。

- CLOSE 命中 owner 且 `tx_submitting==false`：立即释放 owner、清 cancelled、唤醒 worker；写者已由 CLOSE 结算为 `INVALID_STATE`。
- CLOSE 命中 owner 且 `tx_submitting==true`（worker 仍在 `esp_spp_write`，静态 `tx_buf` 不可复用）：只置 `cancelled=true`，继续占住 owner 与该槽；worker 从 API 返回后释放（`worker_submit:653` 附近）。若 API 返回错误则按提交失败结案。
- CLOSE 后旧请求才入队的匿名失败必为 `ESP_SPP_NO_CONNECTION`（槽已消失，`btc_spp.c:900–943`）：**忽略**，不结算当前 owner。匿名 `NO_RESOURCE` 等仍作为当前唯一在途提交的真实失败归属 owner。
- 非零旧/未知句柄的迟到事件一律忽略；`handle_write` 的旧 `stale_owner_done` 分支已删除。
- owner 未释放前该槽不可复用（`rtk_bt_spp_session_alloc:255–259`），避免静态缓冲/迟到事件污染新会话。
- 回归：
  - `write_cancel_by_close`（测试 `:456`）：0x11 写进入 IDF 后 HOLD → CLOSE → **不投递任何旧 WRITE** → 0x22 写成功；旧槽可被 0x33 复用；迟到旧事件不影响 0x22。旧实现会卡到超时。
  - `write_cancel_during_submit`（测试 `:498`）：`esp_spp_write` 入口门闩挂住 worker → CLOSE（保存取消）→ 0x22 排队但不得提交 → release 后 owner 释放、0x22 提交 → 投递旧请求的匿名 `NO_CONNECTION` 被忽略 → 0x22 只在自己的完成事件后返回 `ESP_OK`。
  - `stale_completion_after_reopen`（测试 `:552`）：CLOSE 后无需任何旧 WRITE 即可复用槽并写入；迟到旧成功/失败均被忽略。

## 追加复审 4：回归质量

- `init_waits_stages`（测试 `:129`）用 `fake_gate_*` 门闩：INIT 未到时 init 不得返回；START 未到时仍不得返回；都放行后才 `ESP_OK`。无 sleep 猜进度。
- `init_sync_stage_order`（`:185`）覆盖“回调早于等待返回”的顺序。
- 所有“提交是否发生”类断言改用 `fake_wait_write_calls`/`fake_wait_write_returns`/`fake_wait_disconnect_calls`/`fake_wait_*`；只有预算到期用例需要真实时间，并按契约上界断言。
- `write_cancel_during_submit` 中的 100 ms 负向检查只用于确认“owner 占用时第二块未提交”，随后有确定性的门闩/完成事件断言兜底。

## 人工审查清单（T13 任务要求）

| 检查项 | 结论与位置 |
|---|---|
| 回调中没有阻塞调用 | BTC 回调只做拷贝/状态/置位/`xQueueSend(...,0)`/`xTaskNotifyGive`；GAP 应答与断开在任务上下文（`ctrl_process:408`）。 |
| 临界区内没有 FreeRTOS API | 全部 `taskENTER_CRITICAL` 段已逐段核对；`xStreamBufferReset` 在 `session_alloc` 的临界区之外；唤醒/置位都在临界区外。 |
| 所有 `esp_err_t` 都检查 | 初始化链、GAP 应答、`esp_spp_write`/`disconnect`/`start_srv`/`deinit` 均检查并记录；无 `(void)esp_*`。 |
| write 等待有上界且递减 | `rtk_bt_spp_budget_left:175` 与 `rtk_bt_spp_wait_change` 共用 `start/timeout_ms`；认领、复制、排队、完成等待均受同一预算约束。 |
| 初始化失败按逆序释放 | `rtk_bt_spp_init:1190` 失败路径：SPP deinit → Bluedroid → 控制器 → 静态 RTOS 对象；`unwind`。 |
| 没有自有堆分配 | 会话/流缓冲/队列/事件组/控制表/发送任务栈与 TCB 全是文件作用域静态存储；无 `malloc`/`new`。 |
| RAII 构造 constexpr、不访问 RTOS | 本轮未改 `rtk_os` 生产头；冻结补丁不改变构造/init 契约。 |

## 冻结补丁（未应用）

```diff
--- a/components/rtk_os/include/rtk/os/static_event_group.hpp
+++ b/components/rtk_os/include/rtk/os/static_event_group.hpp
@@ -43,9 +43,17 @@
-    /** 等待事件位；超时返回 0。未初始化返回 0。 */
+    /**
+     * 等待事件位；成功返回等待返回时的事件组快照（可能含本次未请求的位），超时/未满足返回 0。
+     * 未初始化或 bits 为 0 返回 0；wait_all 为 true 要求 bits 全部置位，为 false 要求至少一位。
+     * xEventGroupWaitBits 超时时返回当前已置位的事件位（可能是请求位的真子集），因此这里按语义判定。
+     */
     [[nodiscard]] EventBits_t wait(EventBits_t bits, bool clear_on_exit, bool wait_all, uint32_t timeout_ms) noexcept
     {
-        if (handle_ == nullptr) {
+        if ((handle_ == nullptr) || (bits == 0U)) {
             return 0;
         }
-        return xEventGroupWaitBits(handle_, bits, clear_on_exit ? pdTRUE : pdFALSE, wait_all ? pdTRUE : pdFALSE,
-                                   pdMS_TO_TICKS(timeout_ms));
+        const EventBits_t got = xEventGroupWaitBits(handle_, bits, clear_on_exit ? pdTRUE : pdFALSE,
+                                                  wait_all ? pdTRUE : pdFALSE, pdMS_TO_TICKS(timeout_ms));
+        const EventBits_t wanted = got & bits;
+        const bool satisfied = wait_all ? (wanted == bits) : (wanted != 0U);
+        // 成功时保留完整快照 got；只有等待条件未满足才返回 0。
+        return satisfied ? got : 0U;
     }
--- a/components/rtk_os/include/rtk/os/static_task.hpp
+++ b/components/rtk_os/include/rtk/os/static_task.hpp
@@ -72,5 +72,6 @@
         }
-        // IDF v6.1 内核内部按 StackType_t 字数返回（task.h 注释写作字节，与实现不符），
-        // 这里换算成字节，使返回值与接口注释一致。字数 ≤ StackBytes / sizeof(StackType_t)，
-        // 32 位目标上乘 sizeof(StackType_t) 不会溢出。
+        // IDF v6.1 当前支持目标的 StackType_t 是 uint8_t（esp32 见 portmacro.h 的 typedef），
+        // uxTaskGetStackHighWaterMark() 按 StackType_t 计数，因此返回值本来就是字节数；
+        // 下面乘 sizeof(StackType_t) 是恒等换算，保留只为让接口按字节返回。
+        // 剩余量 ≤ StackBytes / sizeof(StackType_t)，32 位目标上乘法不会溢出。
         const uint32_t words = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle_));
```

## 实际执行的检查（本窗口）

| 检查 | 命令 | 结果 |
|---|---|---|
| 冻结校验 | `sha256sum -c docs/frozen.sha256 --quiet` | **PASS** |
| 冻结补丁可应用性 | `git apply --check /tmp/rtk-t13-frozen-fix.patch` | **PASS**（未应用） |
| host 构建 | `cmake -S tools/t13_host_check -B /tmp/rtk-t13-fix-host && cmake --build ... -j 4` | **PASS** |
| 驱动回归 | `ctest --test-dir /tmp/rtk-t13-fix-host --output-on-failure` | **PASS 22/22**，6.5 s |
| ASan/UBSan | `cmake -S tools/t13_host_check -B /tmp/rtk-t13-fix-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined ..." ... && ctest ...` | **PASS 22/22**，无 sanitizer 报告 |
| 目标构建 | `. ~/esp/esp-idf/export.sh; touch components/rtk_bt/rtk_bt_spp.c; idf.py build` | **PASS**；`rtk_bt_spp.c.obj` 实际重编译，0 warning/0 error |
| 格式 | `clang-format --style=file --dry-run --Werror components/rtk_bt/rtk_bt_spp.c` | **PASS** |
| 冻结 host 测试 | `cmake -S test/host -B /tmp/rtk-frozen-host && cmake --build ... && ctest ...` | **PASS 8/8** |
| HIL / 串口 / 烧录 | — | **NOT_RUN** |

## 风险与未验证

- CLOSE 取消了已进入 IDF 的请求后，旧消息若仍在 BTC 队列中，会以匿名 `NO_CONNECTION` 形式到达；本实现按状态忽略它，依赖“CLOSE 后槽已消失”的 IDF 路径（`btc_spp.c:900–943`）。若 IDF 行为变化，需要重新核对状态判别。
- 真正的断链若只报告匿名 `NO_CONNECTION` 而永不报告 CLOSE，owner 会一直保留到 CLOSE；会话生命周期本身依赖 CLOSE（冻结契约），未在硬件上验证。
- 拒绝命令被发送任务取走后再收到同句柄 CLOSE 的窗口无法撤销；`ctrl_process` 的在册复查可避免误断已接纳会话，但 IDF 复用同一句柄值时仍有极小窗口。
- `slot_held_rejects_new_connect` 的暂停点位于 shim 的 `xStreamBufferReceive` 取缓冲锁之前，刻意不模拟比 FreeRTOS 更强的互斥。
- 短写的具体错误码为 `ESP_FAIL`（映射为 `kIoError`）；冻结头未规定更细的错误码。
- 未做硬件验证：连接、双会话、拥塞、吞吐、重连与 scatternet 仍需 HIL。
