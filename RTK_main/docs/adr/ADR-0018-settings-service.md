# ADR-0018：设置服务与异步命令总线（多调用方配置）

状态：**已接受**（2026-11-20，用户答复见 §6）。由 SV 系列任务卡实施（见 §5）。
关联：ADR-0011（执行器与启动）、ADR-0014（运行期配置键与持久化）、ADR-0016（控制台档位，NONE 档不含控制台）、
ADR-0017（Wi-Fi 网页配置）、ADR-0012（GNSS 驱动与能力位）、ADR-0015（P2 板级：4G 模块 UART 32/33、GPIO26 使能）。

> 命名：SV = Settings serVice。

## 1 背景

用户 2026-11-20 提出两点：

1. 网页上的可选项必须与设备实际能力一致：接收机支持的才显示/可选，不支持的隐藏或禁用。
2. 配置不应依赖控制台（量产 NONE 档不编进控制台）；应有一套统一的底层接口，网页、App、控制台只是不同调用方。
   后续 4G 模块要接 NTRIP、MQTT，调用方还会增加。

现状（代码事实，基线 `feat/w2-web-server` e0a3001）：

- **命令总线**：`fw::Command{target, verb, args[32]}` → 目标模块 `onCommand()` → `fw::CommandResult`。
  唯一的跨任务入口是 `rtos::Executor::call()`（`components/rtos/include/rtos/Executor.hpp:135`）：
  **同步阻塞**、结果经单个 `resultQueue` 返回，注释明确“同一时刻只允许一个调用者任务（当前为控制台）”，
  且不能从执行器任务自身调用（死锁）。
- 因为 `call()` 只能给控制台用，其他调用方都自己在 DataCenter 里开专用通道：

  | 调用方 | 通道（`main/data/AppData.hpp`） | 附加机制 |
  |---|---|---|
  | 手机 PPP（0x33） | `phonePppCommand` / `phonePppResult` | seq 匹配，一条在途 |
  | 手机星座（0x06） | `phoneSatCommand` / `phoneSatResult` | 同上 |
  | 网页 GNSS 设置 | `webGnssCommand` / `webGnssOutcome` | 受理/完成两阶段、`webGnssCancelSeq` 取消、`webGnssFlushGgaSeq/DoneSeq` 退出前落盘 |

  每多一个“调用方 × 目标模块”组合就多一组 Slot + 序号 + 取消原子量 + 目标模块里的轮询函数（如 `GnssModule::pollWebCommand`）。
- **键与取值各写一份**：网页键表在 `components/web_proto`（`KeyTable.hpp`，`KeyKind`/`allowed`/`min/max`），
  手机报文映射在 `PhoneCompatService`，控制台取值解析在 `main/modules/gnss/GnssConsoleValue.hpp`。
  三处的合法范围、是否持久化、是否需要解锁彼此独立；Q1 门控在网页有、在 Configure App 是 TODO，就是这种分叉的结果。
- **能力不外露**：驱动已有能力位 `gnss::cap::kRate20Hz` 等（`components/gnss/include/gnss/Types.hpp:53`），
  但网页的频率选项写死在 `app.js`；驱动对不支持的频率“向下取到支持值”（`Receiver.hpp:87`），网页仍显示“已保存”。
  实测 CM680 当前 `gnss.out.gsv = 5000`（0.2 Hz），网页下拉没有此项而显示空白。
- 网页并不经过控制台：`WebModule` → `webGnssCommand` → `GnssModule::onCommand()`。唯一的耦合是 `hello.console` 显示档位名。
  但“网页 + NONE 档”组合从未构建验证过。

## 2 决定

### 2.1 分层

```
调用方（薄）          网页 WebLogic │ 手机 PhoneCompatService │ 控制台 config/gnss 命令 │（后续）MQTT 远程配置
                                     ↓ settings::Service::set/get/caps(key, …, Caller)
设置服务              键注册表（名称、类型、取值域、归属模块、持久化、权限） + 编码成 fw::Command
                                     ↓ Executor::post()/poll()/cancel()（本 ADR 扩展的异步总线）
归属模块              GnssModule / StorageModule / BtLinkModule / …  onCommand()（不变）
```

- 调用方只认 **键名 + 值**，不再自己拼 `fw::Command`，不再拥有专用 Slot。
- 归属模块不知道调用方是谁（除权限检查在服务层完成），也不再为每个调用方写 `pollXxxCommand`。
- 流数据（RTCM、NMEA、GGA 门控、BaseRtcmRing、FanoutRing）**不走**本总线，仍走现有 Pipe/Ring；本总线只承载控制面（设置、维护命令）。

### 2.2 异步命令总线（扩展 `rtos::Executor`）

在现有 `cmdQueue`（FreeRTOS 静态队列，本身多生产者安全）之上增加非阻塞接口，替换单一 `resultQueue`：

