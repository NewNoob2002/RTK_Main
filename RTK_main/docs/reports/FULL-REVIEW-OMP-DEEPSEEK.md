# RTK_main 全工程复审（QA，只读）

日期：2026-11-11
执行：omp / `magpie/workbuddy/deepseek-v4.1-flash`（Herdr `wF:t4`，未切换模型）
角色：QA 只读复审；未修改产品源码、冻结输入、测试、配置；未执行 git 写操作、安装下载、串口或硬件动作。
范围：`main`、`components/rtk_proto`（C 核心 + C++ 外观）、`components/rtk_core`、`components/rtk_os`、`components/rtk_bt`（C 驱动 + C++ 适配器）、`components/rtk_store`、构建/Kconfig/sdkconfig/分区、host/目标测试、`tools/tidy_check`、`tools/g7b_check`。
依据：`AGENTS.md`、ADR-0002/0003、`docs/protocol/phone_protocol.md`、`docs/acceptance.md`、embedded-engineering（esp-idf profile）、ponytail-audit。所有结论先对照当前源码；旧报告只作背景，不直接采信。

## 0. 结论摘要

**软件复审结论：未发现协议编解码、C/C++ 边界、NVS 单次读写、静态资源封装或构建门禁的新的确定性错误；发现 4 项已证实、可达且违反现有实时/持久化契约的缺陷，必须修复。** 另有若干缺少实际可达时序的条件风险，单独列在 §4，不计入确定缺陷。

必须修复：

1. SPP 事件队列把 DATA 与 OPEN/CLOSE 混在同一 16 深度 FIFO；队列满时丢弃 CLOSE/OPEN，已由 `queue-probe.log` 复现。
2. `rtk_bt_spp_disconnect` 在链路任务上下文调用没有投递截止时间的 `esp_spp_disconnect`；SDK 等待会直接阻塞唯一链路任务。
3. 白名单持久化失败只打印日志，脏标志已清除且不重试；配对或清空后的白名单可能在重启后回退。0x44 的应答先于保存是既有接口时序，单独说明，不另算缺陷。
4. RX 预算按字节数（256 B）而不是时间/帧数约束；`core-probe.log` 已证 240 B、10 次 200 ms 超时写回占用 2000 ms，足以违反 HIL-3 的 ≤100 ms 应答和链路任务周期。

可选：BT 初始化失败时 `status` 的 policy 仍显示 CONCURRENT，而不是 Kconfig 选择值（仅诊断错误）。

## 1. 工程用途与负载假设

RTK_main 是 E1 RTK 基站的 ESP32 固件：经典蓝牙 SPP 服务端（最多 2 个会话），手机执行查询/设置，报警器发送 1 s 心跳并接收 0x40 状态推送；SN 与报警器白名单保存在 NVS；UART0 控制台用于 `status`/`pair`/`alarm`/`sn`/`wl`。

负载假设：两个 SPP 会话同时在线（CONCURRENT 策略），手机查询可能短时突发（探针取 240 B/10 帧），报警器 1 s 心跳/推送持续；所有队列、StreamBuffer、解析器和白名单均有界；经典 ESP32、8 MB Flash、无 PSRAM 依赖。

## 2. 证据快照与实际验证状态

