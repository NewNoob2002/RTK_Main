# RTK_main 全工程复审（第二轮，QA 只读）

日期：2026-11-11
角色：QA 只读复审（不修改产品源码/测试/配置/冻结输入；不做 git 写操作；不安装下载；不接触串口/硬件）
范围：`main`、`components/rtk_proto`（C 核心 + C++ 外观）、`rtk_core`、`rtk_os`、`rtk_bt`（C 驱动 + `SppTransport`）、`rtk_store`、构建/Kconfig/sdkconfig/分区、`test/host`、`test_apps/*`、`tools/*`
依据：`AGENTS.md`、`docs/adr/ADR-0002-cpp-esp-idf.md`、`docs/adr/ADR-0003-c-cpp-split.md`、`docs/protocol/phone_protocol.md`、`docs/acceptance.md`；所有结论以当前源码为准，旧报告只作背景。

## 0. 结论摘要

**未发现新的、已证实的确定性必须修复缺陷。上一轮 `FULL-REVIEW-OMP-DEEPSEEK.md` 的 4 项必修 + 1 项可选修复，在 `FULL-REVIEW-FIX` 之后的当前源码中均已真实落地，并有对应回归用例；本轮独立复跑全部软件门禁通过（详见 §7）。**

剩余问题集中在：认证/断开异常路径的失败恢复（建议）、诊断可见性（建议）、门禁工具健壮性（建议），以及必须依赖实板才能判定的条件风险（疑问）。它们都没有给出本轮可复现的完整时序，故不与确定性缺陷混计。

| 类别 | 数量 | 结论 |
| --- | --- | --- |
| 必须修复（确定性、已证实可达） | 0 | 本轮无 |
| 建议修复（低风险/诊断/维护性） | 7 | §4 |
| 已知限制/契约选择 | 8 | §5，不是实现缺陷 |
| 条件风险/疑问（无可达时序） | 7 | §6 |
| 验证 FAIL | 0 | — |

## 1. 上一轮 4 项必修 + 1 项可选的逐项核验

| 项 | 上一轮结论 | 当前判定 | 源码证据（当前快照） | 本轮执行的回归 |
| --- | --- | --- | --- | --- |
| 1. 事件队列满丢弃 CLOSE/OPEN | 必须修复 | **PASS（已修复）** | 每槽 `lifecycle_pending`/`data_pending`/`drop_pending`（`rtk_bt_spp.c:105-108`）；DATA/RX_DROP 在回调内合并（`:558-600`）；槽复用要求 `!lifecycle_pending` 且无读者/在途发送（`:268-345`）；`CLOSE` 交付前槽不可复用（`:1402-1428`）；`_Static_assert(QUEUE_LEN >= 4*MAX_SESSIONS)`（`:84`） | T13 `lifecycle_burst_churn`（40×DATA/会话 + 倒序 CLOSE + 40 次未发布连接重连：8 事件顺序、0 丢弃、无幽灵 CLOSE、同 handle 重连可读）；29/29 PASS |
| 2. `disconnect` 在链路任务无界阻塞 | 必须修复 | **PASS（已修复）** | `rtk_bt_spp_disconnect` 只登记 `disconnect_pending` 并唤醒 worker（`:1597-1618`）；worker `disconnect_process` 在任务上下文调用 SDK，失败 100 ms 重试、成功等真实 CLOSE（`:787-818`）；`LinkService::apply` 不再阻塞（`link_service.cpp:33-48`） | T13 `disconnect_async_and_reject`（SDK 门闩阻塞时调用者 <100 ms 返回）、`disconnect_retry`、`disconnect_reuse_across_slots` PASS |
| 3. 白名单保存失败被吞掉 | 必须修复 | **PASS（已修复）** | `persist_whitelist`：pending 仅在保存成功后清除，失败保留并按 1000 ms 限速重试（`app_link.cpp:173-190`）；配对升级/0x44 清空/控制台 `wl clear` 全部进入 pending（`:271-312`）；`status` 显示 pending 与最近错误（`:330-380`、`app_console.cpp:145-160`） | 真实组合根 `save_recovery`（ACK 先于保存、999 ms 不重试、1000 ms 恢复、配对自身 dirty、控制台失败恢复、最新 RAM 覆盖、时钟回绕）PASS |
| 4. RX 字节预算不约束整轮时延 | 必须修复 | **PASS（已修复，仍有限制）** | 整轮共享 20 ms 启动预算 + 256 B + **一个完整帧**；完整帧后立即让出并轮转会话；半帧与预读字节跨轮保留（`app_link.cpp:232-270`、`:393-425`）；命令/tick 先于 RX | `rx_fairness`（单帧让出、命令优先、tick 耗尽后另一会话心跳）、`rx_fast_partial`（快速 10 帧无额外 20 ms、抢占后零帧且字节保留）、`rx_tick_budget`（两 ALARM tick 400 ms 且该轮零 RX）PASS |
| 5.（可选）BT 失败时 `status` policy 错 | 可选 | **PASS（已修复）** | `constinit core::Policy s_policy = policy_from_config();`（`app_link.cpp:111`），`read_snapshot` 使用该值（`:551-566`） | `policy_failure` 在 CONCURRENT / PHONE_PREEMPT 两种编译下 PASS |

