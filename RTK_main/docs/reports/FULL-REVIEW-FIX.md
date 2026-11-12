# 全工程复审五项修复

日期：2026-11-11。实现：Codex / GPT-6.1-sol，唯一产品代码集成者；人工负责人 UNKNOWN（待人工审查签字）。主线程只审查方案、源码及证据。采用 embedded-engineering（esp-idf）与 ponytail 最小完整修复原则。

## 结论

五项授权修复均已完成。修复后 host **8/8**、T13 **29/29（原22+新增7）**、真实 main 组合根 **12/12**、冻结 SHA、变更源文件 clang-format、ESP32 完整构建、G5b、G7b 均 PASS。ASan/UBSan 构建 PASS，运行因 LeakSanitizer ptrace 限制 BLOCKED；未关闭 sanitizer。硬件/HIL 未执行，不以 host 或构建通过替代。

本轮日志：`docs/reports/logs/FULL-REVIEW-FIX/`。所有 PASS 来自本次修复后的执行；未用旧 independent 日志冒充。最终源码清单/哈希见该目录 `changed-files.txt`、`final-sources.sha256`。构建唯一工作目录为 `/tmp/rtk-full-review-fix-1mf12pxp`，ESP32 构建子目录 `idf/`；SDK 配置复制到此目录，未更改项目 sdkconfig。

## 调用者与协作证据

实现前调用 CodeGraph，追踪 `rtk_bt_spp_poll_event`、`rtk_bt_spp_disconnect`、worker、main 链路任务与白名单路径，并用当前源码核对：poll_event 由 main 和 T13 调用；disconnect 由 SppTransport 与 T13 调用；note_bt_failure 由 app_main 调用；LinkService::on_data 由 main 与冻结 host 测试调用。主线程已执行专用 LSP references，并报告 poll_event 部分位置陈旧/源码片段错误；依用户补充以当前 CodeGraph、源码及 T13 核对补齐，无重建 LSP 客户端。

Herdr 分屏与 codex/gpt-6.1-sol 模型由主线程验证。子会话 `HERDR_ENV=1` 检查及 CLI 帮助可读，但 agent list / pane current 被运行时 PermissionDenied 拒绝；仅为子会话局部限制，不代表整体分屏未完成。后续未再次访问 Herdr，也未绕过限制。

## 1. 生命周期可靠交付与合并通知

文件：`components/rtk_bt/rtk_bt_spp.c`。

保留现有16深 FIFO。每槽 OPEN 发布后设置 lifecycle_pending，真实 CLOSE 即使已到达，仍要等该 CLOSE 从公共 poll_event 交付后才允许槽复用；同数字句柄也跨槽检查未交付生命周期。DATA 每槽至多一个待交付通知，RX_DROP 每槽独立饱和累计，保留 handle 归属。回调不等待应用消费。

可靠积压上界来自接纳背压：每个槽至多 OPEN、DATA、RX_DROP、CLOSE 四个待交付事件，两个槽合计 **8 ≤ 16**，编译期检查容量。未 poll 时继续重连只能被拒绝，不能反复复用槽生成无界生命周期；不是根据“同时最多两个连接”推断历史积压上界。两个槽共享原 FIFO，生命周期按 BTC 回调顺序交付，旧 CLOSE 不会排到复用后的 OPEN 后面。未发布 OPEN 的拒绝连接不向应用发布幽灵 CLOSE。

接纳背压的可见行为：应用暂不 poll 时新连接可能被拒绝，已接纳会话的 OPEN/CLOSE 仍可靠交付；不承诺保存所有被拒绝连接的应用生命周期。拒绝仍由既有有界控制表处理。

新增行为回归 `lifecycle_burst_churn`：两个未 poll 的 OPEN、40次/会话 DATA 突发、倒序跨槽 CLOSE、40次未发布连接的重连/关闭；验证8个事件顺序、零事件丢弃、无幽灵 CLOSE、同句柄之后能接纳并读数据。`drop_counts_by_handle` 验证两会话各自溢出累计和 handle，不串账。

## 2. 普通断开异步提交与失败重试

文件：`components/rtk_bt/rtk_bt_spp.c`；替身和回归位于 `tools/t13_host_check/`。