| 项 | 状态 | 证据 |
| --- | --- | --- |
| 源码快照 | PASS | `docs/reports/logs/FULL-REVIEW-DEEPSEEK/sources.sha256`，96 文件；本次 `sha256sum -c` 全部 OK，日志与当前源码同一快照 |
| host 单元测试 | PASS | `/tmp/rtk-deepseek-full-review.W0mIk7/logs/host.log`：8/8 可执行文件通过；未逐用例计数 |
| T13 驱动回归 | PASS | `t13-host.log`：22/22 场景通过，6.53 s |
| 目标构建 | PASS | `idf-build.log`：全新 ESP32 构建，exit 0，0 compiler warning |
| 双架构纯组件构建 | PASS | `testapp-esp32.log`、`testapp-esp32c3.log`：均 exit 0，0 warning |
| G7b | PASS | `g7b.log`：17 个自有目标文件，白名单 6/6，exit 0 |
| G5b clang-tidy | PASS | `tidy-check.log`：27 文件，0 发现，exit 0 |
| 静态门禁 | PASS | `static-gates.log`：冻结文件、clang-format、纯组件 include、C 不含 `.hpp`、`extern "C"`、禁用特性 grep 均通过 |
| 体积 | PASS | `idf-size.log`：`rtk_main.bin` 625,328 B（0x98AB0）；size 工具段合计 625,204 B；app 分区 0x1A0000；IRAM 63.16%，DRAM 50.47% |
| ASan/UBSan | BLOCKED | `asan-ubsan.log`：构建成功；运行被 LeakSanitizer ptrace 限制阻断，EXIT 8。未关闭 sanitizer 重跑；不等于产品错误 |
| 权威 astyle_py | NOT_RUN | 需要联网下载/安装，按约束未执行 |
| `cpp_cost.py` | NOT_RUN | 本次未重跑；G7b 已按修订后的白名单门禁 PASS |
| HIL-0..6 | NOT_RUN | 未烧录、未连接串口、未执行 pytest-embedded；`rtk_main_guard` 拒绝 `/dev/ttyACM0` 芯片识别，未绕过；硬件由主线程独占 |
| 事件队列探针 | PASS（调查） | `queue-probe.log`：`queued_data=16 close_events=0 open_events=0 dropped=2 new_session_data=1 readable_bytes=1` |
| 核心链路探针 | PASS（调查） | `core-probe.log`：`disconnect_failure: calls=1 logical_role_none=1 retries_after_tick=0`；`request_burst: rx_bytes=240 writes=10 elapsed_ms=2000 caller_budget_bytes=256` |
| 断开阻塞探针 | PASS（调查） | 主线程 `disconnect-probe.log`：真实 C 驱动 + 既有 SDK/RTOS 替身，注入 SDK 等待 1500 ms，调用方实测等待 1500 ms 后返回 ESP_OK；不是实板测量 |
| 相同数字句柄迟到 WRITE 探针 | 条件证据 | `/tmp/rtk-same-handle-probe.log`：驱动在收到旧句柄 WRITE_EVT 时把新写者以 `ESP_FAIL` 立即结算（`new writer after stale old evt: err=-1 ms=0`）。该探针只证明驱动行为，不证明当前 IDF 会把该事件跨 CLOSE 送到新提交，见 §4.1 |

关于资源的说明：`idf-size.log` 的 DRAM `Remain 61,703 B` 是 DRAM 静态段剩余，不是全部运行时 heap；本次未据此外推资源缺陷。HIL 的运行时 heap/栈余量均为 NOT_RUN。

## 3. 确定缺陷（已证实可达，或源码路径无条件违反契约）

### 缺陷 1（Must fix / High）：事件队列满时丢弃 CLOSE/OPEN，链路会留下幽灵会话

**位置**
- `components/rtk_bt/include/rtk_bt_spp.h:23`：事件队列深度 16。
- `components/rtk_bt/rtk_bt_spp.c:474-494`：`rtk_bt_spp_evt_post()` 队列满时计数并直接丢弃。
- `components/rtk_bt/rtk_bt_spp.c:734-745`（OPEN）、`:756-763`（CLOSE）、`:765-795`（DATA/RX_DROP）都走同一队列。
- `main/app_link.cpp:353-355`：链路任务每轮只取一个事件。
- `main/app_link.cpp:179-184`：DATA 事件只设置 `drain_pending`；`:120-135` 的会话表只在 OPEN 记录、CLOSE 释放。

**可达输入/时序**
两个会话在线，高频 DATA 回调连续投递 16 个事件后，BTC 回调再投递 CLOSE（或新 OPEN）。`queue-probe.log` 已用当前源码快照复现：16 个 DATA 填满队列，CLOSE 与后续 OPEN 均未进入队列，`dropped=2`；驱动层新连接仍收到 1 字节数据，但应用层从未看到 OPEN。

**错误结果**
- CLOSE 丢失：`LinkService::on_close()` 与 `session_forget()` 都不执行；`LinkManager` 保留 PHONE 会话且 PHONE 没有接收超时（`link_manager.cpp` 的 tick 只处理 ALARM/PENDING），该角色永久占用。后续未知设备被当作“第二部手机”拒绝；同一数字句柄重连会被 `LinkManager::on_open()` 规则 1 判为重复句柄并断开。
- OPEN 丢失：驱动已建立流缓冲，但应用没有 `SessionEntry`，DATA 事件无法设置 `drain_pending`；数据留在 StreamBuffer，手机/报警器表现为无响应。
- 两者叠加会阻止应用正常处理重连，可能需要重启恢复；属于可达的可用性故障。

**契约/规则**
`rtk_bt_spp.h` 的事件契约、`kMaxSessions=2`、有界队列；`link_manager.hpp` 规则 1/4；HIL-3 双会话在线、HIL-5 重启后识别。