**对第 4 项的保留说明**：修复把“十次同步回复”收敛为“每轮至多一次”，但没有、也不能在不改冻结 `kWriteTimeoutMs=200` 与 `LinkService::tick` 的前提下消除单次同步写等待。拥塞时一轮最坏约为 tick 2×200 ms + 回复 200 ms ≈ 600 ms（见 §6-Q6）。这是已知限制，不是本轮新缺陷；HIL-3 的 ≤100 ms / 1000±200 ms 必须在实板上判定。

## 2. 本轮按子系统的复审结论

### 2.1 蓝牙 SPP 收发路径

- 回调上下文：`SRV_OPEN/CLOSE/DATA/WRITE/CONG/PIN/CFM` 只做拷贝、状态更新、非阻塞队列/任务通知；可能阻塞的 `esp_spp_write`/`esp_spp_disconnect`/GAP 应答全部在 worker 任务上下文（`rtk_bt_spp.c:1-18, 469-556, 713-818, 860-1075`）。与 IDF `btc_transfer_context`/`btc_task_post` 的阻塞语义一致。
- RX 路径：BTC → 每会话静态 StreamBuffer（2048 B）→ 每槽最多一个 DATA 通知 + 饱和累计 RX_DROP（`:558-600, 908-940`）→ 链路任务 `poll_event` 合并/清零（`:1402-1428`）→ `read`（`:1430-1446`）→ 逐字节解析（`link_service.cpp:100-146`）。
  - 合并规则不存在丢数据窗口：回调先 `xStreamBufferSend`、后查 `data_pending`；应用在 `poll_event` 内清标志。两种交错下要么字节已在缓冲区被读到，要么会重新投递事件。
  - RX_DROP 事件在交付时用 `rx_dropped` 累计值覆盖并清零（`:1418-1423`），不会重复计数；T13 `drop_counts_by_handle` 验证每 handle 独立账。
- 事件容量上界：每槽至多 `OPEN/DATA/RX_DROP/CLOSE` 各 1；槽在 CLOSE 交付前不可复用（`:268-345, 1402-1428`），两槽 ≤ 8 ≤ 16。回调队列满路径（`:594-597`）当前不可达；T13 40×DATA 突发实测 `dropped_events()==0`。
- TX 路径：每次提交 ≤512 B，worker 串行提交、全驱动至多一个在途提交；完成按 `handle + tx_state + tx_seq` 结算，短写/错误按失败传播（`:801-818, 958-1035, 1451-1548`）。关闭时取消在途提交，等 `esp_spp_write` 返回后才释放静态 `tx_buf` 所有权（`:345-390, 713-785`）。
- 双会话：`s_tx_inflight` 全局串行化；`congested` 由 `CONG_EVT` 门控；`users` 计数防止读/写期间槽被复用或 `StreamBufferReset`（`:240-266, 287-300`）。
- 断连/重连：普通断开异步登记 + 100 ms 重试；同 handle 在 CLOSE 交付前或 SDK 调用返回前禁止接纳（`:268-290, 787-818`）；拒绝控制表与普通断开状态分离，不互相挤占（`:1597-1618`）。

