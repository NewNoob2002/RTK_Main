# ADR-0021：P2 4G（EC600U）网络层：AT 驱动、链路管理、NTRIP 与 RTCM 来源仲裁

状态：**草案**（2026-11-26，待用户审批）。
关联：ADR-0008（代码组织与堆分级）、ADR-0011（启动顺序与内存）、ADR-0015（P2 板级：4G UART 32/33、GPIO26 使能）、ADR-0016（控制台档位）、ADR-0018（设置服务；§2.6 4G 约束、Q3 预留 `kNet`）、ADR-0020（OLED 状态页）。
需求依据：`docs/requirements/P2-nextgen-customer.md`（REQ-006 断网自动恢复、REQ-017 无定位不发 GGA）；协议草案 `docs/protocol/mqtt_fence_protocol.md`（§9 待服务器/App 确认，§10 实现边界）。
硬件依据：`docs/hardware/P2/schematic/p2v1.41.pdf`（EC600U、`4G_PWREN`、`4G_UART_TX1/RX1` 1V8↔3V3 电平转换、备注“4G 峰值 2A”“去除多余的 4G 电路复位”）。
旧实现参考（只读）：`P2 V2.7/EC600.ino`、`GSM.ino`、`GSM_Task.ino`、`GSM_MQTT_EVENT.ino`；`文档类/Ntrip获取设置.txt`、`P2-4G优化调试表.xls`。

> 命名：N = Net（4G 网络层）。用户 2026-11-26 确认：模块为 **EC600U**，有可用 SIM 卡；第一期 = 驱动 + 链路 + NTRIP，MQTT 第二期；旧版 TCP JSON / ThingsBoard 上报**预留不急于实现**。

## 1 背景

- P2 v1.41 板载 EC600U（Cat.1），经电平转换接 ESP32 `GPIO32`(RX) / `GPIO33`(TX)，`GPIO26` 为 `4G_PWREN`（高有效）。当前固件把 GPIO26 作为安全输出置低（ADR-0015 §3），4G 从未工作。
- 手机 Nav 协议的 0x11 挂载点列表、0x12 NTRIP 配置、0x32 JSON 服务器、0x33 ThingsBoard 目前只是 RAM+NVS 存储占位（`components/link_core/services/PhoneCompatService.cpp:370-629`）。
- 旧固件（V2.7）问题：所有 AT 交互为 `delay(500)` 轮询 + `strstr`；NTRIP 用透传模式（`AT+QISWTMD=<id>,2`）占住 AT 通道，无法同时跑 MQTT；`GSM_NetState` 置位后从不复位，断网不恢复（REQ-006 缺口）；无定位也上传 GGA（REQ-017 缺口）。
- 内存约束：ESP32 无 PSRAM；链入 Wi-Fi 的镜像启动后最小空闲堆实测 58,624 B，门槛 40,960 B（ADR-0017 §3、HB 报告），余量约 17 KB。因此 TCP/TLS/MQTT 一律用 EC600U 内置协议栈，不在 ESP32 上跑 lwIP-PPP / esp-tls / esp-mqtt（`mqtt_fence_protocol.md` §10）。

## 2 决定

### 2.1 硬件事实与板级门控

| 项 | 值 | 来源 / 待确认 |
| --- | --- | --- |
| 模块 | EC600U（Quectel AT 指令集：`QICSGP/QIACT/QIOPEN/QIRD/QISEND/QMT*/QSSLCFG`） | 用户确认、原理图 |
| UART | ESP32 `UART_NUM_1`，RX=`GPIO32`、TX=`GPIO33`，8N1，无硬件流控；上电默认 115200，`AT+IPR` 切到 460800（旧固件同） | `GSM.ino:7-10`；UART0=控制台，UART2=GNSS（`CONFIG_RTK_GNSS_UART_NUM=2`） |
| 电源 | `GPIO26`=`4G_PWREN` 高有效；关机/USB 在位关机路径置低（ADR-0015 §2） | `GPIO.ino:24-27`、`P2_V2.7.ino:142` |
| PWRKEY / RESET_N / NET_STATUS / RI / DTR | 原理图出现在模块侧；旧固件只用 PWREN，未见接到 ESP32 的 GPIO | **N1 前需核对原理图**：若 PWRKEY 未接 ESP32，则唯一的硬复位手段是 PWREN 断电重上 |
| 峰值电流 | 2 A（原理图备注） | 电池低电量 + 发射峰值可能引发掉压，N1 HIL 必测 |

