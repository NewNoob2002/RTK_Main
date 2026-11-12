# G-C 手机订阅与原始报文透传（ADR-0010 §2.9）

执行方：pi agent（Claude 指定模型）。分支 `feat/gnss`，前置：G-A、G-B 已由 Claude 验收提交。不要切分支、不要 commit。

先完整阅读 `AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0010-gnss.md`（尤其 §2.7、§2.9）、`docs/adr/ADR-0004*`、`docs/adr/ADR-0007*`、
`docs/protocol/phone_protocol.md`，以及协议原文摘录 `docs/protocol/survey_protocol_0530.md`（Claude 已从 PDF 摘出 0x01/0x02/0x04/0x05/0x1F/0x21/0x22/0x23 与帧头）。
再读 `components/link_core/{include/core,services}/**`、`main/modules/{gnss,bt_link}/**`、`components/gnss/src/dialects/unicore*.cpp`、
`components/stream_demux/include/demux/decoders/unicore_hash.hpp`。

## 要做

1. **映射表** `main/def/gnss_sub_def.hpp`（新建，本任务内可改，验收后冻结）：`struct SubMsg { uint16_t msg_id; std::string_view prefix; uint16_t default_period_ms; uint32_t need_caps; }`，
   表项按 §2.9：0x04 GGA、0x05 `#SATSINFOA`、0x1F `#HEADINGA`（需 heading 能力位）、0x21 GST、0x22 RMC、0x23 GSA。NMEA 匹配忽略 talker（GP/GN/...），按第 4–6 字符的语句类型；Unicore 匹配 `#` 后的报文名。
   `period_from_interval(int8_t v)`：v<0 → |v|*1000 ms；v>0 → 1000/v ms（下限 50 ms）；0 → 关闭。
2. **模块输出超集**：Unicore 流动站脚本追加 `GPGST COM<port> <1/rate>` 与 `SATSINFOA COM<port> 1`（按 `components/gnss/src/dialects` 现有写法，更新 test/gnss 字节级期望）。u-blox 方言：GST 用 `CFG-MSGOUT-NMEA_ID_GST_UARTx`，无 SATSINFOA（0x05 回不支持）。
3. **订阅表**（kLink 写、kIo 读）：`std::atomic<uint32_t>` 每表项一个周期（0=关）+ kIo 私有 `last_sent_ms`。PHONE 会话断开（bt_link 已有事件）清零全部。
4. **kIo 侧**：gnss 模块帧回调对 NMEA / UnicoreHash 帧匹配映射表 → 记“最近一句”（每表项一个定长缓冲，NMEA 256 B，SATSINFOA 4096 B——放在 gnss 模块静态区，说明 RAM）→ 若订阅到期则整句写入 `NmeaOut` 管道
   （`rtos::StaticStreamBuffer<4096>`，2 B 小端长度 + 原文；空间不足整句丢弃并计数 + 追踪 `kOutDrop`（在 gnss_def.hpp 追加码前先在报告提出））。
5. **kLink 侧**：`bt_link` 每轮从 `NmeaOut` 取完整句，经现有 async_tx 发给 PHONE 会话（无 PHONE 时丢弃并计数）；不得跨句截断，发送队列满时留到下一轮。
6. **路由**：新增 `GnssService`（`components/link_core/services/gnss_service.cpp` + 头，ADR-0007 服务写法，替换 DeviceService 的 0x04/0x05 占位路由——DeviceService 冻结头若需改须先在报告提出）：
   Type=0 且 interval≠0 → 写订阅、回结果码成功；interval=0 → 回封装帧（载荷 = 最近一句原文，无数据回 0x02 0x02）；不支持的表项（缺能力位）→ 0x02 0x02。
   “最近一句”跨任务读取方案自定（Slot 拷贝或 seqlock），写进报告。
7. **0x02**：GNSS 功能字节 = `kRunning ? 0x81 : 0x01`（bit7 工作，低 7 位数量 1），其余字节不变。
8. **demux**：`unicore_hash::kMaxFrame` 1024 → 4096（该头已列入冻结表，Claude 授权本任务修改此一项并更新 `docs/frozen.sha256` 对应行），补 test/stream_demux 长句用例。
9. **控制台**：`gnss status` 增加订阅表（ID、周期、发送数、丢弃数）与 NmeaOut 统计。

## 门槛（`docs/reports/G-C.md`）

1. IDF 构建 0 告警（`builds/gc`），`RTK_GNSS_ENABLE=n`（`builds/gc-off`）0 告警；DRAM/镜像增量逐项解释。
2. 全部 host 测试普通 + ASan 通过且用例数只增不减；新增：映射表匹配（各 talker）、interval 换算、降采样（模拟时钟）、会话断开清空、0x04 查询应答字节级、管道满整句丢弃。
3. g7b PASS、clang-format、`sha256sum -c docs/frozen.sha256`。
4. 给出 HIL 对照用例：手机工具发 0x04 interval=1 → 1 Hz 收到原始 `$GNGGA`；0x05 interval=-5 → 5 s 一条 `#SATSINFOA`；interval=0 → 停止；断开重连后无输出。

报告末行 `G-C DONE` 或 `G-C BLOCKED: <原因>`。

## 禁止

修改冻结文件（第 8 条授权项除外）；改协议帧格式、NVS 命名空间与键、蓝牙名；烧录、打开串口、联网、git commit/切分支。构建目录只用 `builds/gc*`。