### 2.2 应用调度（`main/app_link.cpp`）

- 单一所有者：LinkManager/AlarmSource/CommandDispatcher/LinkService 只在链路任务访问；控制台经 `StaticQueue<Command>` + `portMUX` 快照（`app_link.cpp:1-6, 105-115, 510-566`）。
- 轮结构：事件批次（≤16）→ 消费白名单 dirty → 命令（≤4）→ 到期 tick → RX（20 ms/256 B/一帧）→ 再消费 dirty → 持久化重试 → 快照（`:393-425`）。
- 预算起点在命令/tick 之前；tick 耗尽预算时不推进 RX 轮转起点（避免跳过另一会话）；非空 RX 时 `poll_event` 用 0 超时，空闲才等 20 ms（`:393-410`）。
- 每轮末尾 `vTaskDelay(1)`，保证同核 IDLE 获得运行机会；TWDT 在整轮结束后喂（`:428-448`）。
- 会话表与驱动发布严格对应：驱动只有在“已发布且 CLOSE 已交付”后才复用槽，应用不会收到超过 2 个未关闭的 OPEN；被拒绝的连接不发布 OPEN/幽灵 CLOSE。

### 2.3 协议与 NVS

- 帧编解码：同步、头长、payload 上限、CRC、错误计数与重同步逐字节实现（`rtk_proto_frame.c`）；C++ 外观只做类型映射（`frame.cpp`、`alarm_codec.cpp`）。CRC 与 zlib 等价（host 用例覆盖）。
- 命令分发与协议文档逐项核对：0x01/0x02/0x03/0x04/0x05/0x06/0x07/0x0D/0x14/0x15/0x40/0x41/0x42/0x43/0x44 的长度、偏移、应答 type 与“只存 RAM/不回复/预留”语义一致（`command_dispatcher.cpp`）。
- NVS：`nvs_open → 操作 → commit → close`；`get_blob(NULL,&len)` 长度查询与 IDF v6.1 `nvs_get_str_or_blob` 行为一致（本轮核对 `nvs_api.cpp:604-612`）；空 blob/键不存在按空白名单处理；非法长度/超限返回 `INVALID_SIZE`（`rtk_store.c`）。

### 2.4 跨核与生命周期

- SMP 临界区全部使用 `portMUX_TYPE`，临界区内不调用 FreeRTOS API/不阻塞（驱动与 `os::CriticalSection`）。
- 任务/队列/StreamBuffer/事件组全部静态存储；`app_link` 的两个核心对象 placement-new 到 `alignas` 静态存储，只构造一次。
- 控制台 4096 B/core0/prio2；链路任务 6144 B/core1/prio10；BT worker 4096 B/core0/prio5；与 sdkconfig（`FREERTOS_HZ=1000`、Bluedroid core0、ACL=2）一致。

### 2.5 整数窄化与越界

逐处核对 `-Wconversion/-Wsign-conversion` 已开启的纯组件与 `-Werror` 目标构建：所有 `static_cast` 均附范围理由；协议/白名单/事件/发送缓冲区的边界都有先行检查（`build_frame`、`ByteWriter`、`load/save_whitelist`、`drain_rx`、`write_chunk`、`rtk_store_*`）。未发现新的越界或窄化缺陷。

## 3. 必须修复

**无。** 本轮没有发现满足“确定性 + 可达时序 + 违反现有契约”三项条件的缺陷。

## 4. 建议修复

### S1（中低）PIN/CFM 应答在投递前出表，失败不重试

