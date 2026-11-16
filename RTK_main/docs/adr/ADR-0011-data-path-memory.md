# ADR-0011：数据通路与内存整合

- 状态：**ACCEPTED（待实施）**；用户已确认设计决定，实施任务仍需单独下达。
- 输入：[起草任务书](../tasks/ADR-0011-brief.md)、[修订指令](../tasks/ADR-0011-revise.md)。本轮只修订本 ADR、K1～K5 卡和摘要；不修改代码、既有冻结文档或 `docs/frozen.sha256`，不 commit、不连接串口。
- 前置：ADR-0003 的 C/C++ 分工、ADR-0007 的执行器/数据中心/服务边界、ADR-0009 的诊断出口、ADR-0010 的 GNSS 控制器。
- 适用硬件：任务书已确认的经典 ESP32、无 PSRAM、BR/EDR SPP。没有获得 SPI/SD 引脚、卡型号、观测命令的新硬件事实。
- 实施顺序：K1 → K2 → K3 → K4 → K5；K6 仅未来占位。§12 决定已接受；各卡须在冻结契约定稿并单独下达后实施。

## 1. 背景、证据与问题

### 1.1 实测基线（引用任务书，非本轮复测）

方案 A 提交 `9a7d7c5` 已将 link/spp_worker/async_tx 栈调为 4096/2560/2048 B。任务书记录静态 DRAM `108,671 / 124,580 B`、空闲堆约 111 KB、负载最低 98.4 KB、最大连续块约 100 KB。124,580 B 按该次报告的预算记账，实施时须以同配置 `idf.py size` 与 map 确认，不能当作芯片全部 DRAM。

| 任务 | 负载下最小栈余量（B） |
| --- | ---: |
| link | 2288 |
| io | 1784 |
| async_tx | 1300 |
| spp_worker | 1672 |
| trace_out | 1324 |
| console | 2712 |

BlueZ `spp_stress`：rtt～win8 零丢失，约 300 rep/s，下行约 25 KiB/s；win16 丢 8 个回复；up256 丢 8～37 帧。任务书给出的原因与源码一致：`AsyncTxTransport::kDepth=8` 满即失败；SPP RX 每会话 4096 B，DATA_IND 回调 `xStreamBufferSend(..., 0)` 满时丢字节；kLink 每会话只缓存 64 B，每轮预算 256 B 且限制产品帧分发数。SPP 回调模式没有本工程可用的对端 RX 反压。

这些数字是历史测量，**不是新方案已达到的结果**。仓库现已有 `tools/hil/spp_stress.py`（rtt/win/up/bad，soak 由 `--duration` 驱动）和 `tools/hil/spp_rtcm_mix.py`；前者 `all` 包含无窗口的 `up` 洪泛。K3 将新增 `upwin/--window-bytes` 字节窗口上行模式，现有洪泛仅作显式丢弃负例，不能把 `all` 的总丢失直接用作窗口内零丢结论。

### 1.2 源码核对所得修正

1. `components/spp_server/private_include/SppServerPriv.h` 中 `s_spp_tx_inflight` 限制的是**全驱动**至多一次未完成提交，比“每会话一次”更严格。每次最多 512 B，等待 WRITE_EVT 后才能继续。
2. `main/def/RtcmDef.hpp` 的 `kRtcmBatchBytes=2×1029+256=2314`。`RtcmBatch` 只有 `span` 和拒绝计数；DataCenter 的 Slot 复制描述符，没有再复制 RTCM 负载。不能把 Slot 记成 2314 B 负载副本。
3. 最大产品线路帧是 `20+256+4=280 B`；旧注释中的 278 B 不作为容量依据。AsyncTx 的 Item 在 ESP32 ABI 下预计 292 B，精确值须由编译产物确认。
4. `fw::FanoutRing` 已冻结：2 B 长度、4 读者、独立 tail、覆盖最旧完整记录，attach 从最新开始。尚未用于 main。其现实现对非二次幂容量在 u32 回绕时可能旋转存储，直接返回长期有效 span 不安全。
5. `LinkService::on_data()` 先执行 `router_.handle()`，再调用 `Transport::write()`；仅替换发送驱动仍会在满时丢掉已执行请求的回复，必须同时增加 dispatch 前的容量预留。

### 1.3 SDK 事实与适用边界

本地 ESP-IDF 的 `components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c:37` 定义 `SLOT_TX_QUEUE_SIZE 10`。在正常已初始化的回调模式中，`btc_spp_write`（约 888～943 行）在 slot 不存在/已断开时返回 NO_CONNECTION，或每 slot `fixed_queue` 满时返回 NO_RESOURCE，失败回 `WRITE_EVT(handle=0,len=-1)`。深拷贝在约 964 行；完成回调与队列释放约 1127～1142 行；CLOSE 释放槽约 1188 行。`esp_spp_api.c` 中 `esp_spp_write` 仅向 BTC 投递。驱动在初始化前/关闭后禁止提交；NEED_INIT 等状态异常按提交失败关闭相关会话处理。

因此：返回成功只说明提交/复制完成，不能等同于对端收到；SDK 内部会分配堆，API 投递可能等待 BTC 队列。保留 spp worker 隔离这种等待。全局与每会话在途均不超过4，且全部发送只经本驱动，故每 slot 队列占用≤4<10，正常路径 NO_RESOURCE 不可达。K2 用编译期断言与注释登记该 SDK 常量及来源；匿名失败仍防御性计数并关闭当前有在途分块的会话，由 CLOSE 正常清理，不设置全局故障或恢复屏障。

## 2. 决策与架构边界

| ID | 决策 |
| --- | --- |
| D1 | GNSS 输出按消费者/报文类别分环：App 流仅 NMEA/hash；静态观测仅二进制且只给 SD。外部报警器不登记 GNSS 读者，不接收任何 GNSS 数据。 |
| D2 | App 每个已接纳 PHONE 会话独立读者、订阅掩码与降采样状态；离线 detach，重连 attach 最新。仍遵守当前最多一个 PHONE 的策略，不借此放宽会话准入。 |
| D3 | 手机 RTCM 独立 SPSC 直通 RtcmPipe；去掉 SessionSink 的 RTCM 负载暂存、rtcm_batch 与 DataCenter 批次槽；RtcmModule 只做统计和控制。 |
| D4 | 每会话 reply 2048B，另给唯一 PHONE 一份可用容量4096B的 stream 区；报警器无 stream 区。worker 两级优先、非阻塞接纳并合并连续提交，全局与每会话在途均≤4。去掉 async_tx 任务/队列。 |
| D5 | dispatch 前为最大回复预留空间；满则只暂停该会话的请求处理。已执行请求不因拥塞重跑、不静默丢回复。可靠性限于连接健康且协议窗口受控，断链显式取消。 |
| D6 | DATA_IND 立即通知 kLink；256～512 B 读取，预算及轮转保证公平；仍需 App 协议窗口，不能承诺无限突发零丢失。 |
| D7 | K5 在通路布局稳定后使用一次性内部 8bit boot 堆，seal 后不再分配；失败退到仅 UART0 控制台。`sys mem` 提供统一内存账。 |
| D8 | FanoutRing 新增 typed mode 和安全 peek/commit 租约；保留 legacy mode。接口、头格式和覆盖例外均需冻结契约正式修订。 |

