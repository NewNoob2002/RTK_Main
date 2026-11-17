# ADR-0010 GNSS 模块接入与多型号适配（第一阶段：流动站）

状态：Proposed（用户 2026-11-12 确认方向：启动探测 + Kconfig 选编、NVS 记忆；HIL 仅 UM980；先做流动站）
日期：2026-11-12
分支：`feat/gnss`

## 1. 背景

- 硬件（E1-Lite / E1-5.4）：UM980 的 COM2 接 ESP32 UART2（RX GPIO16、TX GPIO17），GPIO25 高电平给模块上电，默认 115200。COM1 接 lemo 口，COM3 接 CH340 Type-C。后续 PCB 可能换引脚、换型号（UM981、ZED-F9P、司南 K803……）。
- 现状：手机 RTCM 只进 `rtcm` 模块做统计（ADR-0006 §4 预留“UART 管道”）；0x40 状态的 fixQuality 固定 0xFF；0x04/0x05 为 `01 01` 占位；DeviceService 的 board 固定 "UM980"。ADR-0007 §4.5 预留了 `kIo` 执行器（core0/prio 9）和“加一个 GNSS 模块”的步骤；§3 把“运行时多型号识别与 GNSS 虚基类”推迟到出现第二款硬件——本 ADR 即该决策。
- 参考 SparkFun RTK Everywhere：`GNSS` 是约 110 个虚函数的胖基类（配置、查询、菜单、基站、日志全在一起），子类 `GNSS_UM980/ZED/LG290P/Mosaic`，型号由**主板平台**决定而不是探测模块；模块驱动直接做阻塞 IO（发命令、`delay` 等应答）。
- 本工程约束：多态只限已授权边界（Transport/Sink），扩展单位是 `fw::Module`；纯逻辑放平台无关组件并有主机测试；实时任务不阻塞；所有告警按错误处理。

## 2. 决策

### 2.1 分层

```
components/gnss （纯 C++20，无 IDF；依赖 stream_demux）
  Types.hpp        Family/Model/cap、Fix、SatTable（整数定点）
  NmeaParser.hpp  GGA/RMC/GSA/GSV → Fix/SatTable
  Dialect.hpp      方言描述符 Dialect（函数指针表）+ CmdBuf/ConfigStep/RoverConfig/ProbeResult
  dialects/{unicore,ublox,comnav}.hpp   每个家族一个描述符（+ 家族专用应答解码器）
  Controller.hpp   探测/配置/监视状态机：poll() 产出 Action，onFrame() 喂帧；不做 IO、不计时
main/modules/gnss （IDF 侧）
  gnss_module      kIo 执行器上的 fw::Module：UART 驱动、电源脚、Demux、Controller、NmeaParser、RTCM 输入管道
  gnssCmds        控制台 `gnss` 命令组
  gnss_keys        NVS 键表（ns "gnss"）
main/def/GnssDef.hpp   跨模块类型：GnssStatus、GnssVerb、GnssTrace、RtcmPipe 容量
```

与 SparkFun 的取舍：

| SparkFun 做法 | 本工程 | 原因 |
| --- | --- | --- |
| 胖虚基类，一个型号一个子类 | **按命令方言分家族**，每家族一个 constexpr 函数指针表（同 `demux::Decoder`）；型号差异用 `Model` + 能力位在家族内处理 | UM980/981/982 命令语法相同，只差能力；不引入新的多态边界；描述符可在主机上逐字节测试 |
| 驱动内阻塞发命令等应答 | 方言只“生成字节/识别应答”，时序与重试集中在 `Controller` 一处 | 执行器轮次不阻塞；所有家族共享超时/重试/静默检测逻辑，只测一遍 |
| 平台 ID 决定型号 | **启动探测**（Kconfig 选编哪些方言）+ NVS 记忆上次结果 | 同一固件适配多款 PCB；记忆让正常启动只发一条查询 |
| 每个模块自带解析器库 | 统一 `stream_demux`：NMEA、RTCM3、UBX、Unicore Hash 已有；家族专用应答（Unicore `$command`、ComNav `<OK`）以额外 `demux::Decoder` 随方言提供 | 一条 UART 一个分流器，帧边界与统计复用 ADR-0004/0005 |
| 菜单、基站、日志、PPP、L-Band 都在 GNSS 类里 | 第一阶段只做流动站；基站（survey-in/固定/RTCM 输出）第二阶段在 `Dialect` 上**追加**函数指针 | 控制范围；追加字段不破坏现有描述符的聚合初始化（新增字段放末尾，默认 nullptr） |

