# ADR-0011 修订指令（用户已于审查后确认）

只改文档：docs/adr/ADR-0011-data-path-memory.md、docs/tasks/K1..K5、docs/reports/ADR-0011-draft.md。不改代码、不 commit、不连串口。
把 §12 的 Q1–Q8 改为“已决定”表（附决定与理由），并同步正文、内存账、任务卡、验收。状态改为 **ACCEPTED（待实施）**。

## 用户决定

- Q1 App 流环 **4 KiB** 起步；超过容量的记录（如 4096B SATSINFOA）写者端整条丢弃并计 `oversize_drop`，不截断。若 App 实际需要 SATSINFOA 再单独改 8 KiB（列为可配置 Kconfig，默认 4096）。K4 完整长句验收改为“oversize 显式计数”，不再 BLOCKED。
- Q2 单次查询最新句 >256B → 返回 02 02 + `query_oversize` 计数，不截断、不分片。
- Q3 0x0D **保持现状**（32B COM 查询 / SET 无回复），不扩展线上协议。统一输出配置入口本期只接 **串口控制台**（以及内部 App 订阅按现有 G-C 已定义的逐消息 interval 语义）；NVS 持久默认由控制台命令设置。0x0D 订阅/保存扩展列为未来项，K4 中这部分去掉 BLOCKED，改为“不在本期范围”。
- Q4 接受：回复缓冲满时暂停该会话的请求处理，已执行请求的回复不丢；App 按字节窗口（≈3 KiB 内）限制在途请求，超时不自动重发有副作用命令。
- Q5/简化 B：**GNSS 流发送缓冲只给 PHONE 会话一份 4096B**（报警器不收 GNSS，不分配 stream 区）。从 App 环 peek 后**整条记录**一次复制进 TX.stream（空间不足则不 commit，下一轮再试；落后被覆盖计 dropped），**删除分块租约/记录 token/偏移/`stream_partial_abort`/断链处理**。租约只覆盖一次 ≤4096B 的有界复制，不跨任何等待。会话 TX 改为：每会话 reply 2048B；PHONE 另有 stream 4096B。重算内存账（原 2×1024 stream → 1×4096）。
- Q6/简化 A：匿名 WRITE 失败（handle=0,len=-1）**取消全局 TX_FAULT / 仅控制台降级 / 回调终结屏障要求**。依据本地 SDK `components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c`：该事件只在 slot 未找到（NO_CONNECTION，随后必有 CLOSE）或每 slot `fixed_queue` 满（NO_RESOURCE，`SLOT_TX_QUEUE_SIZE 10`，btc_spp.c:37）时发出。设计约束：全局在途 ≤4 且每会话在途 ≤4 < 10，故 NO_RESOURCE 不可达（加 static_assert/注释引用 SDK 常量）。若仍收到匿名失败：计 `anon_write_fail` + trace，断开当前有在途分块的会话（由 CLOSE 正常清理），不进入全局故障；generation 仅用于丢弃 CLOSE 之后的迟到事件计 `stray_write`。K2 中删除“SDK 屏障证明否则 BLOCKED”的要求，改为 host 替身注入匿名失败用例。
- Q7 门槛：负载下最低内部 8bit 堆 ≥ 81920B、最大块 ≥ 32768B；下行吞吐 ≥ 1.5× 基线（≥37.5 KiB/s），探索 2×。
- Q8 静态观测：仅预留，维持 K6 占位。
- 压测工具：`tools/hil/spp_stress.py` 已加入仓库（模式 rtt/win/up/bad/soak；`up` 为无窗口洪泛）。K3 卡加入：为其新增带字节窗口的上行模式（如 `--mode upwin --window-bytes 3072`），验收 up256 用窗口模式 0 丢；无窗口洪泛作为负例只观察显式 rx_dropped。任务卡里引用真实命令行：`python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode all`。

完成后更新摘要报告，停下等待审查。