**最小修复建议**
合并每会话的 DATA 通知并累加 RX_DROP 计数，避免它们挤占生命周期事件。OPEN/CLOSE 需可靠交付，队列满时保留待交付状态或提供状态重同步；不能仅凭“最多 2 会话”就假定队列不会满，连续断连重连仍可积压多个事件。处理顺序还须避免旧 CLOSE 关闭复用句柄的新会话。

**回归建议**
T13 增加“16 个 DATA + CLOSE + 新 OPEN”场景，断言 CLOSE/OPEN 不被丢弃；host 侧用假事件队列验证应用能释放旧会话并接纳新会话。

**不修影响**
一次数据突发即可让会话永久失联；双会话/手机重连场景不可用。

### 缺陷 2（Must fix / High）：`rtk_bt_spp_disconnect` 在链路任务中无界阻塞

**位置**
- `components/rtk_bt/rtk_bt_spp.c:1435-1449`：`rtk_bt_spp_disconnect()` 直接调用 `esp_spp_disconnect(handle)`。
- `components/rtk_core/link_service.cpp:33-48`：`LinkService::apply()` 在链路任务上下文对每个动作调用 `transport_.disconnect()`，并丢弃返回的 `Status`。
- IDF：`components/bt/host/bluedroid/api/esp_spp_api.c:118-130` → `btc_transfer_context()`；`components/bt/common/btc/core/btc_task.c:366` 使用 `OSI_THREAD_MAX_TIMEOUT`；`components/bt/common/osi/include/osi/thread.h:18`、`osi/semaphore.h:27` 将其定义为 `0xffffffff`（阻塞等待）。

**可达输入/时序**
链路任务因拒绝新连接、PHONE_PREEMPT 踢报警器或 ALARM 3 s 超时调用 `apply()`；BTC 队列暂满时会等待腾出空间，没有调用方截止时间，持续无进展时可能无限期等待。主线程用真实驱动与既有替身注入 1500 ms 的 SDK 等待，`disconnect-probe.log` 实测调用方同样阻塞 1500 ms；这证明等待发生在调用方，不代表已实测 BTC 永久停滞。

**错误结果**
- 等待期间链路任务不能处理事件、命令、RX、tick 或喂 TWDT。若超过当前 5 s 阈值会触发 TWDT 诊断；当前 `CONFIG_ESP_TASK_WDT_PANIC` 未启用，不能声称必然复位/重启。
- 若 `esp_spp_disconnect` 返回错误，`apply()` 仍然移除本地解析器槽与 `LinkManager` 会话，对端连接可能仍在；没有重试，ACL 槽可能被占用到对端主动断开。该失败分支的具体 IDF 时序未单独实测，列在 §4.2。

**契约/规则**
链路任务单一所有者、有界调度与 TWDT 约束；`Transport::disconnect` 的完成通过 CLOSE 异步报告，但这句话本身不保证请求投递不阻塞。问题是当前调用没有等待上界，而不是仅凭“异步”二字判错。

**最小修复建议**
把断开请求放入 `rtk_bt_spp` 的控制表/发送任务（与 PIN/CFM/REJECT 相同的任务上下文），`rtk_bt_spp_disconnect()` 只登记并立即返回 `ESP_OK`；由 worker 调用 `esp_spp_disconnect`。worker 若被 BTC 队列阻塞，至少链路任务仍可处理事件/命令/心跳；失败时记录并在后续 tick 或 CLOSE 时重试。这样不需要修改冻结的 `rtk_bt_spp.h` 签名。

**回归建议**
T13 已有 `fake_set_disconnect_block_ms()`，增加“链路调用 disconnect 在 BTC 阻塞时仍能返回/继续 tick”的场景；应用侧验证断开失败不会永久保留无角色会话。

**不修影响**
BTC 队列拥塞可使唯一链路任务等待投递，且等待没有调用方上界；是否发展为永久停滞取决于 BTC 后续是否恢复，尚无实板复现。

### 缺陷 3（Must fix / High）：白名单保存失败后丢失待持久化状态

**位置**
- `main/app_link.cpp:364-369`：`consume_whitelist_dirty()` 返回 true 后调用 `save_whitelist()`，失败只 `ESP_LOGE`。
- `components/rtk_core/link_service.cpp:182-187`：`consume_whitelist_dirty()` 读取即清除标志。
- `main/app_link.cpp:249-255`：控制台 `wl clear` 先清 RAM 再保存；失败只返回 `kStoreError`，RAM 已清、无重试。
- `components/rtk_core/command_dispatcher.cpp:260-263`：0x44 清除白名单时先构造 `01 01` 应答；`link_service.cpp:124-131` 在 `on_data` 中先 `transport_.write()` 发送，之后才由应用保存。
- `components/rtk_store/rtk_store.c:170-200`：`nvs_set_blob`/`nvs_commit` 可失败；`main/app_main.cpp:138-141` 在 `rtk_store_init()` 失败后仍继续运行。