跨任务仅通过受控 Pipe/环、Slot 小快照、CommandBus。kIo 不直接调用 BtLinkModule；kLink 不调用 GnssModule。新通用环不引入 IDF、FreeRTOS、产品头。C SPP 驱动不调用 C++；分配缓冲由组合根提供。所有运行时实时诊断用 `fw::trace` / C trace hook，文本由低优先级任务输出。

## 3. 数据流、容量、丢弃与拷贝

### 3.1 改前（现有实现）

```text
手机 / 报警器
  | DATA_IND [BTC；不阻塞；满丢字节]
  v
SPP RX StreamBuffer[2 × 4096 B；writer=BTC，reader=kLink]
  | copy -> SessionEntry.rx[2 × 64 B]
  v
会话 Demux[2 × 1029 B；kLink]
  | copy -> SessionSink.bytes_[2 × 1029 B] + frames_[2 × 171 项]
  +-- 产品帧 -> LinkService/Router
  |              -> async_tx[8 × Item≈292 B + 两个 Item + 2048 B 栈]
  |              -> spp tx_buf[2 × 512 B] -> spp_worker[2560 B 栈]
  |              -> 单一全局在途 esp_spp_write -> 蓝牙
  |
  +-- PHONE RTCM -> rtcm_batch_[2314 B；kLink]
                   -> DataCenter Slot<RtcmBatch> [仅描述符，无负载 copy]
                   -> RtcmModule[kLink，统计 + 转发]
                   -> RtcmPipe[4096 B；SPSC；满整帧丢]
                   -> chunk[512 B；kIo 栈内] -> GNSS UART TX[1024 B；IDF 堆]

GNSS UART RX[4096 B；IDF 堆；reader=kIo]
  -> read chunk[512 B；kIo 栈内] -> GNSS Demux[4096 B]
  -> Controller / NmeaParser / 状态 Slot / 诊断
  （尚无 App 输出环，也无静态观测记录）
```

### 3.2 改后（容量为待验收起点）

```text
手机 / 报警器 -> SPP RX[每会话 4096 B，boot 阶段外供]
 BTC 写 + DATA/RX_DROP level 状态 + task notify（mux 外）
  -> kLink 读取缓存[每会话 512 B；可配置 256 B]
  -> 会话 Demux[每会话 1029 B]
     +-- 产品帧 -> pending request[每会话最多 280 B]
     |             -> reserve reply[280 B credit]
     |             -> Router（恰好一次）-> commit reply
     |             -> TX.reply[每会话 2048 B，高优先，满则暂停该会话]
     |
     +-- PHONE RTCM -> RtcmPipe[4096 B；writer=kLink，reader=kIo]
                      [满整帧 drop；非 PHONE 拒绝；共享计数小快照]
                      -> pending UART chunk[512 B；kIo]
                      -> UART TX[1024 B；IDF 堆] -> GNSS

GNSS -> UART RX[4096 B；IDF 堆] -> read[512 B] -> Demux[4096 B；kIo]
  +-- NMEA/hash -> 最新句缓存 + 订阅/到期过滤
  |               -> App FanoutRing[4096 B 起；带类型；覆盖整条]
  |                  writer=kIo，reader=每个 PHONE 的 kLink 读者
  |               -> peek 短租约 -> TX.stream[仅 PHONE 一份，可用4096 B]
  |                  [整条记录一次接纳；不足则 cancel，下一轮重试]
  |                  [租约仅覆盖≤4096 B复制；不持锁/租约等待蓝牙]
  |               -> worker[2560 B 栈；唯一 esp_spp_write 调用者]
  |                  [reply 优先；合并暂存1024 B；全局在途上限4]
  |               -> SDK 深拷贝 -> 蓝牙 App
  |
  +-- 二进制观测 -> 未来 Obs FanoutRing[约16 KiB；记录时才分配]
                   -> SD SPI task[唯一读者；本期不创建、不分配、不驱动]
  +-- Controller/NmeaParser -> 状态 Slot

RtcmModule[kLink] <- 共享通路累计快照；只统计/速率/reset，不承载负载
console -> 统一输出配置入口 -> CommandBus -> kIo 接收机配置 / storage 持久默认
App逐消息 interval -> 同一校验入口 -> 会话订阅（不保存默认，不扩展0x0D）
未来 web / 0x0D订阅与保存扩展：不在本期范围
```

4096 B 静态 StreamBuffer 的可用容量以 `xStreamBufferSpacesAvailable()` 为准（现实现可能保留一字节），窗口计算不能直接假定能存满 4096 B。K5 外供可额外配置哨兵字节，内存表分别列明。

### 3.3 App 环大小与长句的明确边界

typed 记录为 `LE16(payload_len) + uint8_t type + payload`，不含填充，原文保留行尾；type 是映射表 0～31 的稳定类别索引，掩码为 `uint32_t`，不能直接以 0x1F/0x23 等消息 ID 移位。**Q1 已决定：App 环默认4 KiB**；`RTK_GNSS_APP_RING_BYTES` 以 Kconfig 配置、默认4096，2KiB只用于边界试验，8KiB留给实际需求另行变更。4KiB最多容纳4093B负载；超过容量的记录（如4096B SATSINFOA）写者端整条丢弃、累计 `oversize_drop`，不截断、不分片。

本期K4用“4096B完整输入被识别、整条拒绝并显式计数”验收，无容量未决阻塞。若App实际需要完整SATSINFOA，再单独启用8KiB并复验内存门槛；typed mode允许只容纳一条最大记录，§5。GNSS Demux仍4096B，因为二进制解码器已经要求该容量；hash解码器上限1024→4096列入冻结契约定稿。

### 3.4 复制次数的口径

仅统计 RTCM **应用负载入 RtcmPipe 之前**的每个复制边界，不统计蓝牙栈内部、不把 span/计数复制算负载，也不把“完整帧可能含多个 feed 段”当作额外一次。

| 路径 | 改前 | 改后 |
| --- | --- | --- |
| BTC→SPP RX | 1 | 1 |
| SPP RX→kLink read chunk | 1 | 1 |
| read chunk→会话 Demux | 1 | 1 |
| Demux→SessionSink | 1 | 0（RTCM 直接 push） |
| SessionSink→rtcm_batch | 1 | 0 |
| DataCenter RtcmBatch | 0（只有描述符） | 0 |
| Demux/批次→RtcmPipe | 1 | 1 |
| 小计 | **6** | **4** |
| 后续 Pipe→UART pending chunk→IDF TX buffer | 2 | 2 |

任务书“5 次”按其概括口径保留为问题背景，本表以逐边界核对为准。对 App 流：Demux→App 环一次、环→TX.stream 一次、TX→worker 合并区一次、SDK 深拷贝一次；peek 去掉的是“环→读者整句副本”，不是蓝牙端到端零拷贝。未来 SD 可直接写两段 span，但受租约超时/覆盖取舍限制，不能把阻塞 SPI I/O 放入 portMUX。

## 4. SPP 非阻塞 TX、回复反压与 RX

### 4.1 队列、优先级和接纳契约

每会话 TX.reply 2048B；仅为唯一 PHONE 另设一份可用容量4096B的 TX.stream，ALARM/PENDING不分配 stream 区。PHONE关闭时取消其未提交流并解绑，下一PHONE从空区开始；正在API调用的源区间按生命周期保护完成后再复用。记录描述符另计，不占4096B payload容量；使用明确 used/满空状态，不因环哨兵使4096B记录无法接纳。回复全帧原子接纳，`try_reserve_reply(handle,generation,280)` 为一个请求预留最大回复；`commit` 写实际字节并返还多余 credit；无回复/角色拒绝时 cancel。一次至多一个 reservation/会话。ring used、reserved 和 worker 正在复制的区间均不得重复分配。

