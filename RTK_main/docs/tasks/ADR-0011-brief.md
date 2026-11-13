# ADR-0011 起草任务书（数据通路与内存整合）

你负责**起草** `docs/adr/ADR-0011-data-path-memory.md` 及其实施任务卡 `docs/tasks/K1-*.md … K5-*.md`。
本任务**只写文档**：不改任何 .c/.cpp/.h/.hpp/CMake/Kconfig，不改 `docs/frozen.sha256`，不 commit，不连串口。
写完后在 `docs/reports/ADR-0011-draft.md` 写一页摘要（决策列表、待用户确认问题、风险），然后停下等审查。

先读：`AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`、`docs/adr/ADR-0009-trace-console.md`、`docs/adr/ADR-0010-gnss.md`（格式与术语以它们为准，中文写作）、
`components/app_fw/include/fw/fanout_ring.hpp`、`components/app_fw/src/fanout_ring.cpp`、
`components/spp_server/spp_server_tx.c`、`spp_server_rx.c`、`private_include/spp_server_priv.h`、`include/spp_server.h`、
`main/modules/bt_link/{async_tx.hpp,async_tx.cpp,bt_link_module.hpp,bt_link_module.cpp}`、`main/modules/rtcm/*`、
`main/modules/gnss/{gnss_module.hpp,gnss_module.cpp,rtcm_pipe.hpp}`、`main/app_context.hpp`、`main/data/app_data.hpp`、
`components/rtos/include/rtos/*.hpp`、`docs/tasks/G-C-phone-subscription.md`、`docs/protocol/phone_protocol.md`。
参考（只读）：`../../SparkFun_RTK_Everywhere_Firmware/Firmware/RTK_Everywhere/Tasks.ino`（processUart1Message / handleGnssDataTask）。

## 已确定的事实（板上实测，ESP32 经典款无 PSRAM，BR/EDR SPP）

- 方案 A 已提交 9a7d7c5：link 栈 4096、spp_worker 2560、async_tx 2048；静态 DRAM 108,671/124,580；堆空闲 ~111KB，负载最低 98.4KB，最大块 100KB。
  负载下栈剩余：link 2288、io 1784、async_tx 1300、spp_worker 1672、trace_out 1324、console 2712。
- spp_stress（主机 BlueZ）：rtt..win8 0 丢，~300 rep/s，下行 ~25 KiB/s；**win16 丢 8 个回复**（= 超出 AsyncTxTransport::kDepth 8，入队失败丢弃）；
  **up256 丢 8–37 帧**（每会话 4KB RX StreamBuffer，`spp_server_rx.c:36` xStreamBufferSend timeout 0 满即丢；SPP 回调模式无法反压对端；链路任务每次只取 64B）。
- 发送现状：`spp_server_write_chunk` 每会话同时只有 1 个在途分块（≤512B），必须等 ESP_SPP_WRITE_EVT 才发下一块 → 停等，吞吐受限；
  因 write 阻塞才加了 async_tx 任务（StaticQueue<Item,8> 每项 292B + 栈）。代码注释已确认 esp_spp_write 返回时已深拷贝。
- RTCM 上行路径拷贝 5 次：spp RX 4KB → 64B chunk → 会话 Demux buf → SessionSink bytes_ → rtcm_batch_ 2314B → DataCenter rtcm_batch → RtcmPipe 4KB → UART TX。
- `fw::FanoutRing` 已存在（SparkFun 风格一写多读、覆盖最旧、每读者尾指针与 dropped、attach 从最新开始），冻结（ADR-0007 §3.6-1），main 尚未使用。
- SparkFun：单环 4KB 混存所有报文，无类型头，按报文边界丢弃；消费者 BT/TCP client/TCP server(每客户端)/SD/UDP/USB；
  消费者离线时 tail=dataHead；类型过滤在 GNSS 配置与写者端；RTCM 独立缓冲；SD 直接从环内存写（零拷贝）。

## 用户已做的决定（必须体现在 ADR）