**可达输入/时序**
- NVS 初始化失败（例如分区损坏且擦除失败），应用记录 “store init failed; use defaults” 后继续；手机发 0x44 set 0（清空白名单），或报警器在配对窗口发 HELLO 被加入白名单。
- 或 `nvs_set_blob`/`nvs_commit` 返回错误；或 ACK 已发出、commit 前掉电。
`save_whitelist()` 失败后，`whitelist_dirty_` 已被清除，后续 tick 不再重试。

**错误结果**
- 运行时白名单已改变，但 NVS 仍是旧值；重启后旧白名单恢复。配对的报警器可能被拒绝；已清除的旧报警器可能重新获得 ALARM 角色。
- 手机收到 `01 01` 成功，但持久化并未完成；控制台只看到一次错误日志，`status` 不显示存储错误。
- 这是具体的数据丢失/授权状态回退，不是“失败传播本身”。

**契约/规则**
`phone_protocol.md` §5“清空白名单后由 app 层持久化”；HIL-5 配对与重启持久化；NVS 写后 commit 的可靠性要求。

**最小修复建议**
- 在应用层保留“待持久化”标志：消费服务层脏标志后置位，仅在 `save_whitelist()` 成功时清除；保存失败时保留到后续有界重试。无需改变冻结的 `consume_whitelist_dirty()` 读取即清除契约。
- `wl clear` 与控制台命令同样保留待持久化标志；失败在 `status`/控制台持续可见。
- 0x44 的应答先于保存符合现有分层和回写时序，不另算实现偏离。若要求应答保证落盘，应先由架构/QA 修订契约，再实现保存后应答；当前至少应避免保存失败后永久丢失待持久化状态。

**回归建议**
host 侧用假 `rtk_store` 注入 `ESP_ERR_NVS_*`，断言脏标志保留、后续重试成功、`status` 可见；T13/应用侧覆盖 0x44 清除后立即重启的持久化路径（需 HIL）。

**不修影响**
配对/清空白名单在重启后静默回退；安全策略与手机显示不一致。

### 缺陷 4（Must fix / High，拥塞/突发条件下）：RX 字节预算不约束整轮时延，单会话可占用约 2 s

**位置**
- `main/app_link.cpp:55-56`：`kRxChunkBytes=64`、`kRxBudgetBytes=256`。
- `main/app_link.cpp:195-222`：每轮按字节预算循环 `rtk_bt_spp_read()` → `s_service->on_data()`。
- `components/rtk_core/link_service.cpp:126`、`:172`：每帧应答/推送调用 `transport_.write(..., kWriteTimeoutMs)`；`kWriteTimeoutMs=200`（`link_service.hpp`）。
- `main/app_link.cpp:353-355`：每轮只处理一个事件；命令超时 `kCommandTimeoutMs=1000`（`app_link.hpp`）。

**可达输入/时序**
一个会话在 256 B 预算内到达 10 个最小帧（24 B），每个应答在拥塞/超时下等待 200 ms；`core-probe.log` 已复现：`rx_bytes=240 writes=10 elapsed_ms=2000 caller_budget_bytes=256`。两个 ALARM 的 tick 推送还可能再各加 200 ms。

**错误结果**
- 同一轮内链路任务被单个会话占用约 2 s，手机后续应答延迟远超 HIL-3 的 ≤100 ms；报警器心跳/0x40 推送和 tick 被推迟，3 s ALARM 超时余量被压缩。
- 控制台 `post()` 的 1 s 超时先到，显示 “no response; execution result unknown”，即使命令稍后执行。
- 事件队列在阻塞期间继续积压，放大缺陷 1。

**契约/规则**
链路任务有界调度/TWDT；HIL-3 手机应答 ≤100 ms、报警器推送 1000±200 ms；HIL-6 资源与周期稳定性。

**最小修复建议**
在完整帧边界安排 tick/命令/其它会话，并让一轮内的发送共享总时间预算；预算用尽时保留未处理字节和 `drain_pending`。仅在一次阻塞 `on_data()` 返回后检查时间，无法保证该次调用的上界；仅缩小 RX 字节数也不能自动满足 100 ms 应答或推送抖动要求。若需要调整冻结的 200 ms 写回契约或增加可暂停的处理接口，先由架构/QA 裁决。