新增 C 非阻塞 credit/enqueue 接口，返回 success 只表示**驱动取得完整数据所有权**。`kBusy/ESP_ERR_NO_MEM` 表示没有接纳任何字节。原 `spp_server_write` 的等待完成语义不得悄悄改成接纳语义；K2 可保留兼容包装，主通路使用新接口。`core::Transport`/`LinkService` 的冻结契约须修订以支持 reserve/commit/cancel；不得靠未经证明的“检查一次 free 后直接 write”替代预留。

优先级是 reply > stream，worker 每次提交前选择：同一会话已开始的原始 GNSS 句必须保持字节连续，回复只在句边界插入；两个会话轮转，不能让手机饿死报警器。整条流记录已由TX.stream持有，worker可按≤1024B分块提交SDK，不再依赖App环内记录的存续。禁止将产品回复插进NMEA/hash句中。优先级不能抢回已交给SDK的低优先数据；全局及每会话均最多4个在途，限制排队与占线延迟。

**Q5 已决定：整条复制接纳**。BT适配器直接将peek的两段span传给纯C的非阻塞scatter输入接口；驱动为整条记录预留TX.stream空间，在mux外完成总量≤4096B的一次有界复制并原子发布，随后commit消费App环记录。空间不足则不接纳任何字节、不commit，立即cancel释放租约，下一轮重新peek；这期间被写者覆盖的旧记录只计读者 `dropped`。租约仅覆盖本次复制，不跨等待、不保留跨轮发送进度。源记录被消费后，后续环覆盖不影响TX.stream持有的完整句；不增加由覆盖触发的断链路径。该设计去掉读者整句临时数组，仍保留会话TX环及SDK深拷贝。

报警 0x40 周期推送仍支持：使用独立至多一份最新 40 B 待发状态（估算列在元数据预算），可合并旧推送并计数，不侵占已预留回复空间，不改变心跳/白名单策略。此项与请求回复的“已执行必保存”分别统计。

### 4.2 链路任务什么时候暂停、恢复

1. 生命周期事件优先；CLOSE 立即取消对应 generation 的 reservation/pending request/流状态，未执行请求不 dispatch。不能沿用当前 `session_forget` 先 dispatch 尾帧的行为。
2. 从 Demux 得到产品帧时，最多保留该会话一个完整 pending request（280 B），逐字节 feed 可暂停，保留 512 B read chunk 未消费后缀。取得 reply reservation 之后才允许调用可能改变状态的路由。角色转换/准入判断若断开，释放 reservation，禁止副作用重放。
3. 空间不足：标记 `reply_paused`，停止该会话继续读取/dispatch，继续处理另一会话、断开、心跳超时、执行器命令和定时器；无立即可做工作时等待通知，不忙轮询。当前 TCP/RFCOMM 之外没有设备端强制上游反压，App 必须限制在途请求。
4. worker 成功提交并归还环空间后发布 `TX_SPACE` level 状态并通知；kLink 重查完整 280 B credit，成功才恢复。检查状态→准备等待→再检查状态，避免丢唤醒；队列事件丢失仍能从 level 状态恢复。
5. Router 只执行一次。commit 不得因另一生产者消耗空间失败；若出现过期 token/关闭/故障，记录取消原因，不重执行。协议超时不擅自重发有副作用操作；App 根据 seq 对照或重新查询状态。

这里“可靠”指健康连接中已 dispatch 的回复不会因本地队列满而丢。WRITE_EVT 不是应用层 ACK，断链、硬件错误和 App 超出窗口都不在无线端到端保证内。`replies_tx` 延用“成功接纳”含义，另加 completed/cancelled/error 计数，不把入队当作对端收到。

PHONE_PREEMPT：手机抢占报警会话时按正常CLOSE清理并取消其数据，报警器没有GNSS读者/stream区；手机拥塞不阻止抢占/关闭。CONCURRENT：PHONE与ALARM共享worker credit，各自RX/reply/暂停标志，stream仅属于PHONE；报警心跳/0x40仍按既有时限运行。当前最多一个PHONE保持不变；未来增加手机数需要另一次准入/容量审查。

### 4.3 worker 状态机与并发契约

状态不是单一 stop-and-wait 标志，而是会话 OPEN/CONGESTED/CLOSING 与有界在途表的组合：

```text
CLOSED -> SRV_OPEN -> OPEN/IDLE
OPEN + queued + !cong + credit -> RESERVED_SUBMIT
  [mux 内：绑定 slot/generation/序号/长度，标 submitting，保留源区间]
  [mux 外：复制到1024 B合并区；调用 esp_spp_write]
  -> 同步成功：该源区间可回收；在途项等待 WRITE_EVT
  -> 同步失败：取消本次、故障处理；不得重发可能已提交的数据
OPEN -> CONG(true) / WRITE_EVT.cong=true -> CONGESTED
CONGESTED -> CONG(false) -> OPEN（唤醒 worker，继续多次提交）
WRITE_EVT(valid, full len, success) -> 释放一个 credit，更新完成统计
OPEN/CONGESTED -> CLOSE/主动关闭 -> CLOSING -> 正常取消/在途清理 -> CLOSED
```

提交前先登记在途项以允许回调早于API返回；in-flight项有 `submitting/completion_seen/cancelled` 标志，回调只记状态，worker返回后结算一次。正常worker在!cong时连续合并提交，不等每个WRITE_EVT；全局或该会话达4个credit，或达到时间预算，就让出。以C的 `_Static_assert` / C++的 `static_assert` 检查全局≤4、每会话≤4且每会话上限小于本地SDK核实的队列容量10；SDK私有宏不作为组件公共依赖，具名镜像常量旁注明 `btc_spp.c:37 / SLOT_TX_QUEUE_SIZE 10`，升级SDK时复核。4项包括已登记但API尚未返回的提交，不得因取消或早到回调重复归还credit；全部SPP发送只经本驱动。每次合并不得破坏记录边界及已选会话顺序。

| 上下文 | 允许 | 禁止 |
| --- | --- | --- |
| kLink/core1/prio10 | reserve/commit、read RX、消费 TX_SPACE、读 App 环短租约 | 等待 WRITE/CONG、运行时分配、文本日志 |
| spp worker/core0（沿用现有优先级） | 唯一调用 esp_spp_write/disconnect；mux 外 SDK API；公平调度 | 无界 credit、持 mux 调 SDK、运行时分配 |
| BTC 回调 | 更新有限状态、RX timeout=0、trace hook、mux 外 task notify | 等待、解析、分配、格式化、调用可能阻塞的发送 API |
| ISR | 本设计无 ISR API | 调用以上接口 |

portMUX 内只做索引/有界复制（单次 ≤1 KiB）；FreeRTOS 队列/通知、StreamBuffer API、SDK、UART、SPI 一律 mux 外。多阶段 copy 通过 reserved 区间及 users/generation 保护生命周期；不能将“锁外复制”变成被覆盖内存的数据竞争。

### 4.4 CLOSE、迟到事件与匿名错误