- 位置：`components/rtk_bt/rtk_bt_spp.c:469`（`rtk_bt_spp_ctrl_process`）、`:503-518`（PIN/CFM 取走时 `in_use=false`）、`:541-545`（失败仅日志）。
- 可达时序：GAP `PIN_REQ`/`CFM_REQ` 在 BTC 回调登记；worker 取走后先出表再调用 `esp_bt_gap_pin_reply`/`esp_bt_gap_ssp_confirm_reply`。`btc_transfer_context` 在 `osi_malloc` 失败时返回 `BT_STATUS_NOMEM`（`btc_task.c:348-352`），或 Bluedroid 状态非 ENABLED 时 API 返回错误。
- 错误结果：本次认证响应丢失，旧设备 PIN 或 SSP 确认失败；对端需重新发起配对，期间 SPP 无法建立。控制表满（4 项）时新 PIN/CFM 也会被丢弃（`:437-444`）。
- 依据：与 REJECT 不同，PIN/CFM 没有 `submitted/attempted` 重试路径；`RTK_BT_SPP_CTRL_RETRY_MS` 实际只作用于 REJECT。
- 最小修复：PIN/CFM 也在 API 返回 `ESP_OK` 后才出表；失败保留并按 100 ms 重试（与 REJECT 同构），或在表满时按类型优先淘汰。

### S2（低）普通断开失败无限重试且无日志限速

- 位置：`components/rtk_bt/rtk_bt_spp.c:787-818`（`disconnect_process`），失败日志在 `:814`。
- 可达时序：`esp_spp_disconnect` 持续返回错误，且对端/协议栈不再产生 CLOSE（例如句柄已在 SDK 侧失效但 CLOSE 丢失）。普通断开没有尝试次数上限。
- 错误结果：worker 每 100 ms 重试并打 WARN，永久持续；UART 日志与 CPU 开销；`disconnect_submitting` 期间对应槽不可复用。
- 依据：`:797` 只有最小间隔，没有上限或降频。
- 最小修复：记录尝试次数，超过阈值后降为低频日志并保留重试；或由应用层在超时后放弃本地会话（需契约裁决，不能伪造 CLOSE）。

### S3（低）启动时白名单读取失败只剩启动日志可见

- 位置：`main/app_main.cpp:139-150`、`main/app_link.cpp:599-620`。
- 可达时序：`rtk_store_init` 失败/分区损坏后继续运行（既有设计），`rtk_store_get_alarms` 返回非 OK → 空白名单。
- 错误结果：已配对报警器被拒绝（未知设备在无 PHONE 时占 PHONE 槽）；`status` 仍显示 `whitelist: 0 store=ESP_OK`，现场只能翻启动日志。
- 依据：`s_whitelist_error` 只在保存路径写入（`app_link.cpp:185`）。
- 最小修复：在 `StatusSnapshot` 记录加载错误（新增字段或复用错误字段），`status` 显示；不改变冻结的 store 接口。

### S4（很低）缺少跨组件容量常量一致性断言

- 位置：`main/app_link.cpp:70-71,105`；`components/rtk_bt/include/rtk_bt_spp.h:22-24`；`components/rtk_store/include/rtk_store.h:17-19`。
- 后果：`core::kMaxSessions`/`kMaxAlarms` 与驱动/存储宏若未来不一致，应用会静默少处理会话或截断白名单。
- 最小修复：在 `app_link.cpp` 增加 `static_assert(kMaxSessions == RTK_BT_SPP_MAX_SESSIONS)`、`static_assert(kMaxAlarms == RTK_STORE_ALARM_MAX)`（该文件已包含两个头）。
- 说明：当前值一致（2/4），纯维护性。

### S5（工具，低）G7b 异常符号正则不完整

- 位置：`tools/g7b_check/g7b_check.py:26-28`（`EXCEPTIONS_RE`）。
- 后果：未来若链接引入 `__cxa_rethrow`、`__cxa_throw_bad_array_new_length`、`_Unwind_Backtrace` 等未列符号，`is_runtime` 不识别，白名单子集检查不会 FAIL → 假 PASS。
- 依据：本轮 ELF 只有 6 个白名单符号，今天不会误判；属门禁健壮性。
- 最小修复：补全异常/展开符号集合（不要把所有 `__cxa_*` 都算异常），并加负向用例。