**回归建议**
用假 `Transport` 注入 10×200 ms 超时，断言单轮 drain 时间上限和 `drain_pending` 保留；HIL-3 在拥塞/双会话下测应答延迟。

**不修影响**
一个拥塞会话可拖延另一个会话的周期处理；控制台可能先超时并正确报告“执行结果未知”，即使命令随后执行。

### 缺陷 5（Optional / Low）：BT 初始化失败时 `status` 的 policy 显示错误

**位置**
- `main/app_link.cpp:405-406`：`s_policy` 只在 `start()` 中赋值。
- `main/app_link.cpp:281-291`：`publish_snapshot()` 使用 `s_policy`。
- `main/app_main.cpp:157-160`：BT 初始化失败只调用 `note_bt_failure()`，不调用 `start()`。
- `main/app_link.cpp:106`：`s_policy` 静态初值为 `core::Policy::kConcurrent`；`:486-500` 的 `read_snapshot()` 直接使用该值。

**可达输入/时序**
Kconfig 选择 `RTK_LINK_POLICY_PHONE_PREEMPT`，但 `rtk_bt_spp_init()` 失败；控制台执行 `status`。

**错误结果**
`status` 打印 `policy: CONCURRENT`，与实际 Kconfig 选择不符；链路未运行，但诊断信息错误。`cmd_policy` 单独读取 `policy_from_config()`，所以两个命令输出可能互相矛盾。

**契约/规则**
`StatusSnapshot.policy` 注释为“Kconfig 选择的链路策略”；控制台诊断准确性。

**最小修复建议**
将 `s_policy` 的静态初始化改为 `policy_from_config()`（`constinit`），或在 `note_bt_failure()` 中设置；`start()` 保留再次赋值。

**回归建议**
host/启动替身覆盖 BT init 失败路径，断言 `status` 的 policy 等于 Kconfig 值。

**不修影响**
仅误导现场诊断；不影响运行逻辑。

## 4. 条件风险与未验证项（不计入确定缺陷）

以下项目缺少实际可达时序或当前证据只证明“若发生则错误”，按用户要求不与已验证缺陷混在一起。

### 4.1 相同数字句柄的迟到 WRITE_EVT（条件风险）

- 驱动行为已用 `/tmp/rtk-same-handle-probe.log` 证明：旧会话有在途写、CLOSE 后新会话复用同一数字句柄、新写处于 SUBMITTED 时，若收到旧句柄的 `WRITE_EVT`（`len=-1`、`NO_CONNECTION`），`rtk_bt_spp_handle_write()`（`rtk_bt_spp.c:815-841`）只按 handle + `TX_SUBMITTED` 匹配，会把新写者以 `ESP_FAIL` 立即结算（探针输出 `new writer after stale old evt: err=-1 ms=0`）。
- 数字句柄复用本身有 IDF 源码证据：`bta_jv_act.c:216-232` 用空闲 `rfc_cb` 下标生成 `(i+1)|BTA_JV_RFCOMM_MASK`，`bta_jv_free_rfc_cb()` 在关闭时把 `p_pcb->handle` 清零并释放该下标，下一次连接可复用同一数值；`bta_jv_add_rfc_port()` 在 `:2206-2207` 重新赋 handle。
- 但“旧 WRITE_EVT 能跨 CLOSE 到达新提交”尚未证实：当前 IDF `bta_jv_rfcomm_write()` 在 BTC 处理写消息时同步产生 `BTA_JV_RFCOMM_WRITE_EVT`（`bta_jv_act.c:2407-2414`），而缺失槽位的路径在 `btc_spp.c:925-943` 使用 handle=0；正常 FIFO 下写事件应先于 CLOSE，或变成匿名事件。没有现场/HIL 证据证明会出现“旧数字句柄、非匿名、在 CLOSE 后”的事件。
- 结论：列为**条件风险**，不列确定缺陷。若后续 HIL/fault injection 证实该时序，应先核实 SDK 可用的请求/会话标识与取消顺序，再设计过期完成事件的判别，并增加“相同 handle 重连”T13 场景（现有 `stale_completion_after_reopen` 使用不同 handle）。不能简单忽略新会话的第一条失败完成，否则可能吞掉真实错误。

### 4.2 disconnect 返回错误后的本地移除/不重试（条件风险）