- 使用已有的 `Feature::kCellular`（`main/board/BoardProfile.hpp:13`），仅 P2 v1.41 profile 带此特性。**无此特性的板型（E1 DevKit 把 GPIO26 用作电源保持，`BoardProfile.cpp:61`）绝不初始化 4G 模块、不触碰 GPIO26 与 UART1。**
- 射频模式：`RadioMode::kWifiConfig`（网页配置会话）期间 4G 保持断电，为 httpd 让出堆；仅蓝牙工作模式下按 `net.enable` 上电。此规则是 N1 默认，HIL 后可按实测余量放开（见 §5 Q4）。

### 2.2 执行器 `kNet`

- 启用 ADR-0018 Q3 预留的第三个执行器：`name="net"`，**core 0，priority 8**（低于 kIo 9、kLink 10），`roundBudgetMs=20`，`watchdog=true`，静态栈（初值 4096 B，N1 用高水位实测定稿）。
- `kNet` 的 `wait` 阻塞在 UART1 事件队列（带超时），与 kIo 阻塞 GNSS UART 的模式一致；kNet 上的模块不得调用 `delay` 式忙等，所有超时用 tick 比较推进状态机。
- 只有 `Feature::kCellular` 且模块清单包含 Net 模块时才创建 kNet，其他板型零开销。

### 2.3 代码组织（ADR-0008）

| 层 | 位置 | 职责 | 测试 |
| --- | --- | --- | --- |
| AT 引擎（纯逻辑） | `components/cellular/`：`AtEngine`（命令队列、行分帧、最终结果码 `OK/ERROR/+CME ERROR`、URC 分发、`>` 提示符与定长数据读） | 不含 IDF 依赖，输入字节 / 输出字节 | 主机测试 + 假 modem 脚本 |
| 链路状态机（纯逻辑） | `components/cellular/`：`LinkFsm`（§2.5） | 上电、注网、PDP、分级恢复决策 | 主机测试（时间注入） |
| 连接客户端（纯逻辑） | `components/cellular/`：`NtripClient`（N2）、`MqttClient`（N3 预留） | 基于 socket 抽象，不直接拼 AT 时序 | 主机测试 |
| 板级与模块胶水 | `main/modules/net/NetModule`：UART1 安装、GPIO26、执行器轮询、DataCenter 发布、设置/控制台/关机参与者 | 仅此处调 IDF | 门禁构建 + HIL |

- 运行期不分配堆：AT 收发缓冲、socket 读块、NTRIP 请求缓冲都是静态成员；`uart_driver_install` 在启动阶段一次性分配（计入 ADR-0011 启动预算）。

### 2.4 AT 通道规则