```cpp
struct Ticket { uint32_t seq = 0; uint8_t slot = 0xFF; };      // 0 = 无效
fw::Result post(const fw::Command &cmd, Caller caller, Ticket &out) noexcept;  // 不阻塞；队列或完成表满 → kBusy
PollState  poll(const Ticket &t, fw::CommandResult &out) noexcept;             // kPending / kDone / kCancelled
void       cancel(const Ticket &t) noexcept;                                    // 未执行则跳过并以 kTimeout 结束
void       complete(uint32_t seq, const fw::CommandResult &r) noexcept;         // 模块在本执行器任务内异步完成
fw::Result call(cmd, result, timeoutMs);                                        // 保留：= post + 任务通知等待 + poll
```

- **完成表**：每个执行器静态 `kMaxPending = 8` 项 `{seq, caller, state, result, waiterTask}`；无堆。
  `post` 占一项，调用方 `poll` 取走 `kDone` 后释放；`cancel` 后结果到达时直接释放。
- **两阶段完成**：需要等接收机应答的命令（GNSS 改频率、SAVECONFIG、PPP）在 `onCommand` 里返回
  `detail = kDeferred`（不改冻结的 `fw::Result`），模块稍后在本任务内调 `complete(seq, …)`。
  这取代现有 `WebGnssPhase::kAccepted/kDone` 与 `phonePpp*` 的手写两阶段。
- **线程规则**：
  - `post/poll/cancel` 任意任务可调（含其他执行器任务，例如 kLink 上的网页/手机向 kIo 投递）；
  - 调用方**禁止**向自己所在的执行器 `call()`（死锁），但可以 `post()` 后在后续轮次 `poll()`；
  - `call()` 只给可阻塞的任务（控制台 REPL）。
- **排队与仲裁**：同一执行器按 FIFO 执行，不同调用方的设置天然串行；**后写者生效**（不设配置会话锁，见 §6 Q1），每次成功设置使
  `DataCenter::configRev` 加一，所有调用方据此刷新视图（网页推 `config`，App 下次查询读新值，MQTT 发布变化）。
- **超时**：由调用方决定（网页 2 s、手机按旧协议、控制台 3 s），超时调 `cancel`；不在总线里放全局超时。

### 2.3 设置服务（新组件 `components/settings`，纯 C++，主机可测）

- **键注册表**（编译期常量表，扩展 `web_proto::KeySpec` 后迁入；`web_proto` 只保留 JSON 编解码）：

  | 字段 | 含义 |
  |---|---|
  | `name` | 点分键名，即网页协议键（`gnss.rate`、`gnss.out.gga`、`gnss.gga_gate.min_sats`、`bt.pair_next`、`factory.sn` …） |
  | `type` | enum / int range / bool / mac / string |
  | `owner, verb` | 归属模块与 `fw::Command` 动词；编码函数把值写进 `args` |
  | `domain` | 取值域来源：静态表，或**能力回调**（见 2.4） |
  | `persist` | 无 / NVS（归属模块负责写）/ 接收机 SAVECONFIG |
  | `access` | 允许的调用方位掩码 + 是否需要工厂解锁 |

- **接口**：`get(key) → value`（读 DataCenter 快照，不进总线）、`set(key, value, caller) → Ticket`（校验类型、取值域、权限后 `post`）、
  `caps(key) → Domain`、`forEach()`（网页生成 `config`/`caps` 用）。
- **校验在一处**：取值不在域内直接 `bad_value`，**不再由驱动静默降级**；驱动的“向下取整”只保留给启动时恢复旧 NVS 值。

### 2.4 能力驱动的取值域

- GNSS 驱动增加只读查询：`navRatesHz()`（例如 CM680 {1,2,5,10,20}）、`outputPeriodsMs(OutputMsg)`
  （含 0=关闭、5000=0.2 Hz 等真实支持值，且不超过当前定位频率）、`supportsSatMask()`（已有）、按型号的星座集合。
  能力在识别出型号后写入 DataCenter `gnssCaps` Slot（kIo 写，其他只读）。
- 网页协议 v1 **增量**：`hello` 增加 `caps` 对象（键 → 允许值数组或 `{min,max}`）；`gnss.rate` 改变后若输出域变化，
  设备再推一帧 `caps`。不在 `caps` 中的键网页隐藏；旧页面忽略未知字段，兼容。
- 手机协议：查询类报文（如 0x06 星座）按同一 `caps` 回答；App 侧显示由 App 团队决定。

### 2.5 控制台可选

- 控制台的 `config`/`gnss` 设置命令改为调用 `settings::Service`（只剩参数解析与打印），NONE 档去掉控制台不影响任何设置能力。
- 构建门禁增加 **NONE 档 + Wi-Fi** 组合（`tools/ci/run_gates.sh` 新增一项），保证网页/手机路径不链接控制台符号。

### 2.6 对后续 4G（NTRIP / MQTT）的约束（细节另立 ADR）

- 4G 模块（EC600U/Air600，UART 32/33，GPIO26 使能，ADR-0015）的 AT/PPP 状态机和 socket I/O **不放在 kLink**：
  网络操作时延不可控，会拖慢蓝牙链路。预留第三个执行器 `kNet`（core 0，优先级低于 kIo），由 4G ADR 确定栈与预算。