- `core-probe.log` 用失败 `Transport` 证明：`LinkService::apply()` 调用一次 disconnect 后即移除本地角色/槽位，后续 tick 不重试（`retries_after_tick=0`）。
- 真实 SDK 的 `esp_spp_disconnect()` 会检查 Bluedroid 状态，并返回 BTC 投递结果；本次没有实板故障注入证明“请求返回错误、ACL 仍存活且没有后续 CLOSE”这一完整时序，因此把实际连接残留列为条件风险。
- 若 HIL 证实“disconnect 返回错误但 ACL 仍存活且无 CLOSE”，应保留待断开记录并在 tick 重试；这与缺陷 2 的异步化修复可以一起完成。

### 4.3 缺失/被忽略的 WRITE 完成导致 TX 卡住（条件风险）

- `rtk_bt_spp_write_chunk()` 超时返回时可能留下 `tx_state=SUBMITTED`、`s_tx_inflight` 仍被占用（`rtk_bt_spp.c:1362-1383`）；`rtk_bt_spp_handle_write()` 对匿名 `NO_CONNECTION` 选择忽略（`:843-849`）。若该事件是当前请求的唯一完成且没有 CLOSE，后续写会一直等待/超时，两个会话共用的全局 owner 也会阻塞。
- 现有 T13 `write_timeout_budget` 只覆盖“完成延迟到达”，未覆盖“完成永远不到”；当前 IDF 正常路径应产生 WRITE_EVT，缺少实际丢失时序，故列为条件风险。修复方向是每请求截止时间/放弃状态，或在超时后由 worker 确认 IDF 不再使用 `tx_buf` 再释放 owner；不要仅清状态后立即复用静态缓冲。

### 4.4 短写后的流恢复（条件风险/已知限制）

- IDF `bta_jv_act.c:2407-2414` 的 `PORT_WriteDataCO` 可返回部分长度；驱动 `rtk_bt_spp_settle_write()`（`rtk_bt_spp.c:801-812`）把 `len != tx_len` 判为 `ESP_FAIL`。T13 `FAKE_WRITE_SHORT` 已覆盖“失败传播正确”。
- 接收侧解析器按协议不回扫已收字节；截断帧会继续消费后续字节直到期望长度，CRC 失败后重新同步（`rtk_proto_frame.c` 的 `parser_check_lengths`/`parser_finish`）。因此短写可能丢失该应答并顺带吞掉下一帧，但不会永久破坏字节流。
- 这属于现有协议/解析契约允许的错误传播和恢复方式；没有证实额外的消费者错误，故不单列缺陷。若产品要求“短写后立即恢复”，应在 `LinkService` 层关闭该会话或触发重同步/重传策略，而不是修改驱动的失败返回。

### 4.5 其它缺少实际可达时序的风险

| 风险 | 位置 | 为何不单列 | 若证实后的方向 |
| --- | --- | --- | --- |
| 拒绝 OPEN 时应用先记录 `SessionEntry`，CLOSE 丢失/延迟时 map 占满 | `main/app_link.cpp:166-173`、`:120-135` | 依赖缺陷 1 的 CLOSE 丢失或 disconnect 失败；本身没有独立可达时序 | `on_open` 后若 `role==kNone` 立即 `session_forget`；或只记录已接受会话 |
| BTC 热路径日志（队列满/RX overflow/写失败）可能阻塞回调 | `rtk_bt_spp.c:489-493`、`:789-794`、`:875-881` | 未测得 UART/日志锁实际阻塞；日志只在异常路径 | 回调只计数，链路/worker 限速打印 |
| 控制表满时无槽连接被留在 ACL | `rtk_bt_spp.c:748-753`、`:74-76` | 需要 4 个并发 PIN/CFM/REJECT 且无安全槽，未复现 | 为 REJECT 预留/替换 PIN/CFM 项，或由 worker 断开 |
| init 失败时 `objects_delete()` 在锁外重置 sessions/ctrl，而 worker 仍运行 | `rtk_bt_spp.c:531-548`、`:567-576` | 只在初始化失败路径，未测出实际破坏 | 持 `s_session_mux` 复位或停止/通知 worker |
| G7b 的 `EXCEPTIONS_RE` 未覆盖 `__cxa_rethrow` 等未列异常符号 | `tools/g7b_check/g7b_check.py:26-27`、`:33-40` | 当前 ELF 没有这些符号；是潜在假 PASS | 按异常符号分类补充遗漏项，并加负向用例；不要把所有 `__cxa_` 符号都视为异常 |
| `tidy_check` 对 test-app 编译数据库硬编码 `--target=xtensa-esp32-elf` | `tools/tidy_check/tidy_check.py:100` | G5b 验收只用 esp32；未用 esp32c3 跑 tidy | 从 build 配置推导 target |
| 运行时 heap/栈余量、栈溢出、TWDT 实际行为 | HIL-6 | 未上板；`idf-size` 的 DRAM Remain 不是 heap | HIL-6 实测 |

