# K2：SPP 非阻塞多在途发送与回复反压

状态：**ACCEPTED（待实施）**。设计决定已确认；实施与冻结契约定稿另行下达。依据：[ADR-0011](../adr/ADR-0011-data-path-memory.md) §4、§8～12及[修订指令](ADR-0011-revise.md)。

## 1. 目标与前置

以每会话TX字节环和worker多在途替换全局stop-and-wait；删除async_tx任务/队列。健康连接中，TX满导致该会话dispatch暂停，不能丢掉已执行请求的回复。保留spp worker，隔离可能阻塞的BTC API投递。

前置：K1验收、本卡冻结接口定稿。Q4窗口和Q6简化A已接受：App按约3KiB内线路字节窗口等待，超时不自动重发副作用命令；全局及每会话在途≤4，正常路径slot队列不会满。先完成host替身匿名失败/CLOSE故障验证，不再以SDK回调终结屏障证明作为实施前置。

## 2. 范围与允许修改文件

| 文件 | 范围 |
| --- | --- |
| `components/spp_server/spp_server_tx.c`、`components/spp_server/spp_server_session.c`、`components/spp_server/spp_server_init.c`、`components/spp_server/spp_server_gap.c` | TX状态/生命周期/worker接入与GAP控制请求调度；保留C语言边界 |
| `components/spp_server/spp_server_rx.c` | 仅TX_SPACE/唤醒共用机制所需接入，RX块和直通留K3 |
| `components/spp_server/private_include/spp_server_priv.h` | reply/PHONE stream、预留、在途与会话关闭状态；无全局故障态 |
| `components/spp_server/spp_transport.cpp` | credit适配；只经rtos映射esp_err_t |
| `components/link_core/link_service.cpp` | dispatch前reserve、pending请求/一次执行、push合并、取消/暂停恢复 |
| `main/modules/bt_link/bt_link_module.hpp`、`.cpp` | 接入新适配器与会话暂停，移除start_tx/async成员 |
| `main/modules/bt_link/async_tx.hpp`、`.cpp` | 删除实现与引用，不保留备用任务 |
| `main/app_main.cpp`、`main/app_context.hpp`、`main/data/app_data.hpp`、`main/def/link_def.hpp`、`main/modules/system/system_cmds.cpp` | 移除async启动/旧统计，增加TX状态快照；如字段实际位于其他main/def头，先在报告列准确路径 |
| `main/modules/debug/trace_text.cpp` | 只追加新事件文本映射 |
| `components/spp_server/CMakeLists.txt`、`components/link_core/CMakeLists.txt`、`main/CMakeLists.txt` | 源与门禁接入 |
| `tools/t13_host_check/**`、`tools/main_host_check/**`、`test/services/**`、`test/app_fw/**` | 故障替身/接口迁移/等价或增强回归，不减少既有覆盖 |
| `docs/reports/K2.md` | SDK证据、内存/性能与验收报告 |

以上范围只覆盖本卡行为；新增私有实现文件须有明确必要性。源码文件名以仓库为准，不重命名冻结host依赖的文件。

## 3. 冻结变更提案

需架构方逐项定稿并批准：

- `components/spp_server/include/spp_server.h`：新增非阻塞reserve/commit/cancel/stream接纳、TX_SPACE/统计及生命周期契约，旧write完成语义保留或显式废弃，不偷换成功含义。
- `components/spp_server/include/spp/spp_transport.hpp`、`components/link_core/include/core/transport.hpp`：纯逻辑reply credit与适配声明；host假Transport同步更新。
- `components/link_core/include/core/link_service.hpp`：pending/reservation/暂停恢复与统计契约；原on_data兼容需要有明确行为，不能让一次传入多帧越过反压。
- 若需要纯组件新增trace码：`components/link_core/include/core/trace_codes.hpp` 只追加、不重排。
- ADR-0007服务/传输语义、ADR-0009 TX诊断契约；由架构方更新 `docs/frozen.sha256` 对应项。

未列冻结头不允许顺手修改。`test/host/**` 不改；如纯Transport扩展会破坏冻结假传输源码，采用有兼容默认实现的可选credit接口或先报告契约冲突，不修改/跳过旧测试。生产SPP装配必须在init验证credit能力，不支持则初始化失败进入控制台，不能悄悄回退到有副作用后才尝试write的旧模式。

## 4. 必须成立的机制