- NTRIP 客户端、MQTT 客户端作为 `kNet` 上的模块：
  - 它们读设置（挂载点、账号、服务器）用 `settings::get`，订阅 `configRev` 变化后重连；
  - MQTT 用于位置上报（只读，读 DataCenter，不经本总线）和电子围栏属性修改；后者是 `Caller::kMqtt`，经同一 `set()`，
    `access` 位只对电子围栏键开放，GNSS/门控/NTRIP/蓝牙/工厂键对 MQTT 一律拒绝（`locked`）；
  - NTRIP 上行 GGA 取自 Q1 门控之后的 GGA（与蓝牙相同的“不发坏 GGA”规则）。
- RTCM 改正数多来源（蓝牙 App 转发 vs 4G NTRIP）需要来源仲裁（同一时刻一个活动来源），在 4G ADR 中定义；本总线不承载 RTCM。

## 3 后果

- 好处：
  - 新增调用方 = 实现键名↔外部报文映射，零新增 Slot/原子量/轮询函数；新增设置 = 注册表加一行 + 归属模块一个动词。
  - 取值域、权限、持久化规则只有一份；网页、App、控制台、MQTT 行为一致。
  - 网页只显示设备真实支持的选项；不支持的值被拒绝而不是静默降级。
  - 控制台真正可选。
- 代价：
  - 改冻结头文件：`components/rtos/include/rtos/Executor.hpp`（新增 post/poll/cancel/complete、完成表存储）、
    `components/web_proto/include/web_proto/KeyTable.hpp`（迁移/精简）、GNSS 驱动能力查询（`components/gnss/include/gnss/Receiver.hpp`）。
  - 内存：每个执行器完成表约 8 × 48 B ≈ 0.4 KB 静态 RAM；注册表在 flash。
  - 迁移期三条旧通道要逐一删除，迁移需 HIL 回归（手机 0x06/0x33、网页全部设置、控制台）。
- 不变：执行器数量与优先级（直到 4G ADR）、模块归属、DataCenter 流数据路径、网页协议 v1 现有字段。

## 4 备选方案

| 方案 | 不选的理由 |
|---|---|
| 维持现状，每个调用方开专用 Slot | 4G 之后调用方 ≥ 5、归属模块 ≥ 4，组合数爆炸；已出现 App/网页功能分叉 |
| 允许多个任务并发 `call()`（多结果队列） | 仍是阻塞；kLink/kNet 等执行器任务不能阻塞，问题没解决 |
| 设置服务自己开任务串行处理 | 多一个栈（≥3 KB）和一次切换；执行器已有队列，可直接复用 |
| 网页直接调用控制台命令解析 | 与“控制台可选”矛盾，且文本解析不适合作为内部接口 |

## 5 实施计划（SV 系列，已完成：SV1 `1105c35`、SV2 `a74bf94`、SV3 `5bd560f`/`61d2be8`）

| 卡 | 内容 | 验收 |
|---|---|---|
| SV1 | `Executor` 异步接口 + 完成表；`call()` 改为基于 post；主机单测（多生产者、取消、延迟完成、表满） | 门禁全过；控制台命令行为不变（HIL 冒烟） |
| SV2 | `components/settings` 注册表与服务；GNSS 能力查询 + `gnssCaps` Slot；网页切到服务 + `hello.caps`；删除 `webGnss*` 通道 | 网页只显示能力内选项；CM680 GSV 0.2 Hz 正确显示；越界值 `bad_value` |
| SV3 | 手机 `PhoneCompatService`、控制台切到服务；删除 `phonePpp*`/`phoneSat*` 通道；NONE+Wi-Fi 构建门禁；补 Q1 门控的 Configure App 报文（报文号待 App 团队） | 手机 0x06/0x33/门控 HIL；NONE 档网页全部设置可用 |

前置：W2 合入 main。4G 不在本 ADR 范围，另立 ADR（编号待定），但必须使用本 ADR 的服务与总线。

## 6 已决问题（用户 2026-11-20 答复）

- **Q1 冲突策略**：蓝牙与 Wi-Fi 配置模式互斥（ADR-0017），网页与手机 App 不会同时在线，不需要配置会话锁。
  仍可能并发的组合：控制台 + 网页/App（开发档）、蓝牙模式下 App + 4G MQTT（电子围栏）。按“FIFO + 后写者生效 + `configRev` 刷新”处理即可。
- **Q2 MQTT 权限**：MQTT 只用于位置上报与电子围栏属性修改，不用于 GNSS/门控/NTRIP 等设置。注册表中只有电子围栏键对 `Caller::kMqtt` 开放。
  电子围栏键（对应旧协议 0x60–0x62）在 4G/围栏任务中加入注册表，本 ADR 只定权限规则。
- **Q3 4G 执行器**：同意预留 `kNet`（core 0、优先级低于 kIo），栈与内存预算在 4G ADR 中实测后确定。