1. GNSS 输出按**消费者/报文类别分环**，不用一个大环：
   - **App 流环**：只放 NMEA 与 hash（Unicore 文本）；读者 = 每个**手机 App** 会话一个（外部报警器不登记读者，不收任何 GNSS 数据）；
     记录带 1 字节类型，读者按订阅掩码过滤；2–4KB 起步，按实测丢失计数调整。
   - **静态观测环**：只放二进制观测报文；唯一读者 = SD 卡写入任务。SD 卡走 SPI，**本期只预留**（接口、Kconfig 占位、容量估算），不实现、不分配。
     记录开始时从堆分配、停止时释放；按 10Hz 预留（估算 OBSVMCMP ~2.2KB/历元 → ~22KB/s，需 GNSS 串口 ≥460800；环 ≈ 速率 × 300ms × 2 ≈ 16KB）；
     报文集合做成可配置表（E1-5.4 旧固件未给出观测命令，待用户补）。蓝牙 App 不需要原始观测。
   - 输出频率可由蓝牙 App、串口控制台、（未来）webserver 设置 → 设计统一的配置入口（与 G-C 0x0D 订阅/NVS 持久化衔接）。
   - 写者端过滤：某类报文无在线读者或无人订阅则不写入。读者离线即 detach，重连 attach 从最新开始。
2. 差分 RTCM（手机→GNSS）保留独立 SPSC 管道，不进 GNSS 输出环；去掉 rtcm_batch/DataCenter 中转，分流器得到 RTCM 帧后直接 push RtcmPipe，RtcmModule 只做统计。
3. 蓝牙发送重构：每会话发送字节环 + 非阻塞写入；spp worker 连续提交 esp_spp_write 直到 cong=true，等 ESP_SPP_CONG_EVT 恢复；多帧合并成一次写；
   **去掉 async_tx 任务与队列**；两级优先：协议回复（可靠，满则反压链路任务暂停该会话请求处理，不丢）> GNSS 订阅流（从 App 流环读，读不及计 dropped）。
4. 接收：协议层窗口（App 本身异步发送，按测试调整缓冲）；设备端 DATA_IND 立即唤醒链路任务、单次读取 256–512B；在 status 中报告 RX 丢弃。
5. 方案 B：`rtos::boot_alloc`（一次性内部 8bit 堆分配，init 结束后封存，登记 owner/bytes），迁移任务栈、spp rx、Trace/Log ring、gnss rx、RtcmPipe 等大块；
   失败时降级为仅控制台；新增 `sys mem` 命令（静态段 / boot 分配表 / 各 caps 堆）；s_app 移到 .bss（现 52KB 在 .data 因成员初始化 `{*this}`）。
   B 在蓝牙重构之后做（布局先定）。
6. FanoutRing 冻结接口扩展（需写清修订理由、兼容性、frozen.sha256 更新）：记录 1 字节类型 + 读者订阅掩码；只读不拷贝的 peek/commit（回绕时两段 span），供 SD 与 BT 直接从环内存写。

## ADR 必须包含

- 背景与实测数据；数据流全图（ASCII）：各缓冲名、大小、写者/读者任务、丢弃策略、拷贝次数（改前/改后对比）。
- 内存账：改前/改后 静态 DRAM、boot 堆、动态，逐项。
- spp_server 新 TX 状态机（会话发送环、in-flight、cong、关闭时序/竞态：CLOSE 与在途 write、迟到 WRITE_EVT），线程/ISR/BTC 上下文契约。
- 协议回复反压的具体规则（链路任务何时停止 dispatch，何时恢复），以及与 PHONE_PREEMPT/CONCURRENT 两种会话策略的关系。
- FanoutRing 修订后的接口草案（只给头文件签名与契约注释，不写实现）。
- boot_alloc 接口草案与封存语义。
- 可观测性：每环/每读者 dropped、TX 环高水位、cong 次数，进入 `status`/`sys mem`/trace codes。
- 验收指标：spp_stress win16 与 up256 0 丢（或给出窗口约束下的 0 丢条件）；下行吞吐目标（相对 25KiB/s 提升，写实测后定）；DRAM/堆数字；所有现有门禁通过。
- 任务拆分 K1..K5（每卡：范围、允许修改文件、冻结文件变更、测试、门禁命令、验收），建议顺序：
  K1 FanoutRing 扩展 + host 测试；K2 spp_server 非阻塞 TX + 去 async_tx；K3 RTCM 直通 + 链路 RX 唤醒/读块；K4 GNSS 输出环 + App 订阅（并入/替代 G-C 中相应部分）；K5 boot_alloc + sys mem + s_app .bss。SD 卡记录列为未来 K6 占位。
- 待用户确认问题列表（例如 App 流环大小、回复反压时 App 侧表现、观测报文集合）。

门禁命令参见 AGENTS.md（idf build 0 warning、各 host ctest、g7b、frozen sha）。文档写完后无需构建。