1. 每会话reply2048B；仅唯一PHONE另有stream可用容量4096B，ALARM/PENDING不分配stream。合并区1024B；全局及每会话在途均≤4，worker在cong或credit满时暂停提交，不逐个等待WRITE_EVT。SDK深拷贝不等于无阻塞/无堆，只有worker调用SDK发送/断开。镜像SDK的 `SLOT_TX_QUEUE_SIZE=10`，用C `_Static_assert` / C++ `static_assert` 保证每会话≤4<10及全局≤4，注释引用本地 `components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c:37`，升级SDK复核；全部SPP写入只经本驱动。
2. 在Router副作用前取得280B回复reservation；满保存一完整请求、停止该会话dispatch/读取，另一会话/生命周期/定时器继续运行。恢复由TX_SPACE level状态+通知驱动，commit不重新抢空间。无回复/角色断开释放预留，已执行操作绝不重跑。
3. 手机/报警器两策略均保持；push最多合并一份最新40B，不覆盖已保留回复；GNSS只给PHONE且从App环整条复制入TX.stream，容量不足不接纳、不commit，cancel后下轮重试。租约只覆盖≤4096B锁外复制，不跨等待；TX持有完整句后worker可分块提交，reply只在句边界插入、跨会话公平。PHONE关闭按正常生命周期清空其未提交流、解绑后复用这唯一流区。
4. 在API前绑定在途项以处理“回调早于返回”；关闭保留submitting源区间/users直到API返回，正常CLOSE清理后复用。generation仅用于丢弃CLOSE后的迟到事件、计stray_write，不能释放新会话credit。短写/失败不盲目重试。
5. 正常初始化回调模式中handle=0/len=-1来自NO_CONNECTION或slot fixed_queue满；每会话≤4<10使NO_RESOURCE正常不可达。若仍收到匿名失败，计anon_write_fail+trace，请求断开当前有在途分块的会话，由CLOSE正常清理；不猜owner、不减未知credit，无在途会话继续服务。不设全局故障、不退仅控制台、不增加SDK屏障证明要求；host替身注入此异常分支验证即可。
6. callback无阻塞/分配/解析/格式化；mux内不调用RTOS/SDK，索引与copy有界且临界区每段≤1KiB，4096B整条复制在保留区锁外完成并一次发布。统计区分accepted/completed/cancelled/error与cong/high-water/paused、anon_write_fail/stray_write，不混同应用ACK。

K2先用假stream生产者验证低优先级边界；正式App环接入在K4，K2不实现GNSS订阅。

## 5. 测试与门禁命令

确定性用例：16请求/8以上排队、副作用仅一次、reply满暂停/可用恢复、无回复取消credit、早到/重复/迟到WRITE、CLOSE后generation清理及正常重开、CONG反复、WRITE长度不符、同步失败、close发生在copy/API/回调各阶段、push合并、公平且无忙轮询。host替身注入匿名NO_CONNECTION/NO_RESOURCE，验证anon_write_fail/trace、仅关闭当前有在途分块的会话、CLOSE清理、迟到stray_write、无在途会话持续服务及后续正常准入；并覆盖全局/每会话≤4的credit不变量。假PHONE流覆盖4096B整条接纳、空间不足零接纳/不消费、ALARM无流区、后续App环覆盖不影响TX持有句、回复不插句。替换旧async_tx测试为相同负载下新实现测试，保留场景覆盖说明，不能仅删除用例使ctest变绿。

```sh
. ~/esp/esp-idf/export.sh >/dev/null
cmake -S tools/t13_host_check -B builds/k2-t13_host_check
cmake --build builds/k2-t13_host_check
ctest --test-dir builds/k2-t13_host_check --output-on-failure
cmake -S tools/main_host_check -B builds/k2-main_host_check
cmake --build builds/k2-main_host_check
ctest --test-dir builds/k2-main_host_check --output-on-failure
idf.py build
idf.py size
idf.py size-components
python3 -I tools/g7b_check/g7b_check.py --build build
sha256sum -c docs/frozen.sha256
```

必须再执行ADR-0011 §10.2九套普通+ASan、格式/G5b/分层/双架构门禁及两会话策略配置；所有自有目标strict warnings，0告警。主工程build/、其余builds/。不关闭测试/告警。

HIL列计划、当前NOT_RUN：设备/蓝牙负载操作另行授权后，同环境对比win16、K3新增的字节窗口up256、下行吞吐/应答延迟、CONCURRENT报警推送/心跳和PHONE_PREEMPT生命周期。仓库已有工具，真实命令如下（all包含无窗口up洪泛负例，逐阶段判读，不要求TOTAL lost=0）：

```sh
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode all
```

窗口模式的新增实现与命令见K3；K2不能把当前up当作已有字节窗口。

## 6. 验收与交付

无async_tx任务/队列/启动引用；worker保留；健康连接下拥塞零回复静默丢失、恰好一次dispatch、故障取消可观测；全局及每会话credit永不超4，匿名失败计数并经候选会话CLOSE清理。每会话reply2048B、PHONE独享stream4096B及merge1024B合计9216B，其他角色无流区；全局静态/boot账按ADR §8更新。下行必须≥37.5KiB/s（基线1.5倍），探索2倍；负载最低内部8bit堆≥81920B、最大块≥32768B。未测不写PASS；协议窗口内win16/up256条件见ADR §4.5/10.1。

报告 `docs/reports/K2.md`：SDK路径/版本/回调证据，冻结变更批准，逐用例/门禁状态，基线与新static/heap/栈、sizeof(Item)/在途表，参数及吞吐/延迟/取消证据。K3前保留可独立验证的现有RTCM路径。没有硬件授权时HIL为NOT_RUN；冻结接口未完成定稿时仅相应项BLOCKED，不重新提出已接受的Q4/Q6设计决定。

不commit/push、不串口/烧录/安装下载；本轮只修订文档，不执行其中实现命令。