## 5. 明确契约选择/已知限制（不是实现缺陷）

以下为现有协议/ADR/5.4 兼容性选择，若威胁模型变化应走契约变更，不应按实现 bug 处理：

1. **PHONE 角色无认证**：`LinkManager::on_open()` 在无 PHONE 时把任何未知设备设为 PHONE；SPP 使用 `ESP_SPP_SEC_NONE`、SSP Just Works、旧设备 PIN `1234`（`phone_protocol.md` §6）。任何附近设备在手机槽空闲时可查询 SN、设置测试报警、清除白名单。这是 5.4 兼容选择，不是本实现偏离。
2. **PHONE 无接收超时**：`LinkManager::tick()` 只对 ALARM/PENDING 超时；PHONE 可以长期空闲。缺陷 1 的 CLOSE 丢失因此会永久占用角色。
3. **0x0D 设置不回复、0x43 围栏预留、0x07/0x14/0x15 只存 RAM、SN 修改需重启**：均为 `phone_protocol.md` §4/§5 的明确行为。
4. **短写返回失败**：驱动把 `status` 非成功或 `len != tx_len` 判为失败，T13 已覆盖；这是契约允许的错误传播，不应把“返回失败”本身当缺陷。
5. **0x44 清除白名单不要求配对窗口**：协议只限制命令来自 PHONE，未要求二次认证；与第 1 条同属契约选择。
6. **`kWriteTimeoutMs=200`、`kCommandTimeoutMs=1000`**：实现选择的预算；缺陷 4 是预算组合未约束总时延，不是单个常量错误。

## 6. 覆盖清单

已读/已核对：

- 文档：`AGENTS.md`、`docs/adr/ADR-0002-cpp-esp-idf.md`、`ADR-0003-c-cpp-split.md`、`docs/protocol/phone_protocol.md`、`docs/acceptance.md`、`docs/reports/` 下相关复审记录与本次日志。
- `main/`：`app_main.cpp`、`app_link.cpp/.hpp`、`app_console.cpp/.hpp`、`app_version.hpp`、`Kconfig.projbuild`、`CMakeLists.txt`。
- `rtk_proto`：`rtk_proto_frame.c`、`rtk_proto_alarm.c`、`frame.cpp`、`alarm_codec.cpp`、`bytes.cpp`、`status.cpp`、全部公共头与 CMake。
- `rtk_core`：`link_manager.cpp`、`link_service.cpp`、`command_dispatcher.cpp`、`alarm_source.cpp`、`transport.hpp`、`types.hpp`、全部公共头与 CMake。
- `rtk_os`：`esp_status.cpp`、`esp_status.hpp`、`critical_section.hpp`、`static_queue.hpp`、`static_event_group.hpp`、`static_stream_buffer.hpp`、`static_task.hpp`、CMake。
- `rtk_bt`：`rtk_bt_spp.c`、`spp_transport.cpp`、`rtk_bt_spp.h`、`spp_transport.hpp`、CMake；IDF 侧只核对 SPP/BTA 相关路径（`esp_spp_api.c`、`btc_spp.c`、`bta_jv_act.c`、`btc_task.c`、`osi/thread.h`/`semaphore.h`）。
- `rtk_store`：`rtk_store.c`、`rtk_store.h`、CMake。
- 构建：根 `CMakeLists.txt`、`sdkconfig.defaults`、`sdkconfig`、`partitions.csv`、各组件 `CMakeLists.txt`、`main/Kconfig.projbuild`。
- 测试/工具：`test/host/**` 8 个可执行文件源码、`tools/t13_host_check/**`（22 场景）、`test_apps/rtk_pure/**`、`tools/tidy_check/tidy_check.py`、`tools/g7b_check/g7b_check.py`。
- 证据：`sources.sha256` 96 文件校验；`queue-probe.log`、`core-probe.log`；本次 `/tmp/rtk-same-handle-probe.c/.log`。

未逐行复审/未作为构建对象：`reference/c-v1/**` 与 `reference/cpp-v2-draft/**`（只读参考，不参与构建）；IDF 全量源码（仅核对与 SPP/disconnect/回调相关的路径）；`build*`/`test_apps/rtk_pure/build-*` 产物目录（只读日志，不作为自有源码审计）。

## 7. 测试覆盖缺口（作为上述缺陷的回归建议，不单独计数）