rtk_bt_spp_disconnect 只在临界区登记每槽 pending，并以非阻塞通知唤醒现有 worker。未知句柄仍返回 NOT_FOUND；重复请求幂等合并。该状态独立于 GAP/REJECT 控制表，普通请求不会占用或挤掉拒绝命令。ESP_OK 表示请求已被驱动接管，结果仍通过真实 CLOSE 报告。

worker 调用 SDK，失败保留 pending，按100ms最小尝试间隔重试；同一轮每槽最多尝试一次。成功后等待真实 CLOSE，不重复提交，不合成关闭。SDK 或 worker 本身永久停滞时，不能承诺物理断开的完成时间；链路调用者不再等待 SDK 无限投递，仍能处理事件/命令，写调用继续遵守自身有界等待。

disconnect_submitting 期间不仅原槽不可复用，所有槽都禁止接纳相同数字 handle，覆盖“旧 CLOSE 已 poll、另一槽空闲、旧 SDK 仍阻塞”的窗口。CLOSE 清除该代 pending；旧 API 返回前不能把结果写入新会话。

REJECT 保留在途条目，先全表查重再使用空槽；失败按间隔重试。旧拒绝在 SDK 中阻塞时若 CLOSE 后出现同 handle 新 OPEN，重新武装该条目；旧 SDK 返回的成功/失败都不能结算后继拒绝。新的 CLOSE 可取消后继拒绝。成功投递的拒绝等待真实 CLOSE。

新增回归：`disconnect_async_and_reject`（门闩阻塞 SDK 时调用者返回、重复合并、另一会话读数据、拒绝不丢）；`disconnect_retry`（失败后有界重试、成功不重复、真实 CLOSE、复用后新断开）；`disconnect_reuse_across_slots`（CLOSE交付后SDK仍阻塞，另一空槽不得接纳同handle）；`reject_dedup_hole`（前槽空、后槽已有重复拒绝）；`reject_rearm_after_close`（旧拒绝SDK门闩+失败返回、新代拒绝仍提交）。原22场景全部保留；一个旧场景在要求槽复用前显式消费 CLOSE，符合新的接纳背压。

## 3. 白名单保存失败恢复

文件：`main/app_link.cpp`、`main/app_link.hpp`、`main/app_console.cpp`。

应用层消费 service dirty 后置 whitelist_pending，仅保存成功清除。配对升级、手机0x44清空、控制台 wl clear 统一进入该状态；自动重试每1000ms至多一次尝试，保存最新 RAM 表，容忍32位时钟回绕。控制台显式 clear 立即尝试并保留原 StoreError 返回；失败也保留 pending，之后自动恢复。status 显示 pending 和最近存储错误，成功后恢复 OK。

冻结 consume_whitelist_dirty 的读取即清除语义未变。0x44 仍先发 RAM 接受 ACK，再由 app 保存，未改为“ACK 保证落盘”。永久 NVS 故障时 RAM pending 保留且持续限速重试，但不能保证成功；掉电发生在成功落盘前仍可能恢复旧白名单，未引入事务日志或修改冻结持久化接口。

真实 main 回归 `save_recovery`：0x44 ACK先于保存、失败pending/status、999ms不重试/1000ms恢复、配对自身dirty与首次保存失败、控制台clear失败ACK后恢复、最新RAM覆盖、时钟回绕。每个 Kconfig 构建独立进程执行。

## 4. 完整帧边界预算与公平调度

文件：`main/app_link.cpp`；新增 `tools/main_host_check/` 独立工程，不进入冻结 test/host。

经主线程先审查方案后实施：事件先按FIFO处理有限批次，命令和到期tick优先；整轮从命令/tick前开始共享20ms RX启动预算、256字节预算和一个完整帧预算。使用已有 LinkService/解析器，逐字节 on_data，通过真实 frames_rx 变化判断完整帧并立即让出。每槽64字节预读及offset保留，半帧仍保存在原解析器；下一轮无需新DATA通知继续消费。每字节前、预读返回后都复查截止时间，读返回前发生抢占而过期时，该轮不再喂任何字节。

实际尝试会话时才推进轮转起点；tick耗尽时间时保持上轮起点，避免跳过另一会话。完整帧后优先另一槽。有任一待RX或预读剩余时 poll_event 用0超时，空闲才等待20ms；不为快速突发每帧人为增加20ms。任务末尾原 vTaskDelay(1) 保留，当前 CONFIG_FREERTOS_HZ=1000，确保IDLE获得运行机会。