### S6（工具，很低）tidy_check 对 test-app 编译数据库硬编码 xtensa target

- 位置：`tools/tidy_check/tidy_check.py:100`（`--target=xtensa-esp32-elf`）。
- 后果：对 esp32c3 的 test-app DB 会用 xtensa target 解析，可能产生噪音或误判；本轮该部分使用既有 `test_apps/rtk_pure/build-esp32` DB。
- 最小修复：从 build 的 `project_description.json`/toolchain 文件推导 target，或显式传参。

### S7（诊断，低）未知 msg_id 未按协议文档记录日志

- 位置：`components/rtk_core/command_dispatcher.cpp:429-437`（`is_known_id` 为 false 直接 `kNoReply`）。
- 契约：`docs/protocol/phone_protocol.md` §3“……未知 ID：不回复（与 5.4 一致），只记日志。”
- 错误结果：现场无法从日志/`status` 判断手机发送了保留或未知命令。
- 最小修复：在链路任务侧按 `msg_id` 限速计数并暴露到 `status`，或使用 `ESP_LOGD`（避免每帧 WARN 洪泛）。

## 5. 已知限制 / 契约选择（不是实现缺陷）

1. PHONE 无认证、无接收超时；未知设备在无 PHONE 时可成为 PHONE（`link_manager.hpp` 规则 4，5.4 兼容）。
2. `0x0D` 设置不回复、`0x43` 预留、`0x07/0x14/0x15` 只存 RAM、SN 修改需重启、`0x44` 清空不要求配对窗口——均为协议文档明确行为。
3. `0x44`（及配对）先回 RAM 接受 ACK，再由应用持久化；掉电发生在成功落盘前仍可能回退旧白名单（本轮未引入事务日志）。
4. 短写/超时按失败传播，接收侧解析器不回扫、靠 CRC/长度错误重同步；可能丢该应答并吞掉后续一帧，但不会永久破坏字节流。
5. `kWriteTimeoutMs=200`、`kCommandTimeoutMs=1000`、tick 内两次同步推送为冻结/既定契约；本轮未改动。
6. 事件接纳背压：CLOSE 交付前同 handle 不可复用，重连可能被拒绝一次（应用正常 ≤20 ms 内轮询；拥塞时更久），对端需重试。这是修复缺陷 1 的必然代价。
7. ASan 泄漏检测在本机 ptrace 环境不可用（见 §7）；本轮以 `detect_leaks=0` 运行 ASan/UBSan 取得内存/未定义行为覆盖，未据此声明无泄漏。
8. 权威 astyle_py、实板 HIL、运行时 heap/栈/TWDT 均未执行。

## 6. 条件风险 / 疑问（无法给出可达时序，不计入缺陷）