- 缺陷 1：T13 没有“队列满 + CLOSE/OPEN”场景；现有 `open_no_slot_rejects_from_task`/`slot_held_rejects_new_connect` 不覆盖事件队列满。建议新增 T13 场景，并在 host 应用层用假事件队列验证会话释放/接纳。
- 缺陷 2：T13 有 `fake_set_disconnect_block_ms()`，但没有“链路任务调用 disconnect 在 BTC 阻塞时仍能继续”的场景；建议新增。
- 缺陷 3：`test/host` 的 `FakeTransport` 只覆盖写失败；`main/app_link` 不在 host 测试范围内，NVS 保存失败/重试无回归。建议增加应用层假 store 测试。
- 缺陷 4：`test/host` 的 `FakeTransport::write()` 立即返回成功（`test_support.hpp:60-77`），不会暴露 10×200 ms 的时延；建议注入阻塞写并断言单轮预算。
- 双会话 scatternet、配对后重启、白名单回退、TWDT/堆/栈：仅 HIL 可覆盖，当前 NOT_RUN。

## 8. 未验证范围

- HIL-0..6 全部 NOT_RUN：未烧录、未连接串口、未运行 pytest-embedded；`rtk_main_guard` 拒绝芯片识别，未绕过。GPIO26 电源保持只有软件路径与旧固件/IDF 源码核对，没有实测电压/自保持。
- ASan/UBSan 运行 BLOCKED：LeakSanitizer 在 ptrace 环境 fatal，EXIT 8；未关闭 sanitizer 重跑，不能据此判断产品内存错误。
- 权威 astyle_py、`cpp_cost.py` NOT_RUN。
- 运行时 heap/栈余量、双会话吞吐、拥塞、断连重连、句柄复用迟到事件、短写、TWDT 实际触发：NOT_RUN；本次只用 host 探针/源码路径分析。
- `idf-size` 的 DRAM Remain 只作静态段记录，未当作运行时 heap 结论。

## 9. 结论

**必须修复 4 项**：事件队列生命周期事件丢失、disconnect 无界阻塞、白名单持久化失败被吞掉、RX 字节预算不约束时延。**可选修复 1 项**：BT 初始化失败时 policy 诊断错误。其余（相同句柄迟到 WRITE、disconnect 失败不重试、缺失 WRITE 完成、短写流恢复、拒绝 OPEN 的 map 暂留、回调日志、控制表满、init 失败竞态、G7b regex、tidy target）均为条件风险/未验证，已单列，不与确定缺陷混计。

软件门禁证据充分：host 8/8、T13 22/22、ESP32 全新构建、双架构纯组件构建、G7b、G5b、静态门禁、体积均 PASS；但它们不覆盖上述实时/持久化缺陷，也不能替代 HIL。最终签字仍需人工完成。

---

本报告只新增本文件；未修改产品代码、冻结输入、测试或配置。条件探针源码与日志位于 `/tmp/rtk-same-handle-probe.c`、`/tmp/rtk-same-handle-probe.log`；未写入仓库。

## 10. 主线程交叉核实与交付记录

- 已核对当前源码、完整报告与本轮日志；补充运行真实 C 驱动的断开阻塞探针，输出保存在 `docs/reports/logs/FULL-REVIEW-DEEPSEEK/disconnect-probe.log`。所有临时探针源码保留于 `/tmp/rtk-full-review-probes-9lp4jp74/`，不加入生产或冻结测试。
- 本轮 DeepSeek Codex 已完成的软件门禁日志已归档为 `docs/reports/logs/FULL-REVIEW-DEEPSEEK/independent-*.log`；这些检查发生在同一任务内。新的 omp DeepSeek 会话核对并复用日志，没有重复运行基线门禁。
- 当前产品源码、测试、配置共 96 个快照文件未变化；候选源码/镜像准备记录另有 8/8 SHA-256 校验通过。仅新增报告、日志和来源记录；没有产品修复。
- 用户授权硬件测试仍有效。此前自动审批钩子 `rtk_main_guard` 已拒绝 `/dev/ttyACM0` 芯片识别，理由为项目 AGENTS.md §6 禁止串口/芯片访问；本轮没有重试或换工具绕过该拒绝。硬件阶段 BLOCKED，所有未执行 HIL 均为 NOT_RUN。
- 使用技能：`herdr`、`ponytail-audit`、`ponytail-review`、`embedded-engineering`。当前可见复审会话为 Herdr `wF:t4` / `wF:pE`，agent `rtk-omp-deepseek`，模型 `magpie/workbuddy/deepseek-v4.1-flash`。