### 2.2 家族与型号

| 家族 | 型号 | 探测命令 → 应答 | 配置方式 | 应答识别 | 验证状态 |
| --- | --- | --- | --- | --- | --- |
| Unicore | UM980 / UM981 / UM982 | `VERSIONA\r\n` → `#VERSIONA,...;"UM980","<fw>",...*crc32`（`demux::unicore_hash`） | ASCII 命令 | `$command,<命令>,response: OK*hh`；其他 response 文本 = NACK | **HIL（UM980）** |
| u-blox | ZED-F9P | UBX-MON-VER 轮询 → MON-VER，扩展串 `MOD=ZED-F9P` | UBX-CFG-VALSET（RAM 层） | UBX-ACK-ACK / ACK-NAK（cls/id 匹配） | 主机测试（按 F9 HPG 1.32 接口文档） |
| ComNav | K803 | `LOG VERSIONA ONCE\r\n` → `#VERSIONA,...` | NovAtel 风格 ASCII | `<OK` / `<ERROR` 行 | **实验性，默认不编译**；未取得官方手册，拿到后先修订本节 |

Unicore 型号判定：VERSIONA 第一个引号字段（"UM980"/"UM981"/"UM982"；其他 UM9xx → 按 UM980 处理并保留原文到 fwVersion 前缀之外不另存）。

探测顺序：NVS 提示（家族, 波特率）优先，其后 `enabled_dialects()` 顺序 × 波特率表（Kconfig 默认波特率在前，再 115200/460800/921600/230400/38400/9600 去重）。一个组合发一次查询、等 `probeTimeoutMs`；全表 `probeRounds` 遍无应答 → `kFailed`，`retryBackoffMs` 后电源复位重来。

### 2.3 能力位

`cap::kRtcmIn`（全部）、`kBase`（全部，第二阶段用）、`kHeading`（UM982）、`kIns`（UM981）、`kRate20Hz`（UM98x、F9P）。控制台与 DeviceInfo 显示；第一阶段只用于把 `RoverConfig::rateHz` 夹到支持范围。

### 2.4 流动站配置（每次启动写 RAM，不 SAVECONFIG）

- 不写模块 flash：避免每次上电磨损，也避免与用户经 COM3 做的保存配置互相覆盖；代价是每次启动多约 1 s 配置时间。
- Unicore（目标口 `COM<port>`，默认 COM2）：`UNLOG COM2` → `GPGGA COM2 <1/rate>` → `GPRMC COM2 <...>` → `GPGSA COM2 <...>` → `GPGSV COM2 <max(1, 1/rate)>`（`gsv=false` 时省略）→ `MODE ROVER` →（`portBaud≠0`）`CONFIG COM2 <baud>`，此步不等应答、发送后主机 UART 切到新波特率（`switchBaud`）。频率参数按手册：1/2/5/10/20 Hz → `1`/`0.5`/`0.2`/`0.1`/`0.05`。RTCM 输入端口自适应识别（手册 §1.2），无需命令。
- u-blox：CFG-VALSET 分 3 条：① `CFG-RATE-MEAS`=1000/rate、`CFG-UART1INPROT-RTCM3X`=1、`CFG-UART1OUTPROT-NMEA`=1；② `CFG-MSGOUT-NMEA_ID_{GGA,RMC,GSA,GSV}_UART1`（GSV 关时 0）；③ 可选 `CFG-UART1-BAUDRATE`（不等应答 + 切换波特率）。
- 每步默认 500 ms 超时，超时/NACK 重发 `stepRetries` 次，仍失败 → `kFailed`（不跳步：跳过的步骤会让输出状态不确定）。

