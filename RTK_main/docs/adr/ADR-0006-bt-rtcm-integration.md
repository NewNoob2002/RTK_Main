# ADR-0006 蓝牙单流承载产品帧与 RTCM（固件集成 + 上板验证）

- 状态：**Approved 2026-11-11**。用户要求“让产品帧和 RTCM 走同一条蓝牙流，然后进行上板测试”。
- 落实 ADR-0004 §7（方案 B：按同步字节分流），使用 ADR-0005 的 C++ `stream_demux`。评审深度：Delta。

## 1. 决策

1. **分流放在 `main/app_link.cpp` 的 `drain_rx`，不改 `rtk_core`。** 每个 `SessionEntry` 持有一个
   `demux::StaticDemux<rtk::proto::demux_frame::kDecoder, demux::rtcm3::kDecoder>`（产品帧优先，缓冲 1029 B）与一个
   `final : demux::Sink` 会话接收器。产品帧的完整线路帧原样交给 `LinkService::on_data(handle, wire, now)`：
   冻结的 `FrameParser` 对一条已校验的完整帧再解析一次（≤280 B，开销可忽略），`rtk_core` 与 `test/host` 均不改。
2. **调度保持 ADR-0002/0003 的“每轮至多一个产品帧”**：仍逐字节 `feed`，产品帧回调后本轮立即让出。
   RTCM 帧不计入该限制（转发廉价、不产生同步写），只受每轮 256 B / 20 ms 预算约束。
3. **只接受 PHONE 角色会话的 RTCM**（`LinkManager::role(handle) == Role::kPhone`）；其他角色的 RTCM 帧丢弃并计数 `rtcm_rejected_role`。
4. **RTCM 出口 `RtcmForwarder`（`main/app_rtcm.{hpp,cpp}`）**：本阶段没有 GNSS UART 驱动，出口只做统计：
   帧数、字节数、按到达顺序对全部 RTCM 线路帧累计的 `rtk::proto::crc32`（供上板测试与发送端比对内容一致性）、
   最近一条消息号（载荷前 12 bit）、角色拒绝数。接入 UART 管道（StreamBuffer，满则整帧丢弃并计数）留给 GNSS 驱动 ADR。
5. **未知字节只计数**（汇总 `unknown_bytes`），不再透传。会话 OPEN 时 `init` 该会话的分流器，CLOSE 时 `abort` 后释放。
6. **指标语义变化**：坏 CRC/坏长度的产品帧现在在分流器中被拒绝（计入 `demux_product_rejects`），不再到达
   `FrameParser`，因此 `parser_crc_errors/parser_length_errors` 在正常运行中保持 0。`status` 同时输出两组计数。
7. **RX StreamBuffer 可配置**：`rtk_bt_spp.h` 的 `RTK_BT_SPP_RX_BUF_SIZE` 改由 `CONFIG_RTK_BT_SPP_RX_BUF_SIZE`
   （`components/rtk_bt/Kconfig`，1024–16384，默认 4096）决定；非 IDF 构建回退 2048。2 会话共 8 KB 静态 RAM。

## 2. 控制台

- `status` 增加一行：
  `rtcm: frames=<u32> bytes=<u32> crc32=0x<08X> last_msg=<u16> rejected_role=<u32> unknown_bytes=<u32> product_rejects=<u32> rtcm_rejects=<u32>`
  （格式固定，上板脚本按此正则解析）。
- 新命令 `rtcm reset`：经命令队列由链路任务清零上述统计与 crc32（链路任务是唯一所有者）。

## 3. 分工（BATCH-I）

| 任务 | 内容 | 执行 |
| --- | --- | --- |
| I1 固件集成 | `main/app_link.cpp`、`app_link.hpp`、`app_console.cpp`、新 `main/app_rtcm.{hpp,cpp}`、`main/CMakeLists.txt`；`tools/main_host_check` 增加混合流用例 | pi + `codex/gpt-6.1-sol` |
| I2 上板测试工具 | `tools/hil/spp_rtcm_mix.py`：主机蓝牙 PHONE 客户端，RTCM 流 + 产品请求并发，串口读取板端计数并比对 | pi + `antigravity/gemini-3.8-flash` |
| I3 验收与上板 | 审查、门禁、烧录 `/dev/ttyACM0`、运行 I2 工具、报告 | 架构方（Claude） |

## 4. 门禁

1. `idf.py build` 0 警告；`tools/main_host_check` 两种策略全部通过（含新增混合流用例）；`test/host` 8/8；`test/stream_demux` 4/4；冻结校验通过。
2. 上板：RTCM-only、RTCM+产品请求混合两种负载下，板端 `rtcm.frames/bytes/crc32` 与发送端完全一致（未发生 `rx_dropped` 时）；
   产品请求 0 丢失、0 内容错误；记录 RTCM 吞吐上限、溢出点与产品 RTT 变化。