- **单通道复用，不用透传、不用 CMUX**：同一时刻只有一条 AT 命令在途；URC 随时可到，由行分帧器按前缀分发（`+QIURC`、`+QMTSTAT`、`+QMTRECV`、`+CEREG/+CREG`、`+CPIN`、`RDY`、`POWERED DOWN`）。
- socket 用**缓冲访问模式**（`AT+QIOPEN=...,<access_mode>=0`）：收到 `+QIURC: "recv",<id>` 后由 kNet 发 `AT+QIRD=<id>,1024` 分块取数，单块上限 1024 B，背压自然由读取节奏控制；这样 NTRIP 与后续 MQTT 能在同一 UART 上并存（旧固件透传模式做不到）。
- 发送用 `AT+QISEND=<id>,<len>` + `>` 提示 + 定长数据，单次 ≤ 512 B（NTRIP 请求、GGA 上行都小于此值）。
- 每条命令带独立超时（普通 300 ms～1 s；`QIACT` 150 s、`QIOPEN` 结果 URC 150 s，以 AT 手册为准，N1 卡列表）；连续 3 次无响应判定 AT 通道失效，交给 §2.5 L3 恢复。
- 开机同步：`AT` 连发直到 `OK`（≤ 10 s）→ `ATE0` → `AT+CMEE=2` → `AT+IPR=460800`（成功后本端切速，失败保持 115200 继续）→ `AT+GSN`（IMEI）→ `AT+QCCID`（ICCID）→ `AT+CPIN?`。

### 2.5 链路状态机与分级恢复（REQ-006）

```
Off ─(net.enable && 允许上电)→ PowerOn ─RDY/AT OK→ Sync ─→ SimCheck ─READY→ Register ─注册成功→ PdpUp ─QIACT OK→ Ready
 ↑                                                                                       │
 └──────────── L3：PWREN 断电 ≥ 2 s 重上（退避 30 s→60 s→120 s→300 s 封顶）◄── L2 失败 / AT 失效
```

| 级别 | 触发 | 动作 | 退避 |
| --- | --- | --- | --- |
| L1 连接级 | socket `closed` URC、NTRIP 30 s 无数据、`QIOPEN` 失败 | 只重开该连接 | 2 s→4 s→…→60 s 封顶；成功收到数据后清零 |
| L2 承载级 | `+QIURC: "pdpdeact"`、L1 连续 5 次失败、注册丢失后恢复 | `QIDEACT`→`QIACT`，再重开所有已启用连接 | 10 s→…→120 s |
| L3 模块级 | AT 通道失效、SIM 异常后恢复、`QIACT` 连续 3 次失败、注册超时 180 s | 断电重启模块（PWREN 低 ≥ 2 s） | 30 s→…→300 s |

- 健康巡检：Ready 状态每 30 s 查 `AT+CSQ` 与 `AT+CEREG?`（顺带确认 AT 通道活着），结果发布到 DataCenter。
- APN：`net.apn` 为空时不发 `QICSGP`，用模块/运营商自动 APN；非空时 `AT+QICSGP=1,1,"apn","user","pass",<auth>`。
- 所有退避与超时常量集中在一个 `constexpr` 表，主机测试逐级覆盖。

### 2.6 NTRIP 客户端（N2）

- NTRIP Rev1（HTTP/1.0 风格，`GET /<mount> HTTP/1.1` + `Authorization: Basic` + `User-Agent: NTRIP RTKMain/<ver>`），兼容旧 caster；响应接受 `ICY 200 OK` 与 `HTTP/1.x 200`；`SOURCETABLE 200 OK` 解析为挂载点列表（服务手机 0x11）。第一期只走明文 TCP，NTRIP over TLS 不做。
- GGA 上行：取 **Q1 门控之后**的 GGA（ADR-0018 §2.6，满足 REQ-017），默认间隔 10 s，可配 0（不发）/1–60 s。
- 收到的 RTCM 按块交给 §2.7 仲裁，不在 kNet 内解析 RTCM 帧（帧完整性由 GNSS 侧容错，统计字节/块数）。
- 设置键（ADR-0018 注册表，归属 `Owner::kNet`）：`ntrip.enable`、`ntrip.host`、`ntrip.port`、`ntrip.mount`、`ntrip.user`、`ntrip.pass`（只写，读回打码）、`ntrip.gga_interval`；手机 0x12 的现有 112 B 占位映射到这些键（保持现有报文布局），0x11 返回最近一次 sourcetable。
- MQTT 调用方（`Caller::kMqtt`）对 `ntrip.*`、`net.*` 一律 `locked`（ADR-0018 §2.6）。