### 2.5 NMEA 解析（无浮点）

- 输入只有 `demux::nmea` 已校验的完整语句；Talker 不区分（GP/GN/GB/BD/GL/GA/GQ/GI）。
- 纬经度 `ddmm.mmmmmmm` → 1e-9 度整数（int64，精度 ≈0.1 mm，UM980 GGA 最多 7 位小数分）；高度/大地水准面 mm；HDOP/PDOP/VDOP ×100；差分龄期 0.1 s；速度 knots → mm/s（×514.444，整数运算）。空字段保持“未知”值，不报 malformed。
- GSV 成轮：每个系统收齐其“总条数”算完成；下一条 GGA（新历元）到达时把已完成系统的卫星合并 GSA 的 used 标记后整表发布（`kSatsReady`），避免控制台/手机看到半轮数据。
- 一致性：GGA 是定位主语句，`Fix::updateMs` 只在 GGA 更新；RMC 只补日期/速度/航向，GSA 只补 DOP 与 used。

### 2.6 Controller 状态机

```
kOff --start--> kPowerUp(断电 powerOffMs → 上电 → 等 bootMs)
  --> kProbing(组合游标：SetBaud → Write 查询 → 等应答/超时)
        命中 --> kConfiguring(脚本游标：Write → 等 ACK；NACK/超时重试；switchBaud 步发送后 SetBaud)
                    完成 --> kRunning(任何帧刷新 last_frame；静默 silenceMs → reprobes++ → kProbing)
        全表 probeRounds 遍无应答 / 配置失败 --> kFailed(等 retryBackoffMs) --> kPowerUp
```

- `Action` 一次只给一个动作，调用方循环 `poll()` 直到 `kNone`；`kWrite` 的字节指向控制器内部缓冲，下次 poll/on_frame 前有效。
- 只有一个“在途”命令；应答匹配由当前方言的 `matchProbe`/`match_reply` 判定，其他帧只用于静默检测（NMEA 另行交给 NmeaParser）。
- 时间比较全部回绕安全（`static_cast<int32_t>(now - deadline) >= 0`）。

### 2.7 main 侧集成