**实际软件上界与限制：**

- 单轮RX最多一个完整帧，因此最多一次冻结的200ms同步回复，消除256字节内10次等待组成约2秒的批次。检查时尚有不足20ms预算的一帧仍可同步等待200ms；RX等待窗口为约20+200ms，另有CPU/RTOS调度开销，不能把20ms称为硬截止时间。
- 冻结 LinkService::tick 内部最多对两个ALARM各同步写200ms，因此一次tick仍可能等待400ms；它耗尽整轮预算后该轮不再开始RX。时间预算阻止再叠加RX等待，但没有改变tick内部两次写。若tick开始前的同步命令/NVS本身已耗时，无法凭此对整轮wall clock作硬上界保证。
- 在仅同步写等待的模型中，一轮最多RX一次200ms或tick两次共400ms；不再是十次RX回复加tick。tick/命令在下一轮优先获得处理机会；对端心跳帧在另一会话让出后轮转处理，tick消耗预算的轮次不会改变其优先起点。
- NVS调用、日志、抢占、系统停滞无新的硬截止保证；因此不承诺所有拥塞应答≤100ms，也不承诺1000±200ms推送抖动在任意SDK拥塞中成立。冻结Transport写回200ms与tick同步遍历限制需要HIL和后续契约裁决，未擅自缩短常量或新增冻结接口。

主程序回归运行真实 app_link.cpp、SppTransport、LinkService、LinkManager、协议解析器，只替换IDF/存储/字节传输边界，以虚拟时钟确定性注入200ms同步等待：`rx_fairness` 验证单帧让出、命令优先、tick耗尽预算后下一轮另一会话心跳、十帧最终完整消费；`rx_fast_partial` 验证快速十帧无20ms额外poll等待、跨轮半帧、预读被抢占后零帧且字节完整保留；`rx_tick_budget` 验证两个ALARM真实tick合计400ms且该轮零RX。`lifecycle_application` 验证真实组合根按OPEN/CLOSE/同handle OPEN顺序释放重建并正常处理新连接查询。

## 5. BT初始化失败policy

文件：`main/app_link.cpp`。

s_policy 以现有 constexpr policy_from_config() constinit 初始化；start未运行、BT失败的console-only状态也遵守Kconfig。note_bt_failure签名及调用未变。`policy_failure` 分别在CONCURRENT和PHONE_PREEMPT编译下，验证未启动任务时read_snapshot的policy正确、bt_ready=false。其余真实组合根场景也分别编译，合计六场景×两配置=12个独立进程测试。

## 实际命令与结果

各日志包含实际完整命令和逐步EXIT。下表中 `W=/tmp/rtk-full-review-fix-1mf12pxp`，所有需IDF环境的新shell先 `. ~/esp/esp-idf/export.sh >/dev/null`。IDF执行均设置 `IDF_COMPONENT_MANAGER=0`，未安装下载。

| 状态 | 实际命令/结果 | 本轮日志 |
| --- | --- | --- |
| PASS | `cmake -S test/host -B $W/host`；`cmake --build $W/host -j4`；`ctest --test-dir $W/host --output-on-failure`，8/8 | host.log |
| PASS | `cmake -S tools/t13_host_check -B $W/t13`；build/ctest同上，最终29/29，原22+新7 | t13-final.log |
| PASS | `cmake -S tools/main_host_check -B $W/main`；build/ctest同上，最终12/12，各场景独立进程 | main-final.log |
| PASS | `sha256sum -c docs/frozen.sha256`，全部OK | frozen.log |
| PASS | `clang-format --style=file --dry-run --Werror <changed-files.txt中全部.c/.cpp/.h/.hpp>`；之前统一 `-i` 格式化一次，修正后仅格式化受影响文件 | format-final.log、format-apply.log |
| PASS | `idf.py -B $W/idf -DIDF_TARGET=esp32 -DSDKCONFIG=$W/sdkconfig -DCMAKE_EXPORT_COMPILE_COMMANDS=ON build`，唯一新目录完整构建；真实产品修正后同目录 `idf.py -B $W/idf build` | idf.log、idf-final.log |
| PASS | `python3 -I tools/tidy_check/tidy_check.py --host-build $W/host --idf-build $W/idf --test-app-build /tmp/rtk-deepseek-full-review.W0mIk7/testapp-esp32 --jobs 4`，27文件0发现；只复用允许的testapp编译数据库 | g5b-final.log |
| PASS | `python3 -I tools/g7b_check/g7b_check.py --build $W/idf`，17个自有目标文件，白名单6/6 | g7b-final.log |
| PASS | `idf.py -B $W/idf size` 与 `size-components`；最终bin 626688 B，size段合计626568 B，IRAM63.16%、DRAM50.66% | size-final.log |
| PASS / BLOCKED | ASan/UBSan：`cmake -S test/host -B $W/asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"`；build PASS；ctest EXIT8，LeakSanitizer fatal明确提示ptrace不支持，运行BLOCKED；未关闭sanitizer重跑 | asan.log |
| NOT_RUN | 权威astyle_py，按禁止安装/下载约束不执行 | — |
| NOT_RUN | 本轮双架构testapp重建、cpp_cost.py；纯组件未修改，按指定门禁仅复用testapp数据库，执行G7b | — |
| BLOCKED / NOT_RUN | 此前rtk_main_guard拒绝硬件/串口动作为既定阻断；本轮未重试；HIL-0..6、实板时延、断连重连、配对重启、电源保持电压均NOT_RUN | 既有阻断见FULL-REVIEW-OMP-DEEPSEEK.md §10 |