| 编号 | 风险 | 位置 | 为什么只是疑问 | 若证实后的方向 |
| --- | --- | --- | --- | --- |
| Q1 | 同数字句柄的迟到 `WRITE_EVT` 结算新会话 | `rtk_bt_spp.c:958-1015` | IDF 在 BTC 处理写消息时同步产生 WRITE_EVT（`bta_jv_act.c:2407-2414`），正常 FIFO 下先于 CLOSE；没有证据表明旧句柄事件跨 CLOSE 到达新提交 | 增加“同 handle 重连 + 迟到旧完成”故障注入，再设计过期完成判别 |
| Q2 | 缺失 `WRITE_EVT` 导致 TX owner 永久占用 | `rtk_bt_spp.c:1451-1548, 958-1015` | 需要 SDK 完成事件丢失且无 CLOSE；现无复现 | 每请求截止时间/放弃状态，且确认 IDF 不再使用 `tx_buf` |
| Q3 | worker 永久阻塞在 `esp_spp_write`/`esp_spp_disconnect` | `:745, :808` | 调用方已有界，但整体进度依赖 BTC 恢复；无实板证据 | 不可在不改 SDK 的前提下彻底解决；需 HIL 故障注入与看门狗策略 |
| Q4 | 初始化失败拆除时回调仍运行 | `:637-665, 1315-1325` | 仅在 `esp_spp_deinit` 确认超时的失败路径；未复现 | 拆除前确保回调静默，或延迟释放 RTOS 对象 |
| Q5 | 回调上下文日志可能阻塞 | `:594-597, 936-940, 1015-1020` | 只发生在异常路径（队列满/溢出/写失败），未测到 UART 锁阻塞 | 回调只计数，由 worker/链路任务限速打印 |
| Q6 | 拥塞时单轮最坏 ≈600 ms | `link_service.cpp:148-180`、`app_link.cpp:393-425` | 需要每次写都等满 200 ms；HIL-3 阈值需实板验证 | 由架构裁决缩短写超时/拆分 tick，或接受并改验收阈值 |
| Q7 | 重连背压窗口 | `rtk_bt_spp.c:268-290` | 正常轮询 <1 轮；窗口依赖 Q6 | 若 HIL-3b 报警器 15 s 回连失败，再评估提前释放槽 |

## 7. 本轮验证状态

所有命令在临时目录执行；未修改项目 `sdkconfig`，未烧录、未接触串口/硬件。

| 项 | 状态 | 实际命令/结果 |
| --- | --- | --- |
| 冻结文件 | **PASS** | `sha256sum -c docs/frozen.sha256`：115/115 OK |
| host 单元测试 | **PASS** | `cmake -S test/host -B /tmp/r2-host && cmake --build /tmp/r2-host -j8 && ctest --test-dir /tmp/r2-host`：8/8 可执行文件通过，0 warning |
| T13 驱动回归 | **PASS** | `cmake -S tools/t13_host_check -B /tmp/r2-t13 && ... && ctest`：29/29（7.24 s），0 warning |
| 真实组合根回归 | **PASS** | `cmake -S tools/main_host_check -B /tmp/r2-main && ... && ctest`：12/12（CONCURRENT/PHONE_PREEMPT 各 6） |
| ASan/UBSan 构建+运行 | **PASS（泄漏检测 BLOCKED）** | `/tmp/r2-asan`，`ASAN_OPTIONS=detect_leaks=0 ctest`：8/8；未关闭 ASan/UBSan；LSan 仍受 ptrace 限制，记为 BLOCKED |
| ESP32 目标构建 | **PASS** | `. ~/esp/esp-idf/export.sh`；`idf.py -B /tmp/r2-idf -DIDF_TARGET=esp32 -DSDKCONFIG=/tmp/r2-idf/sdkconfig -DCMAKE_EXPORT_COMPILE_COMMANDS=ON build`：exit 0，0 warning/error。临时配置与项目 `sdkconfig` 的有效 `CONFIG_*` 逐项 diff 为 0，构建具有代表性 |
| 体积 | **PASS** | `idf.py -B /tmp/r2-idf size`：Total 626,568 B；IRAM 82,779 B（63.16%）；DRAM 63,109 B（50.66%） |
| 双架构纯组件 | **PASS**（本轮 esp32c3；esp32 由主构建覆盖） | `idf.py -B /tmp/r2-pure-c3 -DIDF_TARGET=esp32c3 build`（test_apps/rtk_pure）：exit 0，0 warning |
| G7b C++ 运行时 | **PASS** | `python3 -I tools/g7b_check/g7b_check.py --build /tmp/r2-idf`：17 个自有目标文件，白名单 6/6 |
| G5b clang-tidy | **PASS** | `python3 -I tools/tidy_check/tidy_check.py --host-build /tmp/r2-host --idf-build /tmp/r2-idf --jobs 4`：27 文件，0 发现（test-app 部分复用既有 `build-esp32` DB） |
| G1 冻结一致性 | **PASS** | 同上 |
| clang-format | **PASS（本轮未改源码，未重跑）** | 仅写本报告；格式门禁沿用 `FULL-REVIEW-FIX` 证据 |
| 权威 astyle_py | **NOT_RUN** | 需联网下载/安装，按项目约束不执行 |
| HIL-0..6 | **NOT_RUN / BLOCKED** | 未烧录、未连接串口；硬件由主线程独占；前序 `rtk_main_guard` 拒绝未绕过 |
| 运行时 heap/栈/TWDT、双会话吞吐、断连重连、配对重启 | **NOT_RUN** | 仅 host/构建/源码路径分析，不替代实板 |