- **执行器**：新增 `ExecKind::kIo`（core0 / prio 9 / 栈 4096 B / idle_wait 20 ms / round_budget 20 ms），只承载 `gnss` 模块。等待钩子 = `uart_read_bytes(..., maxMs)`：有数据就喂分流器，所以 UART 数据到达即唤醒，无需额外事件。
- **UART**：`uart_driver_install(port, rx=4096, tx=1024, 0, nullptr, 0)`；RX 每次最多读 512 B；`kSetBaud` → `uart_set_baudrate` + `uart_flush_input`。第一阶段统计 rx/tx 字节与分流器 unknown/resync；驱动层 FIFO/环形缓冲溢出需要事件队列才能得知，暂不统计（HIL 以 NMEA 丢句与 demux 统计判断）。
- **分流器**：`demux::StaticDemux` 挂 `nmea`、`rtcm3`（模块输出 RTCM 时统计/第二阶段转发）+ 所有启用方言的 `decoders`（按地址去重），最多 `kMaxDecoders=8`。
- **电源脚**：Kconfig `RTK_GNSS_POWER_GPIO`（-1 = 无电源控制，`kPower` 动作忽略）与 `RTK_GNSS_POWER_ACTIVE_HIGH`。
- **RTCM 输入管道**（ADR-0006 §4 预留项落地，K3 直通修订）：`bt_link` 模块（kLink）在 Demux 回调确认 PHONE 角色后直接调用 `RtcmPipe::push(frame)` 推入完整 RTCM 帧（去掉中间 `RtcmBatch` 与负载复制）；空间不足整帧丢弃并计数（不写半帧，避免模块收到截断帧），`rtcm` 模块仅读取通路累计快照并控制 reset epoch。`gnss` 模块（kIo）取出最大 512 B chunk 并保留 pending 缓冲与已写偏移，处理 `uart_write_bytes` 短写/零写与可恢复重试，不可恢复错误显式 discard 并触发控制器恢复，同时与 UART RX 轮转分时避免相互饿死。管道 = `rtos::StaticStreamBuffer<4096>`（单写单读），为整帧丢弃新增 `space()` 查询。模块未在 `kRunning` 时也写入（UM980 上电即可接收 RTCM），但掉电/探测中清空。
- **数据主题**（DataCenter 新增）：`gnssStatus`（`GnssStatus`：state、family、model、fwVersion、hostBaud、caps、ControllerStats、NmeaStats、uart rx/tx 字节、rtcm 管道 push/drop/written）、`gnss_fix`（`gnss::Fix`）、`gnss_sats`（`gnss::SatTable`，512 B，只在成轮时写）。
- **链路**：`DeviceInfo` 新增 `fixQuality`（默认 0xFF；修订 ADR-0007 冻结头），`bt_link` 每轮比较 `gnss_fix` 版本，更新 `fixQuality`、`board`（型号名）、`board_fw`（固件版本）；`AlarmService::make_status` 改用 `info.fix_quality`（0x40 字节 4 不再固定 0xFF；GGA 超过 3 s 未更新视为 0xFF）。
- **0x04/0x05 等 GNSS 报文**：见 §2.9（G-C 落地；G-B 阶段保持占位）。
- **追踪**：`fw::TraceCat` 追加 `kGnss`（修订 ADR-0009 冻结头，追加在 `kCount` 前）；事件：state 变化、probe 发送/命中、step 发送/ack/nack/timeout、静默重探、管道丢帧、baud 切换。
- **NVS**（ns `"gnss"`）：`family` u8、`model` u8、`baud` u32（探测成功后若与已存不同则写，去抖由 storage 模块现有机制保证）；`rate` u8（1/2/5/10/20，控制台可改，默认 Kconfig）、`gsv` u8（0/1）。
- **控制台** `gnss`：`status`、`fix`、`sats`、`probe`（重新探测）、`reset`（电源复位）、`rate <hz>`（写 NVS 并重配）、`send <text>`（原样追加 `\r\n` 发给模块，调试用；只在 kRunning/kFailed 下允许，避免打乱脚本）。全部用 `cli::out_*`，成功以 `OK` 结尾。

### 2.8 Kconfig（main/Kconfig.projbuild `menu "GNSS"`）

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `RTK_GNSS_ENABLE` | y | 关闭则不编译 gnss 模块、kIo 执行器、管道（rtcm 只统计，与当前行为一致） |
| `RTK_GNSS_UART_NUM` | 2 | 不能与控制台 UART 相同（构建期 static_assert） |
| `RTK_GNSS_RX_GPIO` / `RTK_GNSS_TX_GPIO` | 16 / 17 | ESP32 侧引脚 |
| `RTK_GNSS_POWER_GPIO` | 25 | -1 = 无电源控制 |
| `RTK_GNSS_POWER_ACTIVE_HIGH` | y | |
| `RTK_GNSS_DEFAULT_BAUD` | 115200 | 探测首选与 NVS 无记录时使用 |
| `RTK_GNSS_PORT` | 2 | 模块侧连接 ESP32 的口号（UM980 COM2） |
| `RTK_GNSS_RATE_HZ` | 1 | NVS 无记录时的定位频率 |
| `RTK_GNSS_DIALECT_UNICORE` / `_UBLOX` / `_COMNAV` | y / y / n | 编译哪些方言（gnss 组件 Kconfig） |

### 2.9 手机订阅与原始报文透传（《测量软件控制协议-0530》，ADR-0011 修订）

