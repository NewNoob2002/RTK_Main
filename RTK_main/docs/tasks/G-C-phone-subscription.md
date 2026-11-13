# G-C 手机订阅与原始报文透传（ADR-0010 §2.9）

执行方：pi agent（Claude 指定模型）。分支 `feat/gnss`，前置：G-A、G-B 已由 Claude 验收提交。不要切分支、不要 commit。

先完整阅读 `AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0010-gnss.md`（尤其 §2.7、§2.9）、`docs/adr/ADR-0004*`、`docs/adr/ADR-0007*`、
`docs/protocol/phone_protocol.md`，以及协议原文摘录 `docs/protocol/survey_protocol_0530.md`（Claude 已从 PDF 摘出 0x01/0x02/0x04/0x05/0x1F/0x21/0x22/0x23 与帧头）。
再读 `components/link_core/{include/core,services}/**`、`main/modules/{gnss,bt_link}/**`、`components/gnss/src/dialects/unicore*.cpp`、
`components/stream_demux/include/demux/decoders/unicore_hash.hpp`。

## 要做

> 架构修订注（ADR-0011 / K4）：跨任务数据路径改用 `fw::FanoutRing`（typed 模式，4096 B 独立 App 环，`RTK_GNSS_APP_RING_BYTES`）与 PHONE 4096 B `TX.stream` + `GnssLatestCache`（6144 B），替代原 §3/§4 中的 `NmeaOut` `StaticStreamBuffer` 方案；并由 `gnss/out` NVS blob 统一管理控制台默认输出配置。协议层行为（6 条报文 ID、interval 换算、>256 B 查询返回 `02 02`、断连清零无回放、`0x0D` 保持不变）完全保留。

1. **映射表** `main/def/gnss_sub_def.hpp` + `main/data/gnss_output.{hpp,cpp}`：
   表项按 §2.9：0x04 GGA、0x05 `#SATSINFOA`、0x1F `#HEADINGA`（需 heading 能力位）、0x21 GST、0x22 RMC、0x23 GSA。NMEA 匹配忽略 talker（GP/GN/...），按第 4–6 字符的语句类型；Unicore 匹配 `#` 后的报文名。
   `GnssService::interval_to_period_ms(int8_t v)`：v<0 → `|int16(v)| * 1000 ms`（`-128` 为 `128000 ms`）；v>0 → `max(50, 1000/v) ms`；0 → 关闭。
2. **模块输出超集**：Unicore 流动站脚本追加 `GPGST COM<port> <1/rate>`、`SATSINFOA COM<port> 1` 及 `HEADINGA COM<port> 1`（仅 `cap::kHeading` 型号）；u-blox 方言追加 `CFG-MSGOUT-NMEA_ID_GST_UART1`（`0x209100D4`），无 SATSINFOA（0x05 无缓存回 `02 02`）。
3. **订阅表**（kLink 写、kIo 读）：`GnssAppSubState`（`portMUX_TYPE` 保护）保存 `mask`、每类型有效周期与发送/跳过统计；PHONE 会话断开（bt_link 已有事件）清零全部（重连后不自动吐流、不回放）。
4. **kIo 侧**：gnss 模块帧回调对 NMEA / UnicoreHash 帧匹配映射表 → 更新 `GnssLatestCache`（4×256 B NMEA + 1024 B HEADINGA + 4096 B SATSINFOA = 6144 B）→ 若订阅命中且降采样到期，整句写入 4096 B `gnss_app_ring`（`FanoutRing` typed 模式，单条上限 4093 B；≥4094 B 写侧丢弃计数 `oversize_drop`）。
5. **kLink 侧**：`bt_link` 每轮从 `gnss_app_ring` `peek` 完整句，经 `spp_server_stream_reserve` / `spp_server_tx_commit` 直接写入 PHONE 4096 B `TX.stream`（满时 `cancel` 留待下一轮）；无 PHONE 订阅时写侧不入环并计 `no_subscriber`。
6. **路由**：新增 `GnssService`（`components/link_core/include/core/services/gnss_service.hpp` + `services/gnss_service.cpp`，替换 DeviceService 的 0x04/0x05 占位路由）：
   Type=0 且 interval≠0 → 写订阅、回 `01 01`；interval=0 → 停止订阅并经 ≤256 B 有界查询拷贝回封装帧（无数据、无能力位或原文 >256 B 回 `02 02`）。
7. **0x02**：GNSS 功能字节约定 `kRunning ? 0x81 : 0x01`（若冻结头 `reply.hpp`/`device_service.hpp` 未修订则在 K4 报告列出所需字段并申请批准）。
8. **demux**：`unicore_hash::kMaxFrame` 1024 → 4096（已授权修改并更新 `docs/frozen.sha256` 对应行），补 test/stream_demux 4096/4097 用例。
9. **控制台**：`gnss status` / `status gnss` 增加 App 环、reader、订阅、最新句超限统计与 `obs` 预留占位行；`gnss out` 支持查看与设置持久化的 `gnss/out`。

## 门槛（`docs/reports/G-C.md`）

1. IDF 构建 0 告警（`builds/gc`），`RTK_GNSS_ENABLE=n`（`builds/gc-off`）0 告警；DRAM/镜像增量逐项解释。
2. 全部 host 测试普通 + ASan 通过且用例数只增不减；新增：映射表匹配（各 talker）、interval 换算、降采样（模拟时钟）、会话断开清空、0x04 查询应答字节级、管道满整句丢弃。
3. g7b PASS、clang-format、`sha256sum -c docs/frozen.sha256`。
4. 给出 HIL 对照用例：手机工具发 0x04 interval=1 → 1 Hz 收到原始 `$GNGGA`；0x05 interval=-5 → 5 s 一条 `#SATSINFOA`；interval=0 → 停止；断开重连后无输出。

报告末行 `G-C DONE` 或 `G-C BLOCKED: <原因>`。

## 禁止

修改冻结文件（第 8 条授权项除外）；改协议帧格式、NVS 命名空间与键、蓝牙名；烧录、打开串口、联网、git commit/切分支。构建目录只用 `builds/gc*`。