- OPEN/CLOSE按正常生命周期清理会话；在途项绑定slot/generation/handle，CLOSE后旧generation的迟到/重复/无匹配WRITE只计 `stray_write`，不归还新会话credit。generation只用于本地清理与丢弃迟到事件，不作为SDK回调新字段，不增加回调终结屏障前置。
- CLOSE立即禁止该会话新接纳/提交，撤销未提交数据并计cancelled；已被API深拷贝的数据只结算状态，不重传。API调用尚未返回时保留源区间/users，返回后完成清理，随后可按正常生命周期复用槽与PHONE流区。
- WRITE_EVT有正常handle时按该会话FIFO匹配最早未完成项，并检查长度/status。短写或错误不能盲目重发尾部，关闭受影响连接并报告；不匹配事件计 `stray_write`。
- **Q6 已决定**：正常初始化下匿名 `handle=0,len=-1` 的原因是NO_CONNECTION或slot队列满。全局与每会话在途均≤4<队列容量10，NO_RESOURCE在本设计约束下不可达；NO_CONNECTION随正常CLOSE收敛。若仍收到匿名失败，累计 `anon_write_fail` 并trace，标记并请求断开当前有在途分块的会话，由CLOSE正常取消其未完成数据。不能猜一个owner、减一个未知credit或重传可能已提交的数据；无在途的会话继续服务，不冻结新准入。
- 不设全局故障态、不因匿名失败退仅控制台、不要求蓝牙停止/反初始化或回调终结屏障。K2以host替身注入匿名失败，验证关闭候选会话、CLOSE清理、迟到统计和继续服务；SDK队列常量升级时复核上述约束。

### 4.5 RX 唤醒、协议窗口与零丢失条件

DATA_IND 复制完 RX 后，在 mux 外通知 kLink；RX_HAS_DATA/RX_DROP/TX_SPACE 为 level 状态，OPEN/CLOSE 不被数据通知淹没。单次 read 默认 512 B（允许比较 256 B），逐帧/逐字节暂停解析；每轮从下一会话开始，按时间与字节预算轮转。保留或重定现有 20 ms 上界并通过双策略 host 用例确认；不能仅把 read 缓冲增大而仍每 20 ms 限一帧。

App保持异步发送，以完整回复释放请求窗口。**Q4已接受约3KiB内的字节窗口**；计算请求线路总字节，不能只数payload：`未获回复的请求线路总字节 + 额外DATA_IND突发 + 安全余量 <= RX实际空闲容量`。名义窗口3072B，若实际RX可用4095B则安全余量按1023B记；需要额外突发或并发RTCM时继续收紧窗口，使总量不越界。16个最大280B请求为4480B，不能一次全部在途。无窗口的up256只作负例；窗口模式每条256B payload需按280B线路帧记账，因此3072B内最多10条该请求在途。

没有回复的命令（如既有 0x0D SET）不能用回复释放窗口，须在 App 做独立字节速率/突发上限，不擅加 ACK。RTCM 也需按输出能力节流，并观察 pipe_drop。status 报 rx_dropped（字节）、RX高水位、事件丢失和暂停时间。发生 RX 字节丢失保留 demux 重同步并可观测，不把丢包误认为协议坏帧。

## 5. FanoutRing 冻结接口修订草案（K1）

只给声明与契约，不是本轮头文件改动。继续使用 `fw::Result`、`LockOps`，不增加平台依赖。原有常量、legacy `init/attach/write/read/detach/stats` 声明和 2 B 格式保留；旧测试不改、不减。扩展 mode 不能与 legacy 记录混用。示意声明中的既有类只列新增成员，完整特殊成员沿用旧头。

```cpp
namespace fw {
inline constexpr size_t kFanoutTypedHeaderBytes = 3;
using FanoutMask = uint32_t;

struct FanoutLease {
    uint32_t generation = 0;
    uint32_t serial = 0;
    uint8_t reader_id = 0;
};
struct FanoutView {
    FanoutLease lease{};
    uint8_t type = 0;
    std::span<const uint8_t> first{};
    std::span<const uint8_t> second{};
};
struct FanoutTypedReaderStats {
    FanoutReaderStats base{};
    uint32_t filtered = 0;
    uint32_t pinned_drop = 0;
};
struct FanoutTypedWriterStats {
    FanoutWriterStats base{};
    uint32_t no_subscriber = 0;
    uint32_t pinned_drop = 0;
    uint32_t oversize_drop = 0;
    size_t high_water_bytes = 0;
};
class FanoutRing {
public:
    // 启动前单线程、非 ISR；typed 专用，容量为二次幂且 <2^31，max_record+3 <= cap。
    // 负载 1..65535；存储寿命覆盖环；无分配；重复 init 拒绝。
    [[nodiscard]] Result init_typed(std::span<uint8_t> storage, size_t max_record,
                                    const LockOps &lock) noexcept;
    // 每 id 一个读者任务、非 ISR；mask=0 不接收；attach 从最新位置开始。
    [[nodiscard]] Result attach(FanoutMask mask, uint8_t &reader_id) noexcept;
    // 仅该读者、非 ISR；无活动租约时更新；不回放旧订阅，tail 移至当前 head。
    [[nodiscard]] Result set_mask(uint8_t reader_id, FanoutMask mask) noexcept;
    // 写者任务、非 ISR；type<32；无匹配读者不写，kOk 且 no_subscriber++。
    // 被 pin 阻挡则不等待、整条拒新 kBusy；oversize 为 kInvalidArg。
    [[nodiscard]] Result write(uint8_t type, std::span<const uint8_t> payload) noexcept;
    // 仅该读者、非 ISR；跳过不订阅记录，最多一个活动租约/id。
    // kNotFound 无记录；kBusy 已有租约；两 span 只含 payload、长度和等于原记录长度。
    [[nodiscard]] Result peek(uint8_t reader_id, FanoutView &view) noexcept;
    // 仅该读者、非 ISR；消费整条并释放 pin；过期 token 拒绝且不移动 tail。
    [[nodiscard]] Result commit(const FanoutLease &lease) noexcept;
    // 仅该读者、非 ISR；释放 pin、不消费；不得在 cancel 后访问 view。
    [[nodiscard]] Result cancel(const FanoutLease &lease) noexcept;
    // 任意任务非 ISR；锁保护下读取；无效 id 返回空统计，不改变状态。
    [[nodiscard]] FanoutTypedReaderStats typed_reader_stats(uint8_t reader_id) const noexcept;
    [[nodiscard]] FanoutTypedWriterStats typed_writer_stats() const noexcept;
};
} // namespace fw
```

契约细节：

- `detach` 有活动租约返回 kBusy；读者必须先 cancel/commit。断链由该读者任务完成，不能从另一任务强制释放正在使用的指针。销毁/重初始化前所有任务与租约必须静止。
- 无 pin 时保持整记录覆盖；覆盖仅给落后的订阅读者计 dropped，被过滤记录计 filtered；legacy 统计口径不变。pin 阻挡覆盖时写者不等待，计 writer `pinned_drop`，并给当时订阅该类型的读者各计一条 `pinned_drop`；这与已经发布记录被覆盖的 dropped 分开。无读者/无订阅计 no_subscriber，不算传输丢失。
- 类型头和记录发布原子可见。长记录写入分为保留空闲区→锁外 copy→锁内发布，读者不可见未完成记录；或 ≤1 KiB 分段锁内复制但整个记录直到最后才发布。旧可读区域和租约区都不可覆盖。每段临界区必须有界；不为 4096 B memcpy 长持 SMP 锁。
- typed mode 限二次幂存储，u32 逻辑位置回绕不搬动内存，差值始终 <2^31。legacy 原算法保留。generation/serial 回绕不能让旧 lease 重新有效；耗尽时要求静止重置或使用更宽 token，K1 以测试和定稿说明选择。
- `read()` 在 typed mode 返回 kNotSupported，typed peek 在 legacy mode 同样拒绝，避免静默丢类型。实现可复用内部逻辑，不能复制两套无关环实现。
- writer 的旧“永远覆盖最旧”语义在 typed 租约模式下修订为“永不阻塞，pin 时拒新”；必须同步 ADR-0007 §3.6-1 和头注释。调用者不能期待既零拷贝 pin 又永不拒新。

