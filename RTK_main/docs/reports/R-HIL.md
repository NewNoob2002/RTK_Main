# R 阶段 HIL 冒烟记录（ADR-0008 §5）

环境：RTK 板 /dev/ttyACM0（F8:B3:B7:25:D8:B2）；报警器测试固件 /dev/ttyUSB0（08:D1:F9:DD:3E:E6，NVS 目标自动重连）；
主机蓝牙作手机角色，`tools/hil/spp_rtcm_mix.py --serial none --seed 7`，每轮 10 s，板端计数经 monitor 面板读取。
基线对照：docs/reports/J5.md §5（双会话约 30 KB/s 0 丢弃，产品 RTT p50 约 190 ms）。

## 2026-11-12 — 提交 719580f（R0 + R5 + R1）

- 上电正常，报警器自动重连（OPEN 后角色 ALARM），`status` 各项正常。
- 双会话（报警器在线 + 主机手机）：

| 负载 | RTCM 帧 主机/板 | 字节/CRC | rx_dropped 增量 | 产品 ok/sent | RTT p50/p99 ms |
|---|---|---|---|---|---|
| 5 KB/s + 产品 | 286/286 | 一致 | 0 | 200/200 | 24/59 |
| 20 KB/s + 产品 | 1107/1107 | 一致 | 0 | 201/201 | 29/77 |
| 30 KB/s + 产品 | 1633/1633 | 一致 | 0 | 200/200 | 181/265 |
| 30 KB/s 仅 RTCM | 1659/1659 | 一致 | 0 | — | — |

- 板端：write_errors 0、disconnects 0、tx queue_full 0 max_depth 2、exec overruns 0 max_round 2 ms、heap min 137,436、link 栈余量 4,692。
- 结论：PASS，与 J5 基线一致。

## 2026-11-12 — R2（模块类化 + AppContext，未提交树 = 本次 R2 提交）

- 报警器板未接入（/dev/ttyUSB0 不存在），仅单会话（主机作手机）。烧录 `builds/r2`，上电 `status` 正常，whitelist 1 条从 NVS 读回。

| 场景 | RTCM 帧 主机/板 | 字节/CRC | rx_dropped 增量 | 产品 | p50/p99 ms |
| --- | --- | --- | --- | --- | --- |
| 5 KB/s + 产品 | 286/286 | 一致 | 0 | 200/200 | 24/64 |
| 20 KB/s + 产品 | 1107/1107 | 一致 | 0 | 201/201 | 19/61 |
| 40 KB/s + 产品 | 2297/2297 | 一致 | 0 | 200/200 | 35/93 |
| 40 KB/s 仅 RTCM | 2276/2276 | 一致 | 0 | — | — |

- 板端：write_errors 0、tx queue_full 0 max_depth 1、exec overruns 0 max_round 2 ms、heap min 141,252、link 栈余量 4,748。
- 结论：单会话 PASS，与 J5 单会话基线一致；双会话待报警器板接回后补测。

## 2026-11-12 — R3（命名空间与 include 根统一）

- 报警器板已接回，上电自动重连（`pushes_tx` 持续增长），本次为**双会话**（报警器 + 主机作手机）。烧录 `builds/r3`。

| 场景 | 实际速率 | RTCM 帧 主机/板 | 字节/CRC | rx_dropped 增量 | 产品 | p50/p99 ms |
| --- | --- | --- | --- | --- | --- | --- |
| 5 KB/s + 产品 | 5.8 KB/s | 286/286 | 一致 | 0 | 200/200 | 21/60 |
| 20 KB/s + 产品 | 20.8 KB/s | 1107/1107 | 一致 | 0 | 200/200 | 30/102 |
| 40 KB/s + 产品 | 28.9 KB/s（双会话 RFCOMM 信用限速） | 1593/1593 | 一致 | 0 | 201/201 | 202/300 |
| 40 KB/s 仅 RTCM | 28.9 KB/s | 1635/1635 | 一致 | 0 | — | — |

- 板端：write_errors 0、disconnects 0、tx queue_full 0 max_depth 2、exec overruns 0 max_round 2 ms、heap min 137,068、link 栈余量 4,748。
- 结论：PASS，与 J5 双会话基线（约 30 KB/s、p50 约 190 ms）一致；R2 当时缺的双会话一并覆盖。