### 初次失败与修正后的通过

- 初次main工程编译 FAIL（main.log，EXIT2）：新host替身缺少既有RAII所需portENTER_CRITICAL/portEXIT_CRITICAL别名。只在新替身头补齐映射，未改冻结RAII或产品锁语义。
- 修正替身后的main首轮运行10/12 PASS、2/12 FAIL（main-rerun.log，EXIT8）：公平调度场景暴露tick耗尽预算仍推进RX轮转起点，另一会话被跳过；失败时测试继续解引用空optional触发断言。修正产品起点推进时机，并为失败诊断加has_value检查，不削弱成功oracle。
- 修正后main-final.log为12/12 PASS。原T13批次28/28 PASS（t13.log）；审查补充跨代拒绝重新武装后新增第29项，t13-final.log为29/29 PASS。
- 真实产品修正后仅重跑受影响main/T13、format、IDF、G5b/G7b和size；host纯组件及冻结文件未改，已通过项目不无故重复运行。初次日志保留，没有覆盖失败记录。

## 文件范围、契约及剩余边界

修改产品：`components/rtk_bt/rtk_bt_spp.c`、`main/app_link.cpp`、`main/app_link.hpp`、`main/app_console.cpp`。修改既有测试：`tools/t13_host_check/{CMakeLists.txt,t13_host_check.c,shim/fake_idf.c,shim/fake_idf.h}`。新增独立测试：`tools/main_host_check/{CMakeLists.txt,main_host_check.cpp,shim/freertos/task.h,shim/esp_system.h,shim/esp_task_wdt.h}`。报告/日志仅位于docs/reports。无泛用测试框架、额外依赖或无关重构。

冻结公共头、test/host、docs非reports、根规则/分析配置、sdkconfig.defaults、分区、reference均未修改。保留现有GPIO26电源保持，app_main未改。保留所有原有/未跟踪文件；仓库根所在父git原本将整个RTK_main显示为未跟踪，没有执行reset/clean/stash/commit/push。

公共接口签名、帧协议、0x44 RAM ACK、200ms写回、consume_dirty语义未改变；异步disconnect的ESP_OK为已接管请求，实际关闭仍以CLOSE确认。main状态快照为非冻结应用接口，增加pending/error显示。按用户指定的GPT实现及真实溯源要求，新源文件署名Codex / GPT-6.1-sol、人工负责人UNKNOWN；与旧模板的DeepSeek字样不同，在此记录溯源规则偏离，不冒称本轮由旧模型生成。旧文件既有来源注释保留。

NVS永久失败、成功前掉电、SDK永久停滞、控制表超出既有有界认证负载、实际无线吞吐/句柄事件顺序/运行时heap栈/TWDT仍需对应硬件证据；本轮不扩展复审报告中未证实的其他条件风险。未执行任何串口、flash、monitor、erase、efuse或硬件动作，也未绕过此前自动审批拒绝。授权本轮实现与软件验证已全部完成，人工签字与硬件验收待后续独立流程。