## 6. RTCM 直通与 GNSS 输出配置

### 6.1 RTCM

Demux 回调内确认 PHONE 角色后直接向共享 RtcmPipe 整帧 push；回调 span 只在调用期间有效，不保存指针。最大线路帧 1029 B，容量不足整帧丢并 trace；非 PHONE 拒绝，外部报警器不接收 GNSS。产品帧保留一个 pending request，移除整个 SessionSink 大队列和 rtcm_batch，而不是只把数组换名。

共享通路统计 helper 放 main 数据/支持层：kLink 是唯一写者，提供累计 frames/bytes/crc32/last_msg/rejected_role/pipe_drop 等小快照；RtcmModule 每轮读取累计差量、负责 reset 命令/速率展示，不让 BtLinkModule 调 RtcmModule 方法。reset 在同一 kLink 顺序点切换统计 epoch，不并发 reset StreamBuffer，不误改 GNSS 消费者游标。既有 `rtcm:`、`stats:` 两行字段及顺序保持，新增诊断另起新行。

kIo 读 Pipe 后要保留 UART 未完成的 512 B chunk 和偏移；短写/零写/错误时不得直接丢后缀，也不得重发已经写出的字节。按预算重试可恢复错误；不可恢复时计显式 discard 并回到 ADR-0010 控制器恢复流程。断电/探测未就绪时沿用其丢弃/暂停策略但要计数，不向未初始化 UART 写入。UART RX 与 RTCM TX 轮转避免输出阻塞拖垮 GNSS 输入。

### 6.2 订阅、频率与统一配置入口（替代 G-C 相应范围）

保留 G-C 的消息映射及 interval：0x04 GGA（默认1s）、0x05 SATSINFOA（5s）、0x1F HEADINGA（1s、需能力位）、0x21 GST（1s）、0x22 RMC（1s）、0x23 GSA（3s）；NMEA 忽略 talker。interval<0 为绝对值秒（先提升整数宽度再处理 -128），>0 为 1000/v ms 且最短50ms，=0 关闭并查询最新句。原始持续输出不加产品帧封装；无缓存/不支持返回 02 02。

统一的main输出配置入口接受 `SetOutput(source, session_generation, msg_id, period_ms, persist)` 等值对象，共用映射、能力校验和频率边界。**本期外部配置只接串口控制台**；内部App订阅沿用G-C已定义的逐消息interval语义，调用同一校验入口，但不新增协议、不保存持久默认。接收机配置经过CommandBus交给所属执行器，不跨任务直接改模块；未来web与0x0D订阅/保存扩展不在本期范围。区分：

1. **会话订阅**：kLink 独占、关闭即清；向 kIo 发布小型配置快照，包含每会话 period/mask/epoch。kIo 每会话独立 last_sent，到期读者取并集决定是否写该条，读者侧再按自身周期和掩码过滤。无在线订阅读者不写 App 环。配置快照加锁，不能用非原子 payload seqlock。
2. **接收机输出配置/持久默认**：kIo独占，本期仅串口控制台命令可设置并请求保存；命令队列排序，最后成功命令生效，GNSS不支持则返回失败而不持久化。用 `gnss/out` 的版本化有界表blob存默认周期及报文配置，storage负责读写；已有 `gnss/rate/gsv/family/model/baud` 键保持。NVS写失败要报告，不能将“已运行但未保存”说成持久化成功；App逐消息订阅不改NVS。

重连仍 mask=0、无历史回放；NVS 默认仅作为用户显式开启订阅的默认值，不在重连后自动开始流。固定 GNSS 输出超集加 ESP 降采样；实际频率受模块产生速率限制。console 的 GNSS rate 和每类 output 周期通过同一校验，不把观测记录开启混入 App 订阅。

最新句缓存只保留这六类，预算见§8。查询采用kLink→kIo命令→有界邮箱返回≤256B快照，kIo私有大缓存不经Slot大memcpy，禁止用非原子seqlock。**Q2已决定：最新句>256B返回02 02，累计 `query_oversize`，不截断、不分片**；已有缓存内容与持续订阅不因此改变。原始流不受回复payload256限制，但仍受App环整条容量约束。

**Q3已决定：0x0D保持现状**，32B COM查询、SET无回复；本期不添加订阅表/保存字段、不扩展线上协议。`survey_protocol_0530.md` 中头offset19 interval和逐消息查询继续适用；`phone_protocol.md` offset19“预留”需架构方正式对齐。统一入口、控制台持久默认及现有App逐消息订阅可独立完成，0x0D订阅/保存扩展列未来项，不阻塞K4。

K4 并入 G-C 的映射、方言输出超集、GnssService路由、0x02功能字节、hash长句、查询/订阅/NVS与诊断；其 `NmeaOut`/async_tx/非原子seqlock方案由本 ADR 替代。此处不取消 G-C 任何字节级协议回归。

### 6.3 静态观测与未来 K6

只预留：二进制观测独立 typed ring，唯一读者 SD SPI 任务，App/报警器无读者。记录开始才从内部堆申请、停止且读者静止后释放；不经过封存的 boot_alloc。本期不分配环/栈/驱动、不开 SPI、不选引脚。

按任务书估算 OBSVMCMP 约2.2 KB/历元、10Hz约22 KB/s；16 KiB约为速率×300ms×2（约13.2KB后向上留量）。GNSS串口至少460800 baud（8N1理论46080 B/s），还须叠加 NMEA/hash 和实际观测峰值测量。报文集合为有界可配置表；E1-5.4 无观测命令可用证据，具体集合/命令/引脚均 UNKNOWN，待用户补。K6 必须重新验证 SD 写延迟、pin导致的拒新、掉电一致性和堆最低余量。

## 7. boot_alloc、仅控制台降级与 .bss（K5）

### 7.1 接口草案与生命周期

