# P2 下一代设备客户需求（存档与规划）

- 来源：客户需求文档《P2下一代设备需求_中文译本.pdf》（2026-10-22，8 页，REQ-001～REQ-022）。
- 状态：**仅存档规划，未排期**。当前主线仍是完善 RTK_main 现有功能（见 PROGRESS.md）。
- 对照基线：旧固件 `P2_V3.1_IDF`（`main.c` 版本串 `V3.0.1`）；客户报告的问题固件为 **2.4.0.9**，需确认客户现场版本与该源码是否同一分支。

## 1. 需求与差距

| REQ | 需求摘要 | 旧固件现状（源码证据） | 归属 |
|---|---|---|---|
| 001 | 报警 ≥85 dB(A)@1 m | 自激蜂鸣器，GPIO 开关 | 硬件 |
| 002 | 扬声器不在背面 | — | 硬件 |
| 003 | LED 10 m 迎光可见 | — | 硬件 |
| 004 | ≥10 围栏×40 点，double ≥7 位小数，设备端判断，报警 ≤1 s | `user_geofence.h`：5×20；随 GGA 触发判断 | 固件 |
| 005 | 空中加载/更新单个或全部围栏，API 有文档 | 围栏仅蓝牙下发；MQTT RPC 只有 `query_status`/`set_status`/`warning` | 固件+服务器+App |
| 006 | 网络（RTK 改正+MQTT）数秒内自动恢复；长断网与围栏报警区分显示 | `GSM_NetState` 初始化后不复位；`QMTSTAT` 仅清 `Thingsboard_status`，未见 PDP/socket/MQTT 重建 | 固件 |
| 007 | 充电时不自动开机 | `main.c:119` 充电界面需长按 2.5 s 才开机，需 HIL 复现客户现象 | 固件/硬件 |
| 008 | 完整 API 文档（MQTT/RPC、载荷、应答） | 无 | 固件+服务器 |
| 009 | 围栏类型 1 字节，可小消息修改 | `warn_type` 1 内/2 外 | 固件+App+服务器 |
| 010 | 声+光/仅声/仅光，≥2 种声音，闪烁频率可调或常亮 | 每围栏 led/beep/motor 开关；无 PWM，节奏绑 GGA 频率 | 固件+App |
| 011 | 抗震 | — | 硬件 |
| 012 | KlickFast 支架 | — | 硬件 |
| 013 | （可选）蓝牙音频到 Peltor 耳机 | 无 | 评估 |
| 014 | HTTP/NTRIP/MQTT 用 TLS 1.2/1.3，校验证书，文档化密码套件 | 全部明文 TCP（`AT+QIOPEN ..."TCP"`，`QMTOPEN` 无 SSL） | 固件+服务器 |
| 015 | CA 信任库可更新 | 无 | 固件+服务器/App |
| 016 | 仅用户显式操作后允许蓝牙配对（工厂例外） | `xyz_bluetooth.c:48-54` 自动接受，IO cap `NONE` | 固件+App |
| 017 | 无定位时不得向 RTK 服务发送 GGA | `user_gsm.c:288` 无条件发送 `GGA_buff` | 固件 |
| 018 | 可编程配置 API | 部分 BT 协议 | 固件+服务器 |
| 019 | 初始配置后仅授权方可改配置 | 无 | 固件+服务器+App |
| 020 | SingularNav App 可靠 | — | App |
| 021 | 2.4.0.9 连不上 PointPerfect Flex；蜂鸣间隔/时长错；断网不恢复 | 见 006/010/017 | 固件 |
| 022 | 各台特性一致（音调、闪烁规律） | 音调由自激蜂鸣器决定（2.7 vs 3 kHz） | 硬件+固件 |

## 2. RTK_main 现状
4G（EC600）未实现（需新 ADR，复用 ADR-0018）；围栏 0x60/0x61/0x62 为 `PhoneCompatService` 占位；蜂鸣器/马达/LED（GPIO13/15/23）只做安全关断。

## 3. 拟定工作包（未排期）
1. **WP-NET**：4G 连接状态机（PDP→socket→MQTT/NTRIP 分级重连）、TLS+CA 更新、GGA 有效性门控、PointPerfect Flex 联调、MQTT 配置/围栏 API 与文档 —— REQ-005/006/008/014/015/017/018/019/021。先出 ADR。
2. **WP-FENCE**：围栏引擎与固定容量持久化（≥10×40 或 8/16 kB 预算）、类型字节、≤1 s 报警 —— REQ-004/009。
3. **WP-ALARM**：LEDC PWM 生成音调与闪烁模式，报警模式/声音选择 —— REQ-010/022（音调一致性需可定频驱动的蜂鸣器）。
4. **WP-BT-PAIR**：按键开启配对窗口 + 工厂例外 —— REQ-016。

## 4. 待与服务器/App 同事确认
- 服务器是否已有围栏管理与下发接口（旧固件 MQTT 侧未见围栏 RPC，只能确认设备端）。
- MQTT 载荷上限：服务器 broker 与 EC600 `AT+QMTPUB` 单包长度上限需查实后再定分包方案。
- 设备身份与授权（access token / X.509、配置归属锁定）。
- TLS 端点（MQTTS/HTTPS/NTRIPS）与证书链、CA 更新通道。

## 5. 旧版协议文档（参考基线）
位置：`P2 手持式GNSS接收机项目/文档类/`（仓库外）。
- `MQTT平台指令交互.doc`：自定义 MQTT（v3.1/3.1.1，ClientID+用户名+密码）；上行 `v1/devices/me/telemetry` `{"gga","warn_status","client_id"}`；下行 `v1/devices/<SN>` `{"params":{"warning":"1|2","warning_precision":"4.0"}}`；1 min 无下行自动报警。**无围栏下发、无应答、无 TLS。**
- `P2本地电子围栏专用协议指令-20260707.docx`：蓝牙二进制 0x60（列表）/0x61（单围栏读写：名称16B+enable/led/beep/motor/warn_type/point_num+2B 保留+n×(lat,lon double)，记录长 24+16n，40 点=664 B）/0x62（手动打点 Set_Code 1–5）。
- `P2 JSON交互协议说明书.doc`：TCP JSON（motor/beep/led/GGA/upload/GNSS/query_status/set_status）。
- `ThingsBoard OTA升级 - 副本.docx`：ThingsBoard 标准 OTA 主题（`v2/fw/response/+/chunk/+`）。

## 6. 协议制定分工（建议）
- **固件侧起草设备端协议**（MQTT 主题/载荷/应答/错误码、围栏记录格式、分包与版本/CRC、BT 0x60–0x62 扩展）：受存储容量、EC600 单包上限、报警时延等设备约束决定。围栏 MQTT 载荷建议复用 0x61 二进制记录（扩展类型/报警模式字段），保证 BT 与 4G 同一数据模型。
- **服务器/App 同事负责并评审**：围栏主数据归属与冲突策略、授权与设备身份、对客户的服务器 API 文档、App 编辑/配对/锁定流程。
- 双方签字后写 ADR（4G/围栏），再排期实现。