### 2.7 RTCM 来源仲裁

- 来源：`kPhone`（蓝牙 App 转发，kLink）与 `kNtrip`（kNet）。同一时刻只有一个**活动来源**，非活动来源的数据丢弃并计数（新增丢弃原因 `kSource`）。基准站模式下两者都拒绝（沿用 `RtcmDropReason::kMode`）。
- 设置键 `rtcm.source = auto | phone | ntrip`，默认 `auto`：`ntrip.enable` 且 NTRIP 最近 5 s 内有数据 → NTRIP；否则手机。切换带 3 s 去抖，避免两路交替。
- `RtcmPipe` 目前是 kLink 单生产者流缓冲（`main/modules/gnss/RtcmPipe.hpp:20`）。N2 把写入端改为“仲裁后单写者”：活动来源判定与写入在同一把短锁内完成，保证任一时刻只有一个任务写入，消费者（kIo）不变。
- 仲裁状态与各来源计数发布到 DataCenter，供控制台、OLED（ADR-0020 COMM 页）、手机状态读取。

### 2.8 MQTT 与旧协议（第二期，预留）

- N3 使用 EC600U `AT+QMTCFG/QMTOPEN/QMTCONN/QMTSUB/QMTPUB`（必要时 `QSSLCFG` 走 TLS），协议按 `mqtt_fence_protocol.md`，§9 七个问题答复后定稿。
- 旧版 TCP JSON（0x32）与 ThingsBoard 上报（0x33、`v1/devices/me/telemetry`）**只预留**：保留现有 NVS 占位与设置键位置，N1/N2 不实现连接；`LinkFsm` 的连接槽按 NTRIP + MQTT + 1 个备用 socket 设计（EC600U `connectID` 0–11，本固件只用 0–2）。

### 2.9 关机与电源

- Net 模块登记 PW1 关机参与者（`kQuiesce` 阶段）：依次 `QICLOSE` 各连接 → `AT+QPOWD=1`（等 `POWERED DOWN` ≤ 1.5 s）→ GPIO26 置低；总时长 ≤ 2 s，在 PW1 3 s 保存超时之内；AT 无响应时直接拉低 PWREN。
- USB 在位关机、低电关机路径同样关 4G（ADR-0015 §2 已规定）。
- 低电策略：电池 < 10%（`bat_pct`）时不再上电 4G；已在线则保持到低电关机流程，以免峰值电流叠加导致掉压（HIL 验证，见 §5 Q3）。

### 2.10 控制台（ADR-0016）与状态

- `net status`：状态机阶段、SIM/IMEI/ICCID（ICCID 部分打码）、CSQ、注册状态、PDP、各连接状态与重连计数、退避剩余时间（所有档位）。
- `net power on|off`、`net reset`（DEV 档）；`net at <cmd>`（仅 DEV 档，在 kNet 队列中排队执行，回显结果，禁止 `ATE1`/`AT+IPR`/`AT+QISWTMD` 等会破坏状态机的命令）。
- `ntrip status`、`ntrip connect|disconnect`（N2）。
- OLED COMM 页增加 4G 行（CSQ 格数 + 阶段缩写 + NTRIP 是否在收），具体排版随 N2 或 OL 后续卡。

## 3 内存与体积预算

| 项 | 预算 | 说明 |
| --- | --- | --- |
| kNet 静态栈 | 4,096 B（N1 实测高水位后定稿） | `.bss` |
| UART1 驱动 | RX 2,048 B + 事件队列 16 项，TX 0（同步写） | 启动阶段堆分配一次 |
| AT 行缓冲 / 发送缓冲 | 512 B + 512 B | 静态 |
| socket 读块 | 1,024 B | `QIRD` 单块上限 |
| NTRIP 请求 + 设置镜像 | ≤ 512 B | N2 |
| 状态快照、计数 | ≤ 256 B | DataCenter |
| **合计** | **≤ 9 KB DRAM（含 UART 驱动），IRAM 0** | 链入 Wi-Fi 的镜像最小空闲堆仍须 ≥ 40,960 B；关 Wi-Fi 构建门槛不变 |