在 `components/rtos/include/rtos/` 新增 RAII 包装头（项目允许新增，旧冻结头不改）。内部只申请 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`，对齐满足栈/字节存储，禁止依赖PSRAM。返回码用 fw::Status；不直接暴露 esp_err_t。示意声明：

```cpp
namespace rtos {
struct BootAllocation {
    const char *owner = nullptr; // 静态字符串，唯一名字
    size_t bytes = 0;
    size_t alignment = 0;
};
class BootArena {
public:
    constexpr BootArena() noexcept = default;
    BootArena(const BootArena &) = delete;
    BootArena &operator=(const BootArena &) = delete;
    // app_main 启动单线程、非 ISR；只成功一次，表容量固定且不从堆分配。
    [[nodiscard]] fw::Status init() noexcept;
    // 仅 init 到 seal/abort 之间；失败不发布 out、不留下半条账；字节数/对齐溢出须校验。
    [[nodiscard]] fw::Status allocate(const char *owner, size_t bytes, size_t alignment,
                                      std::span<uint8_t> &out) noexcept;
    // 启动单线程、非 ISR；所有消费者绑定成功才 seal；之后 allocate 永远 kInvalidState。
    [[nodiscard]] fw::Status seal() noexcept;
    // 启动单线程、非 ISR；仅未 seal 且没有任务/回调/驱动使用块时释放全部，进入 ABORTED。
    [[nodiscard]] fw::Status abort() noexcept;
    // 任意任务非 ISR；seal/abort 后账表不可变；返回描述不含可写数据指针。
    [[nodiscard]] std::span<const BootAllocation> allocations() const noexcept;
    [[nodiscard]] size_t total_bytes() const noexcept;
    [[nodiscard]] bool sealed() const noexcept;
};
[[nodiscard]] BootArena &boot_alloc() noexcept;
} // namespace rtos
```

状态 `UNINIT → OPEN → SEALED` 或 `OPEN → ABORTED`，终态不能重开。固定登记表建议32项，满同分配失败；owner唯一、无所有权转移；SEALED块运行期不释放，arena析构不触发任务存储释放。abort必须明确释放前提，不能拿来回收仍在使用的任务栈。分配源文件登记G7b heap allowlist且文件头注明启动期档位。

组合根先为所有启用子系统分配/绑定，后 seal，最后创建并启动任务。FreeRTOS任务仍用 `xTaskCreateStaticPinnedToCore`，TCB 静态且栈外供，单位字节，沿用核/优先级及原因。Executor已有StorageView可复用；现StaticTask/StaticStreamBuffer仅内嵌数组，新增外供存储包装，不暗改旧冻结头。

C驱动通过纯C的 storage view/config 接收RX/TX/worker栈等存储，指针寿命覆盖驱动与回调；它不包含rtos C++头、不自行调用heap API。UART驱动内部RX/TX仍由IDF申请，单列SDK堆，不伪称已由boot_alloc接管；本次“gnss rx迁移”包括main的Demux存储，IDF RX缓冲只有存在已验证外供API时才迁移。

### 7.2 失败降级

UART0 console的初始化资源与最小命令表先准备、独立于arena；打印和错误处理不能依赖待分配Trace/Log环。任一申请/绑定失败：不启动蓝牙、kLink、kIo、trace_out；确认无消费者后abort全部已申请块；仅注册help/sys mem/sys info/reboot等安全命令。GNSS/BT业务命令返回 `ERR unavailable: console-only`，不保留绑定失效span。

seal后若任务/IDF驱动初始化失败，先停止已启动子系统并等待回调结束，保留sealed块到重启，不释放可能仍被引用的内存，进入console-only。报告失败owner/bytes/阶段及保留量。不可通过动态缩小栈、关告警、转用PSRAM或无界重试“降级”。控制台本身初始化失败的既有应急路径保留，报告为不可提供控制台，不能保证无内存仍能正常REPL。

### 7.3 s_app 与 sys mem

当前 `AppContext` 成员 `{*this}` 使约52KB聚合落.data（任务书给定）。改为 `.bss` 的 `constinit` 零初始化拥有存储，模块引用在显式init原地绑定；可采用分离的零初始化存储与小型依赖对象，必须维护构造不失败/不触硬件、初始化顺序及销毁寿命。不在全局动态初始化调用硬件。以map验证大payload确实进.bss，不以删constinit换取表面结果。

`.data → .bss` 减少镜像中初始化数据，**本身不减少运行时静态DRAM，也不增加空闲堆**；boot迁移则将同一RAM从静态段移到堆，扣除分配开销后整体RAM不会凭空增加。

`sys mem` 经 `cli::Verb/dispatch`、`cli::out_*` 输出：静态.data/.bss/合计（链接符号/size校核，不把Flash rodata算DRAM）、arena状态和owner/requested/alignment/总量、各caps的total/free/min_free/largest（INTERNAL|8BIT、8BIT、DMA分别展示且标明集合重叠，禁止相加），以及console-only失败原因。输出末行OK或ERR；读取封存表无分配，跨任务数据用快照；旧sys heap保留。

## 8. 内存账（B，预算不冒充实测）

配置基准：2个SPP槽、GNSS与trace启用、link4096/io4096/spp_worker2560/trace_out3072栈，App环4096；SD关闭。以下是payload数组的可复算设计账。`Δmeta` 包含ABI、TCB/控制块删除收益、描述符/新lease/指针增长及实际编译布局差异，必须K2/K4/K5用sizeof/map逐项替换；不是允许无限增长的余量。

### 8.1 K1～K4逐项静态变化

| 项目 | 改前静态 | K4设计静态 | 增减 |
| --- | ---: | ---: | ---: |
| link/io/spp_worker/trace_out栈 | 13824 | 13824 | 0 |
| async_tx栈 | 2048 | 0 | -2048 |
| async队列8×292及两个Item | 2920 | 0 | -2920 |
| SPP RX两会话 | 8192 | 8192 | 0 |
| 原SPP tx_buf | 1024 | 0 | -1024 |
| TX.reply 2×2048 + PHONE stream 1×4096 | 0 | 8192 | +8192 |
| worker合并区 | 0 | 1024 | +1024 |
| 在途描述预算4×32 | 0 | 128 | +128 |
| kLink读取缓存 | 128 | 1024 | +896 |
| SessionSink负载 | 2058 | 0 | -2058 |
| SessionSink帧描述2×171×4（ABI估算） | 1368 | 0 | -1368 |
| 单完整pending request 2×280 | 0 | 560 | +560 |
| rtcm_batch | 2314 | 0 | -2314 |
| DataCenter RtcmBatch描述符/相关字段 | 原sizeof | 移除 | 纳入Δmeta，不虚报负载收益 |
| RtcmPipe字节存储 | 4096 | 4096 | 0 |
| Trace + Log payload | 8192 | 8192 | 0 |
| GNSS Demux | 4096 | 4096 | 0 |
| App typed环 | 0 | 4096 | +4096 |
| 最新句缓存预算：4×256 + HEADINGA1024 + SATSINFOA4096 | 0 | 6144 | +6144 |
| 订阅/邮箱/推送/新统计额外预算 | 0 | 1024 | +1024 |
| 其余既有对象 | 不变基数 | 不变基数 | 0，偏差入Δmeta |

新增预算减去删除payload合计 `+10332 B`；`S_K4≈108671+10332+Δmeta=119003+Δmeta B`。相较初稿，2×1024B stream改为仅PHONE一份4096B，净增2048B。未来若单独启用8KiB App环，再加4096，约123099+Δmeta；本期仍以4KiB验收。最后1024B只是设计预算，需证明实际表、邮箱和推送状态放得下；不能既计预算又重复计同一字段。K4静态需低于链接器实际预算，若接近124580B则先审查布局/报告，不擅自提前实施K5或删功能。

### 8.2 K5静态→boot迁移清单

| 外供块（owner分别登记） | 迁移字节 |
| --- | ---: |
| task.link / task.io / task.spp_worker / task.trace_out | 13824 |
| spp.rx.0 / spp.rx.1 | 8192 |
| trace.ring / log.ring | 8192 |
| gnss.demux（main接收工作区） | 4096 |
| rtcm.pipe | 4096 |
| spp.tx.reply.0 / spp.tx.reply.1 / spp.tx.stream.phone / spp.tx.merge | 9216 |
| gnss.app_ring | 4096 |
| gnss.latest（按固定上限布局） | 6144 |
| **申请payload总和 M** | **57856** |

只迁移数组，大多数TCB、StreamBuffer控制块、arena登记表、lease和小快照保留静态。哨兵字节、alignment和heap块头另列。预计 `S_K5≈119003-57856+Δmeta+ΔK5=61147+Δmeta+ΔK5 B`，`boot_requested=57856 B`；未来8KiB App环为61952B，静态主体不因此增加。不是新的实测DRAM数字。

### 8.3 堆、动态与阶段总账

| 类别 | 改前 | K4后 | K5后 |
| --- | --- | --- | --- |
| 静态DRAM | 108671（历史） | 119003+Δmeta（估算） | 61147+Δmeta+ΔK5（估算） |
| boot_alloc请求 | 0 | 0 | 57856（4KiB App）；61952（未来8KiB） |
| IDF GNSS UART RX/TX | 4096+1024及控制开销 | 保持 | 保持，仍计SDK堆 |
| console UART TX | 8192及控制开销 | 保持 | 保持，不纳入arena |
| BT/BTC发送深拷贝等 | IDF动态 | 有界在途增加，需测 | 同K4，不计作boot；4×1024只是应用待完成负载上界，不是SDK全部开销 |
| 应用运行期分配 | 现主通路无 | 无 | seal后无 |
| SD观测环/栈/驱动 | 0 | 0 | **0**（未来K6环约16384+开销） |

若其他SDK分配不变，K4新增10332B使历史负载最低98.4KB约下降10.09KiB；K5静态释放M又从堆取回M，最终free基本不变，仅多alignment/块头/ΔK5和SDK在途波动。98.4KB原报告单位需实测脚本统一为B，不能用混合单位给出伪精确最终free。**Q7已决定：负载最低内部8bit堆≥81920B，最大连续内部块≥32768B**，作为统一验收门槛替代不符当前配置的ADR-0009历史120KB目标，旧报告保留作历史。最大块为未来16KiB记录环留余量，不是本期分配承诺；启用SD后需重算。

所有阶段必须报告 static + boot + SDK实时堆 + high-water 的同一时刻数据，caps重叠不累加。链接器静态预算、运行期堆预算是两个门禁，降低静态数不能替代最低堆验收。

## 9. 可观测性

新增行/节，不改HIL依赖的 `rtcm:`/`stats:` 原字段顺序。计数按累计饱和或明确回绕策略实现，reset带epoch；detach前归档读者统计防止重连抹去证据。

| 出口 | 字段 |
| --- | --- |
| status / status link | 每会话generation、RX used/hwm/dropped_bytes、reply used/hwm、PHONE stream used/hwm（其他角色无stream）、reserved、paused/累计ms/最大ms、accepted/completed/cancelled/errors、inflight、cong_enter/cong_duration、stray_write、anon_write_fail、push_coalesced |
| gnss status / status gnss | 每环cap/used/hwm、writer records/bytes/overwritten/no_subscriber/oversize_drop/pinned_drop；每读者mask、records/bytes/dropped/filtered/pinned_drop；各消息period/sent/skipped、query_oversize；观测显示reserved/not allocated |
| status rtcm | 保持原两行，另行pipe accepted/drop/discard/UART pending及错误、reset epoch |
| sys mem | §7.3的静态段、boot owner表、各caps堆、seal/降级状态 |
| trace（只追加码） | TX reserve失败/恢复、cong进入/退出、提交失败/短写/迟到/匿名故障、RX drop、环覆盖/租约拒新/oversize、RTCM pipe/UART错误、boot阶段失败/封存 |

C驱动只提交数值给已有trace hook，文本映射在main debug表，旧事件编号不重排。boot失败在trace未初始化时用控制台安全出口。高频统计不能每字节trace刷爆4096B环；使用计数/状态变化和限量事件。trace关闭配置不分配其环/输出任务，统计小快照仍可用。

## 10. 验收、实施门禁与回退

### 10.1 行为与性能

- K1 legacy测试全通过；typed覆盖、mask、两段span、pin拒新、代际失效、u32回绕、并发读写无数据竞争；ASan/UBSan通过。sanitizer不替代SMP锁审查。
- K2在替身中一次发16个需回复请求、强制TX满/CONG，最终回复逐seq齐全，副作用每条仅一次；CLOSE/早到WRITE/匿名错误/短写/双会话公平可重复注入。无线HIL需确认报警心跳与推送时限。
- win16及up256使用窗口内零丢验收，记录请求线路大小、窗口字节、RTCM并发流量、RX高水位。K3新增upwin模式，以 `--window-bytes 3072` 运行256B payload上行，窗口只在收到完整有效回复后归还，至少10000帧且连续10min（不足时重复），`rx_dropped=0`、应答/序号/CRC零缺失。现有 `--mode up` 无窗口洪泛只作负例，观察显式rx_dropped，不纳入零丢结论。
- **Q7已决定**：同机BlueZ、同报文、同无线环境下，下行≥基线25KiB/s的1.5倍，即≥37.5KiB/s，探索2倍（50KiB/s）。报告实际值和p50/p95/p99延迟，未测不能写达标；未达门槛标FAIL并报告。连续流中健康未拥塞时协议应答p95≤100ms，长句/拥塞另报最大延迟；遵守Q4窗口等待与有副作用命令不自动重发。
- K3 RTCM逐字节CRC32和frames/bytes对照一致；pipe满、UART短写/暂不可写、断电/重探测均可观测；无丢失负例不能靠端到端CRC一句话掩盖。
- K4无订阅时App环写入0；ALARM无stream区且GNSS流量0；断链重连无历史。默认4KiB下4096B hash整条拒绝且oversize_drop增加，>256B查询返回02 02且query_oversize增加；边界内记录整条进入PHONE stream，满则cancel重试、落后覆盖计dropped且不断链。双方策略、-128/0/正interval、控制台持久默认/NVS重启/错误通过；0x0D保持32B COM查询/SET无回复，其订阅/保存扩展不在本期范围。
- K5 seal后申请失败、逐owner注入分配失败退控制台、初始化中途失败无UAF；map确认.bss与boot owner账对应；静态预算不过界、最低内部8bit堆≥81920B、最大块≥32768B、各任务余栈≥原验收30%且无持续下降、无TWDT。栈不因迁移再缩小。

### 10.2 软件门禁命令（将来实施执行，本轮不构建）

每新shell按AGENTS加载IDF，主工程只用默认 `build/`，其他构建只放 `builds/<名称>/`。以下 `kN` 由当前任务号替换。已有配置不能被静默覆盖；ON/OFF、两策略及方言配置通过独立临时defaults/既有配置机制验证，不改冻结sdkconfig.defaults。不得触发组件下载。

```sh
. ~/esp/esp-idf/export.sh >/dev/null
idf.py build
idf.py size
idf.py size-components
python3 -I tools/g7b_check/g7b_check.py --build build
sha256sum -c docs/frozen.sha256