协议约定（PDF 原文要点）：手机以 Type=0 查询某报文 ID；头部第 19 字节 `MSG Interval`（signed char）≠0 时，接收机**主动周期发送标准报文本体**
（无 AA 44 18 头与 CRC 二次封装）；v<0 → 周期 |v| 秒（如 `-128` → `128000 ms`），v>0 → 周期 `max(50, 1000/v) ms`，0 = 关闭自动输出并回一次封装应答。PDF 后续可能小改，
所以 ID→语句映射集中在一张 `constexpr` 表里。

| MSG ID | 报文 | 模块侧来源 | 协议默认 | 缓存上限 |
| --- | --- | --- | --- | --- |
| 0x04 | GPGGA | NMEA GGA | 1 Hz | 256 B |
| 0x05 | SATSINFOA（Unicore） | `#SATSINFOA` ASCII | 5 s | 4096 B |
| 0x1F | HEADING | UM982 `HEADINGA`（无能力位则不支持） | 1 Hz | 1024 B |
| 0x21 | GPGST | NMEA GST | 1 Hz | 256 B |
| 0x22 | GPRMC | NMEA RMC | 1 Hz | 256 B |
| 0x23 | GPGSA | NMEA GSA | 3 s | 256 B |

决策（数据路径与内存语义以 ADR-0011 为准）：
1. **模块输出与控制台持久化**：Unicore 流动站脚本在现有 GGA/RMC/GSA/GSV 之外追加 `GPGST COM<port> <rate>` 与 `SATSINFOA COM<port> 1`（双天线型号追加 `HEADINGA COM<port> 1`）；u-blox 方言追加 `CFG-MSGOUT-NMEA_ID_GST_UART1`（无 `SATSINFOA`）。控制台 `gnss out` 与 NVS `gnss/out` blob 管理模块默认输出表；App 每报文 `interval` 只做本次会话订阅与降采样，不改 NVS。
2. **订阅表**：单份（同一时刻只有一个 PHONE 会话，ADR-0007 CONCURRENT 策略下 PHONE ≤1），`mask` + 每类型周期/统计（`GnssAppSubState`）；PHONE 断开或重连后全清 `mask=0`，无自动吐流、无历史回放。
3. **跨任务路径（typed FanoutRing + SPP TX.stream）**：kIo 收到完整语句 → 分类并更新最新句缓存（总计 6144 B）→ 查订阅掩码与降采样到期 → 整句写入 4096 B `gnssAppRing`（typed `FanoutRing`，单条最大有效载荷 4093 B；≥4094 B 写侧整帧丢弃并计数 `oversizeDrop`）→ `bt_link` 每轮 `peek` 完整句经 `spp_server_stream_reserve` / `spp_server_tx_commit` 写入 PHONE 4096 B `TX.stream`（满时 `cancel` 留待下一轮）。
4. **查询应答（Interval=0）**：从最新句缓存通过 ≤256 B 有界查询拷贝回封装帧（载荷 = 最近一条原文）；无数据、缺少能力位或原文 >256 B 时，不截断、回结果码失败 `0x02 0x02`（>256 B 递增 `queryOversize`）。
5. **0x01/0x02/0x0D**：0x01 板卡型号/板卡固件来自探测结果（G-B 已写 DeviceInfo）；0x02 GNSS 功能字节按 ADR-0010 §2.9 约定应为 `kRunning ? 0x81 : 0x01`（注意：`Reply.hpp` 与 `DeviceService.hpp` 属冻结头，K4 若未获授权修订则在报告列出字段请求）；`0x0D` 保持 `DeviceService` 32 B COM 查询 / SET 不回，与 `gnss/out` 解耦。
6. **demux 容量**：`#SATSINFOA` 在多星时可达 3–4 KB，`unicore_hash::kMaxFrame` 由 1024 扩到 4096 并更新 `docs/frozen.sha256` 对应条目（与 `unicore_bin` 同为 4096 B，GNSS 分流器 RAM 不变）。
7. **后续**（不在本节）：自定义 Hash 报文与惯导报文（GPINS/GPFPD/INSPVAXA 等，PDF 0x0D 中已有 ID 972–974）作为新表项接入同一机制；K803 暂不支持。