- Flash 增量目标 ≤ 40 KB（N1+N2）。

## 4 分期

| 卡 | 内容 | 验收要点 |
| --- | --- | --- |
| N1 | P2 profile 打开 `Feature::kCellular`、kNet 执行器、UART1、GPIO26 上电/断电、`AtEngine`、`LinkFsm`（到 Ready + L1–L3 恢复框架）、`net.enable/apn/user/pass` 设置键、关机参与者、控制台 `net *`、DataCenter 快照 | 主机测试：AT 分帧/URC/超时、状态机全路径与退避；门禁全过；HIL：插 SIM 开机到 Ready、拔天线/飞行模式恢复、拔 SIM 后 L3、关机 ≤ 2 s、低电/峰值不掉压 |
| N2 | `NtripClient`（sourcetable、Basic 认证、门控 GGA）、`rtcm.source` 仲裁与 `RtcmPipe` 单写者改造、`ntrip.*` 设置键、手机 0x11/0x12 映射、OLED/控制台状态 | 主机测试：HTTP 响应解析、GGA 间隔、仲裁切换去抖；HIL：外场 4G NTRIP 到 RTK Fixed、断网 60 s 内恢复、手机/NTRIP 切换 |
| N3（预留） | EC600U MQTT/TLS、电子围栏协议、旧版 JSON/ThingsBoard 兼容 | 等 `mqtt_fence_protocol.md` §9 答复后另起卡 |

## 5 风险与待确认

1. **Q1 PWRKEY/RESET_N 是否接 ESP32**：N1 开工前对照 `p2v1.41.pdf` 第 4G 页确认；若仅 PWREN，L3 用断电重上，并在 HIL 测模块掉电后的重新上电时间。
2. **Q2 460800 波特率稳定性**：电平转换（1V8↔3V3）下高波特率误码需 HIL 统计；不稳则固定 115200（NTRIP 1 Hz RTCM 约 1–2 KB/s，115200 足够）。
3. **Q3 峰值 2 A 与掉压**：低电量 + 注网发射峰值可能触发 BOD 或 SGM41513 限流，N1 HIL 用 `simbat` 之外的真实低电量测试。
4. **Q4 Wi-Fi 配置模式下 4G 断电**：默认断电换堆余量，若用户需要网页配置时 4G 继续收 NTRIP，需 N1 实测堆后再定。
5. **Q5 EC600U 固件版本**：TLS 版本/密码套件、`QIRD` 单块上限、`QMTPUB` 长度上限以模块实际固件为准（同时回答 `mqtt_fence_protocol.md` §9 Q1 的固件侧部分）。
6. **Q6 旧 App 兼容**：0x12 报文布局保持不变；旧 App 若依赖透传模式下的行为（如挂载点刷新时序），N2 HIL 用旧 App 验证。

## 6 备选方案与否决理由

| 方案 | 否决理由 |
| --- | --- |
| PPP + lwIP + esp-mqtt / esp-tls | 每条 TLS 会话数十 KB，Wi-Fi 镜像堆余量约 17 KB，放不下 |
| 透传模式（旧固件 `QISWTMD 2`） | 占住 AT 通道，NTRIP 与 MQTT 无法并存，URC 也收不到，断网检测只能靠超时 |
| CMUX 多路复用 | EC600U 支持，但实现复杂、需额外缓冲；单通道 + 缓冲访问模式已满足 2–3 路连接 |
| 放在 kLink 或 kIo 执行 | ADR-0018 §2.6 已否决：网络时延不可控，拖慢蓝牙链路与 GNSS 读 |