# 全部现有host工程；本轮不执行。实现卡还需自己的新增故障用例。
for suite in host app_fw stream_demux bt_reconnect services cli gnss; do
    cmake -S "test/$suite" -B "builds/kN-$suite" &&
    cmake --build "builds/kN-$suite" &&
    ctest --test-dir "builds/kN-$suite" --output-on-failure || exit 1
done
for suite in main_host_check t13_host_check; do
    cmake -S "tools/$suite" -B "builds/kN-$suite" &&
    cmake --build "builds/kN-$suite" &&
    ctest --test-dir "builds/kN-$suite" --output-on-failure || exit 1
done
# 九套工程的ASan/UBSan构建：
for source in test/host test/app_fw test/stream_demux test/bt_reconnect test/services test/cli test/gnss tools/main_host_check tools/t13_host_check; do
    suite=${source##*/}
    cmake -S "$source" -B "builds/kN-asan-$suite" \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" &&
    cmake --build "builds/kN-asan-$suite" &&
    ctest --test-dir "builds/kN-asan-$suite" --output-on-failure || exit 1
done

# 改动的cpp/hpp逐文件替换参数；C文件按.clang-tidy-c与既有G5b脚本检查。
clang-format --style=file --dry-run --Werror <改动的.cpp/.hpp>
# rtk_pure的两架构构建分别在 builds/rtk_pure-esp32 与 builds/rtk_pure-esp32c3，
# 用 idf.py -C test_apps/rtk_pure -B <对应绝对构建目录> set-target <目标>，再build。
python3 -I tools/tidy_check/tidy_check.py --host-build builds/host --idf-build build --test-app-build builds/rtk_pure-esp32
```

G5b的 `builds/host` 需按AGENTS配置生成compile_commands并构建，再运行脚本，不能用不存在的编译数据库。所有自有目标调用strict warnings；0 warning，不增加宽泛豁免。测试工程如需要接入K1新API，可改test/app_fw等非冻结工程；test/host原文件一个字节不改。权威astyle由用户/验收方运行，本轮及实施方不安装下载。

HIL仅写计划，以下为工具命令清单，本轮不执行蓝牙负载/串口/flash/monitor/SPI。现有命令可运行all基线（含up负例）；upwin命令须K3实现新增模式后才可执行，不把当前脚本误写为已支持：

```sh
# 现有命令；all包含无窗口up洪泛，按各子阶段判读，不要求TOTAL lost=0。
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode all
# K3新增后的窗口上行验收命令，当前NOT_RUN。
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode upwin --window-bytes 3072 --flood 10000 --duration 600
```

实际设备/蓝牙负载测试由验收方在对应授权下运行；本次不新增工具代码。回退为任务级恢复先前布局的独立补丁，不在运行中把SEALED arena拆回静态数组；本轮不产生回退代码。

## 11. 冻结修订清单与任务拆分

以下是**未来冻结契约定稿清单，不是本轮文档修订授予的代码修改权限**。§12的设计决定已接受；实施卡单独下达后，架构方先定稿指定契约并仅更新相应SHA条目/新增已批准条目；实现方不能全表重算掩盖其他改动。

| 卡 | 重点 | 冻结变更提案 |
| --- | --- | --- |
| [K1](../tasks/K1-fanout-ring.md) | typed FanoutRing、租约、host回归 | `components/app_fw/include/fw/FanoutRing.hpp`；ADR-0007 §3.6-1 |
| [K2](../tasks/K2-spp-nonblocking-tx.md) | 多在途TX、reply credit、删async_tx | `components/spp_server/include/SppServer.h`、`components/spp_server/include/spp/SppTransport.hpp`、`components/link_core/include/core/Transport.hpp`、`components/link_core/include/core/LinkService.hpp`；若扩展纯逻辑trace编号则 `components/link_core/include/core/TraceCodes.hpp`；ADR-0007/0009相关契约 |
| [K3](../tasks/K3-rtcm-rx-path.md) | RTCM直通、RX通知/512B、公平 | 在K2已批准的 `SppServer.h` 中定稿wake/RX统计契约；ADR-0007/0010通路描述 |
| [K4](../tasks/K4-gnss-app-ring.md) | App环、订阅/配置/NVS、取代G-C相关部分 | `components/stream_demux/include/demux/decoders/UnicoreHash.hpp`；新增 `components/link_core/include/core/services/GnssService.hpp`；如服务注册确需更改 `components/link_core/include/core/CommandRouter.hpp` 必须再列明；ADR-0010 §2.9、G-C、phone_protocol；需要方言公共数据表时先报告具体文件 |
| [K5](../tasks/K5-boot-memory.md) | boot_alloc、sys mem、s_app .bss | 新增rtos RAII头按AGENTS例外；C storage契约修订 `SppServer.h`；ADR-0009内存指标/命令与CODING-STYLE启动堆说明如有冲突由架构方修订 |
| K6（未来） | SPI SD与静态观测记录 | 无本期修改/分配/硬件动作；待观测命令和板级信息后另立卡 |

所有卡报告 `docs/reports/K<N>.md`，逐项PASS/FAIL/NOT_RUN/BLOCKED、命令/日志、内存实测、契约偏离和未决问题。禁止commit/push、串口/硬件操作、安装下载，授权未覆盖的冻结改动先停该项并继续独立工作。

## 12. 已决定（用户审查确认）

以下决定来自[修订指令](../tasks/ADR-0011-revise.md)，已同步正文、内存账、任务卡和验收；本轮只改文档，实施另行下达。

| 项 | 已决定 | 理由 / 验收影响 |
| --- | --- | --- |
| Q1 App环与长hash | Kconfig默认4096B，超容量写者整条丢并计oversize_drop，不截断；实际需要完整SATSINFOA时另改8KiB。 | 控制本期RAM；4KiB装不下4096B+3B头。K4验收显式oversize计数，无长句容量阻塞。 |
| Q2 最新句查询 | >256B返回02 02并计query_oversize，不截断、不分片。 | 维持现有payload上限与线上协议。 |
| Q3 0x0D/持久配置 | 0x0D保持32B COM查询/SET无回复；本期外部统一配置入口只接串口控制台，持久默认由控制台设置；内部App订阅沿用G-C逐消息interval。 | 不扩展未定义的线上字段；0x0D订阅/保存及web不在本期范围，不阻塞K4。 |
| Q4 回复反压/App | 回复满暂停该会话，已执行请求回复保留；App按约3KiB内字节窗口等待，超时不自动重发副作用命令。 | 有限RX容量需要上游节流，避免丢回复与重复执行；up256用窗口模式零丢验收。 |
| Q5 简化B/整条接纳 | 每会话reply2048B；仅PHONE另有stream4096B。peek后整条复制，空间不足不commit、cancel后下轮重试；被覆盖计dropped。租约只覆盖≤4096B复制，不跨等待。 | 一次接纳完整句，省去跨轮租约与发送进度追踪；TX持有后不受App环覆盖影响，报警器不占stream RAM。 |
| Q6 简化A/匿名WRITE | 全局≤4且每会话≤4<SDK队列10，断言/注释引用btc_spp.c:37。匿名失败计anon_write_fail+trace，关闭当前有在途分块的会话并由CLOSE清理；迟到事件计stray_write。 | 正常NO_RESOURCE不可达，NO_CONNECTION按关闭收敛；不设全局故障/仅控制台降级/回调终结屏障前置，K2用host替身注入验证。 |
| Q7 门槛 | 负载最低内部8bit堆≥81920B、最大块≥32768B；下行≥37.5KiB/s（基线1.5倍），探索2倍。 | 为当前通路与未来观测预留余量；数值已接受，是否达标须实施后实测。 |
| Q8 静态观测 | 维持未来K6占位，本期不分配、不驱动SPI。 | 观测命令/报文集合/板级信息仍UNKNOWN，留到K6取得事实后实施。 |

本轮修订完成即停下等待审查，不启动K1。