### 2.10 NVS 存储目录（2026-11-12 补充）

命名空间与键名保持不变（旧数据可直接读出）。整数键由 `main/modules/storage/StoreCatalog.cpp` 按分组登记，
`StoreKeys.cpp` 提供按类型的通用读写；功能关闭时该组为空表，不再用 `#if` 阶梯。SN 与白名单仍走冻结的 `nvs_store`（ADR-0003）。

| 分组 | 命名空间/键 | 类型 | 内容 | 写入方 |
|---|---|---|---|---|
| identity | `rtk/sn` | str ≤16 | 设备 SN | `factory sn` |
| link | `rtk/alarm_wl` | blob n×6 | 报警器白名单 | 配对 / 手机 0x44 / `factory wl-*`（去抖保存） |
| gnss.config | `gnss/rate` | u8 | 定位输出频率 Hz | `gnss rate`（G-C 起：`gnss set`、手机 0x0D） |
| gnss.config | `gnss/gsv` | u8 | 是否输出 GSV | 同上 |
| gnss.cache | `gnss/family` `gnss/model` | u8 | 上次识别的模块，用于加快下次探测 | 探测成功后 |
| gnss.cache | `gnss/baud` | u32 | 上次通信波特率 | 同上 |
| debug | `dbg/mask` `dbg/report` | u32 / u16 | trace 类别掩码、周期报告间隔 | `debug save` |

G-C 将新增 `gnss/out`（blob，逐报文输出表，手机 0x0D 设置后永久保存）。

## 3. 冻结接口（实现方不得修改）

- `components/gnss/include/gnss/{types,dialect,nmea_parser,controller}.hpp`
- `components/gnss/include/gnss/dialects/{unicore,ublox,comnav}.hpp`
- `main/def/GnssDef.hpp`
- 修订：`components/app_fw/include/fw/Trace.hpp`（+`kGnss`）、`components/link_core/include/core/Reply.hpp`（`DeviceInfo::fixQuality`）、`components/rtos/include/rtos/StaticStreamBuffer.hpp`（+`space()`）

## 4. 验收

- 主机：`test/gnss` ctest——NMEA（UM980 实录语句、空字段、7 位小数、负坐标、多系统 GSV 成轮、GSA used 合并、畸形）、三个方言的字节级脚本与应答匹配、Controller（探测命中/换波特率/全表失败退避/配置 NACK 重试/切波特率/静默重探/回绕）；ASan。
- 构建：IDF esp32 0 告警；`RTK_GNSS_ENABLE=n` 也能构建；g7b PASS；DRAM/镜像增量记录到报告。
- HIL（UM980，/dev/ttyACM0）：冷启动探测到 UM980 + 固件版本、配置成功；`gnss fix` 有定位（室内可能无解则至少 GSV 有卫星）；NVS 记忆后第二次启动只发一条查询；`gnss reset` 后恢复；手机 RTCM（HIL 工具 40 KB/s 突发）全部写入 UART、管道丢帧数记录；蓝牙数据通路无回退（RTCM/产品帧 0 丢失）；0x40 fixQuality 与 `gnss fix` 一致；kIo 栈 HWM、heap min。

## 5. 开放问题

1. ~~手机载荷格式~~ → §2.9（PDF《测量软件控制协议-0530》）；PDF 后续修改时只改映射表与 phone_protocol.md。
2. ~~原始 NMEA 透传~~ → 需要，§2.9。
3. ~~司南 K803~~ → 用户 2026-11-12：暂不考虑，COMNAV 方言保持默认关闭、不做 HIL。
4. 基站模式（第二阶段）：survey-in / 固定坐标 / RTCM 输出 → 经 FanoutRing 发往手机与报警器。
5. 自定义 Hash 报文与惯导报文：格式待用户给出。