## 8. 覆盖清单与未验证范围

已读/已核对（以当前源码为准）：

- 文档：`AGENTS.md`、ADR-0002、ADR-0003、`docs/protocol/phone_protocol.md`、`docs/acceptance.md`、上一轮复审与 `FULL-REVIEW-FIX`。
- `main`：`app_main.cpp`、`app_link.cpp/.hpp`、`app_console.cpp/.hpp`、`app_version.hpp`、`Kconfig.projbuild`、`CMakeLists.txt`。
- `rtk_proto`：`rtk_proto_frame.c`、`rtk_proto_alarm.c`、`frame.cpp`、`alarm_codec.cpp`、`bytes.cpp`、`status.cpp`、全部公共头与 CMake。
- `rtk_core`：`link_manager.cpp`、`link_service.cpp`、`command_dispatcher.cpp`、`alarm_source.cpp`、`transport.hpp`、`types.hpp`、全部公共头与 CMake。
- `rtk_os`：`esp_status.cpp`、全部 RAII 头与 CMake。
- `rtk_bt`：`rtk_bt_spp.c`、`spp_transport.cpp`、`rtk_bt_spp.h`、`spp_transport.hpp`、CMake；IDF 侧核对 `esp_spp_api.c`、`btc_spp.c`、`btc_task.c`、`nvs_api.cpp` 的相关路径。
- `rtk_store`：`rtk_store.c`、`rtk_store.h`、CMake。
- 构建：根 `CMakeLists.txt`、`sdkconfig.defaults`、`sdkconfig`、`partitions.csv`、各组件 `CMakeLists.txt`。
- 测试/工具：`test/host/**`、`tools/t13_host_check/**`（29 场景）、`tools/main_host_check/**`（12 场景）、`test_apps/rtk_pure/**`、`test_apps/rtk_spp_peer/**`、`tools/tidy_check/tidy_check.py`、`tools/g7b_check/g7b_check.py`。

未验证：

- HIL-0..6、实板双会话 scatternet、配对后重启、白名单回退、电源保持电压、运行时 heap/栈/TWDT。
- ASan 泄漏检测（ptrace 限制）。
- 权威 astyle_py、`cpp_cost.py`（未要求，未重跑）。
- esp32c3 的 test-app clang-tidy（工具硬编码 xtensa target，见 S6）。

## 9. 结论

- **必须修复：0 项。**
- **上一轮 4 项必修 + 1 项可选的修复状态：PASS（以当前源码与独立复跑的回归为准）。**
- **建议修复：7 项（S1–S7），均为异常路径恢复、诊断可见性或门禁健壮性，不阻断当前功能。**
- **条件风险：7 项（Q1–Q7），缺少可达时序或只能由实板判定，已与确定性结论分开。**
- 软件门禁本轮独立复跑通过：冻结 115/115、host 8/8、T13 29/29、真实组合根 12/12、ESP32 构建 0 warning、esp32c3 纯组件构建 0 warning、G7b PASS、G5b 27 文件 0 发现、ASan/UBSan 8/8（LSan BLOCKED）。这些仍不能替代 HIL；最终签字需人工完成。

---

本报告只新增本文件；未修改产品代码、测试、配置或冻结输入；未执行 git 写操作、安装下载、串口或硬件动作。临时构建与日志位于 `/tmp/r2-*`，不写入仓库。
