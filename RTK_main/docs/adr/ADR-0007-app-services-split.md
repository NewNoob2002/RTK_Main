# ADR-0007 应用层架构：模块框架 + 服务化拆分 + 控制台组件

- 状态：**Accepted v3 2026-11-11**（用户审阅 v2 草图与流程图后要求补入 §3.6 的三项 SparkFun 借鉴，随后开始 J0）。
- 起因：用户要求“按你的方案启动应用层拆分，先写 ADR-0007”。v2 补充要求：本轮**以完善架构为主**，为后续加入其他组件做好准备，避免反复修改架构。
- 基线：ADR-0006 加上 I3 上板通过的固件（docs/reports/I3.md）。除 §7 列出的变化外，外部行为必须与基线一致。
- 评审深度：Full（解冻 `components/rtk_core/include/rtk/core/command_dispatcher.hpp`）。

## 0. 决策记录（用户答复 v1 §9）

| 问题 | 答复 | 落实 |
| --- | --- | --- |
| Q1 解冻 command_dispatcher.hpp | 同意 | J2 用 CommandRouter 替换；旧 `test/host/test_command_dispatcher.cpp` 保留到 J2 验收，之后移入 `reference/` |
| Q2 RTK 是否主动回连报警器 | 按建议 | RTK 只做 SPP 服务端，回连由报警器负责；量产行为不变 |
| Q3 正式报警器固件位置 | `P2_RTK/ExternalSpeak` | 该固件目前只有 I2S（main.c 237 行），还没有蓝牙。`bt_reconnect` 写成通用纯 C++ 组件，ExternalSpeak 以后加蓝牙时直接引用；本 ADR 不修改 ExternalSpeak |
| Q4 bt scan/connect 控制台 | 暂不做 | 本轮不新增 rtk_bt 的 scan/connect 驱动 API，也不新增 `bt *` 命令；控制台组件只提供扩展点 |

## 1. 现状问题

| 位置 | 问题 |
| --- | --- |
| `main/app_link.cpp`（789 行，20+ 个 `s_*`） | 一个文件承担 7 件事：组装根、会话表与 RX 调度、分流、白名单落盘、控制台命令队列、StatusSnapshot、BT 初始化状态 |
| `rtk_core/command_dispatcher.cpp`（471 行） | 一个 switch 处理全部 msg_id；`Context` 混合了报警、链路、设备信息、3 份配置；每加一条指令都要改这个类 |
| `main/app_console.cpp`（350 行） | 命令集中在一张表里，由 `CommandId` 枚举和一个大 switch 与链路耦合 |
| 扩展性 | 以后要加 GNSS UART、NMEA 回传、电池、LED、OTA 等组件时，没有固定的接入点：每加一个，都要同时改 app_link、dispatcher、console、snapshot 这 4 处 |

## 2. 架构原则（以后加组件都按这几条接入，不再改框架）

1. **模块（Module）是唯一的扩展单位。** 新功能就是一个新模块：在 `app_main` 的模块表里加一行，再按需登记 4 类扩展点：协议路由、控制台命令、存储键、数据主题。
2. **执行器（Executor）负责线程。** 一个执行器对应一个 FreeRTOS 任务，按顺序调用挂在它上面的模块 `poll()`。**每个模块只属于一个执行器**，模块内部状态因此单线程独占，模块本身不需要加锁。
3. **跨执行器只允许 3 种通道**，模块之间不得直接调用对方的方法：
   - **状态**：DataCenter 主题，单写多读，读最新值。
   - **字节流**：Pipe（StreamBuffer，单生产者单消费者，满了整帧丢弃并计数）。
   - **请求**：CommandBus（发到目标执行器的队列，带 seq 的结果回执）。
4. **编译期组装**：不用字符串查找注册，不用 `new`、`std::function`、RTTI；多态仍只限于 Transport 和 demux::Sink。模块操作表用 C 风格函数指针结构体，所有表都是 `constexpr` 或 `constinit` 静态对象。
5. **纯逻辑放纯组件**，能在 host 上测试；与 IDF 有关的部分放带 `rtk_` 前缀的组件或 `main`。

## 3. 架构草图

### 3.1 分层与组件

```
┌──────────────────────────────── main（组装根，只做组装） ──────────────────────────────────┐
│ app_main.cpp ── 模块表 kModules[] ── 执行器表 ── 路由表 ── 控制台表 ── 存储键表               │
│ 模块实现（IDF 相关）：bt_link_module  storage_module  rtcm_module  system_module            │
└───────────────────────────────────────────────────────────────────────────────────────────┘
        │ 使用                                │ 使用                         │ 使用
┌───────▼──────────────┐  ┌──────────────────▼────────────┐  ┌───────────────▼────────────┐
│ rtk_console（IDF）    │  │ rtk_os（IDF 封装）             │  │ rtk_bt（C 驱动 + 适配器）    │
│ esp_console REPL      │  │ StaticTask/Queue/StreamBuffer │  │ SPP 服务端、RX StreamBuffer │
│ ConsoleCmd 表注册     │  │ Executor（任务 + 模块轮询）     │  │ TX worker                  │
│ 参数解析辅助          │  │ CommandBus（队列 + seq 回执）   │  │                            │
└───────────────────────┘  └───────────────────────────────┘  └────────────────────────────┘
        │                                │                               │
┌───────▼────────────────────────────────▼───────────────────────────────▼───────────────────┐
│ 纯组件（host 可测，无 IDF/FreeRTOS 头）                                                      │
│  app_fw：Module/ModuleOps、DataCenter（Topic + seqlock Slot<T>）、Pipe 接口、StoreKey 描述   │
│  rtk_core：LinkManager  AlarmSource  LinkService  CommandRouter                             │
│            services/{device, config, alarm, pair}（每个服务提供 kRoutes[]）                │
│  stream_demux   rtk_proto   bt_reconnect（重连策略状态机，报警器端复用）                     │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
        │
┌───────▼──────────┐
│ rtk_store（C，NVS）│
└───────────────────┘
```

组件依赖（更新 AGENTS.md §3）：

| 组件 | 可 REQUIRES | 禁止 |
| --- | --- | --- |
| `app_fw`（新，纯 C++20） | 无 | IDF、FreeRTOS、`rtk_*` |
| `bt_reconnect`（新，纯 C++20） | 无 | IDF、FreeRTOS、`rtk_*` |
| `rtk_core` | rtk_proto、app_fw | IDF、FreeRTOS |
| `rtk_os` | rtk_proto、app_fw、freertos、esp_timer、log | rtk_core 以上 |
| `rtk_console`（新） | console、esp_system | 一切 `rtk_*` 业务组件（只依赖 IDF，可被报警器复用） |
| `main` | 以上全部 | — |

### 3.2 模块契约（`app_fw/module.hpp`，J0 冻结）

```cpp
namespace app {
struct ModuleOps {                       // 函数指针表，不用 virtual
    Status (*init)(void *self) noexcept;               // 启动前，单线程，可失败
    Status (*start)(void *self) noexcept;              // 执行器启动后、首轮 poll 前
    void   (*poll)(void *self, uint32_t now_ms) noexcept;  // 非阻塞；单次耗时上限见执行器
    void   (*on_command)(void *self, const Command &, CommandResult &) noexcept; // 可为 nullptr
};
struct Module {
    std::string_view name;   // 控制台 / 日志用
    ModuleId id;             // enum class，CommandBus 寻址
    ExecutorId executor;     // 所属执行器
    uint16_t poll_period_ms; // 0 = 每轮都调用
    void *self;
    const ModuleOps *ops;
};
}
```

执行器（`rtk_os/executor.hpp`）：
- 每轮固定顺序：等待唤醒（事件位、队列或超时）→ 处理 CommandBus → 按周期调用各模块 `poll()` → 统计本轮耗时 → 让出。
- 本轮超过预算时记录 overrun 计数，供 `status` 输出。

执行器规划：

| 执行器 | 核 / 优先级 | 本轮模块 | 以后可挂 |
| --- | --- | --- | --- |
| `kLink` | core1 / 10 | bt_link、rtcm、storage、system | 报警推送、配对 UI |
| `kIo` | core0 / 9 | （本轮不创建） | GNSS UART、NMEA 回传、电池采样、LED |

### 3.3 扩展点

| 扩展点 | 登记形式 | 消费者 |
| --- | --- | --- |
| 协议路由 | `inline constexpr Route kRoutes[] = {{msg_id, RoleMask, handler}}` | CommandRouter（编译期合并、排序，查重用 `static_assert`） |
| 控制台命令 | `inline constexpr ConsoleCmd kCmds[] = {{"group", help, fn}}` | rtk_console 批量注册 |
| 存储键 | `inline constexpr StoreKey kKeys[] = {{ns, key, type, size, default}}` | storage_module（去抖、重试、导入导出） |
| 数据主题 | `enum class Topic` 加 `TopicTraits<Topic::X>::type` | DataCenter（`Slot<T>` 静态存储） |

### 3.4 数据中心主题（本轮）

`LinkStatus`、`Sessions`、`AlarmState`、`Heartbeat`、`LinkStats`、`RtcmStats`、`StoreStatus`、`ExecStats`（各执行器的轮次、overrun、栈水位）、**`SystemMode`（预留，见 §3.6-3）**。
主题集合是产品相关的，定义在 `main/app_data.hpp`（J1 编写），每个主题一个 `app::Slot<T>`；以后加新主题只追加一个结构体和一个 Slot。

### 3.5 J0 冻结的接口细节（v3）

- **锁注入**：纯组件不能包含 FreeRTOS 头，跨任务共享的 `Slot<T>`、`FanoutRing` 通过 `app::LockOps`（函数指针 + ctx）注入锁；
  目标板由 `rtk::os::spin_lock_ops(portMUX_TYPE&)` 提供（临界区内只做 memcpy，单次 ≤ 1 KB），host 测试用 `app::kNoLock`。
  不用无锁 seqlock：C++ 内存模型下对非原子负载的并发拷贝是数据竞争（UB），而临界区代价可度量且足够小。
- **模块 poll 返回 bool**：返回 true 表示仍有待处理工作（如 RX 积压），执行器本轮等待时间取 0，复现基线的 `rx_pending` 行为。
- **执行器等待钩子**：执行器每轮先调用可选 `wait(ctx, max_ms)`（kLink 用 `rtk_bt_spp_poll_event` 的阻塞等待），没有钩子时阻塞在命令队列上。
  命令在每轮开头处理，最坏延迟 = 一轮（≤ 20 ms + 1 tick）。每轮末 `vTaskDelay(yield_ticks)`（基线为 1）。
- **CommandBus 并入执行器**：`Executor::call(cmd, result, timeout_ms)` 同步调用，按 seq 匹配，超时返回 `kTimeout`；结果队列满时丢最旧。
  只允许一个调用者任务（控制台）；多调用者以后加 per-caller 结果队列，不改 Command 结构。
- **路由绑定**：服务提供 `static constexpr Route kRoutes[]`（handler 签名 `DispatchResult(void *self, const FrameView&, uint32_t, Reply&)`），
  `CommandRouter::init(span<const RouteTable>)` 在启动时把 `{self, routes}` 展开到固定容量索引并检查重复 msg_id（重复 → `kInvalidArg`）。
  查找顺序复现旧 dispatcher：未登记 ID → `kNoReply`（先于角色检查）；角色不在 `RoleMask` → `kDenied`；type 限制由 handler 自查。
- **J2 拆两段**：J2a 只新增 router/services/test/services（不碰 link_service 与 main，可与 J1 并行）；
  J2b 在 J1 合入后把 `LinkService` 的依赖从 `CommandDispatcher&` 换成 `CommandRouter& + AlarmService&`（我先冻结新版 link_service.hpp），
  旧 dispatcher 与其测试移入 `reference/`。

### 3.6 借鉴 SparkFun 的三项预留（v3 新增）

1. **FanoutRing（app_fw，一写多读）**：GNSS 输出这类“一路数据发往多个出口”的场景。按记录（长度前缀）存储，单写者永不阻塞；
   写满时覆盖最旧记录，落后读者在下一次 `read()` 时整条跳过被覆盖的记录并累计 `dropped`；每个读者独立尾指针，最多 `kMaxReaders=4`。
   存储由调用方提供（静态数组），本轮实现 + host 测试（J4），不接入业务。
2. **StoreKey 元数据**：在存储描述之外加 `flags`（控制台可见 / 参与导入导出 / 只读 / 需重启生效）、`min/max`、`product_mask`、`help`，
   使同一张表以后驱动 NVS、控制台 `cfg get/set` 和 App 配置（对应 SparkFun `rtkSettingsEntries[]`，但静态、类型化、按模块分表）。
   本轮只迁移白名单与 SN 两个键；`cfg` 命令不做。
3. **系统工作模式**：预留 `Topic SystemMode{mode, since_ms}` 与 `ModuleId::kSystemMode` 位置（对应 SparkFun `States.ino`），本轮不实现状态机。

不借鉴：运行时多型号识别与 GNSS 虚基类（等第二款硬件再开 ADR）、网络消费者模型（无网络）、运行时可调任务优先级（违反静态可验证）。

## 4. 流程图

### 4.1 启动

```
app_main
  ├─ nvs/store 初始化 ── storage_module.init()：按 kKeys 读取 NVS（白名单、SN）→ 写入 DataCenter
  ├─ rtk_bt 初始化（失败 → system_module 记录 bt_ready=false，继续运行控制台）
  ├─ for m in kModules: m.ops->init(m.self)          // 任一失败 → 日志 + 标记，不启动该模块
  ├─ CommandRouter 组装（constexpr，无运行时成本）
  ├─ Executor kLink.start()  → 每个模块 start() → 进入轮询
  └─ rtk_console.start(合并后的 kCmds)
```

### 4.2 蓝牙入站帧（与基线时序一致）

```
BTC 回调 ── 拷贝到会话 RX StreamBuffer ── 事件队列 ── 唤醒 kLink
kLink 每轮：
  bt_link_module.poll()
    └─ drain_rx（每轮 256 B / 20 ms 预算，最多一个产品帧）
         └─ StaticDemux
              ├─ 产品帧 → LinkService.on_data → FrameParser → LinkManager 角色检查
              │            → CommandRouter.dispatch(msg_id, role)
              │                 └─ services/xxx handler → Reply → Transport.write（同步，200 ms 超时）
              ├─ RTCM（仅 PHONE）→ rtcm_module：统计 + crc32 → [以后] Pipe → kIo GNSS UART
              └─ 未知字节 → 计数
  rtcm_module.poll() / storage_module.poll() / system_module.poll()
  每 100 ms：LinkService.tick → 超时断开、配对窗口、报警推送
  每轮末尾：各模块把状态写入 DataCenter（单写者）
```

### 4.3 控制台命令

```
用户输入 ─ rtk_console（console 任务）
   ├─ 只读命令（status 等）：直接 DataCenter.read(Topic) → 打印        // 不进链路任务
   └─ 改状态命令（alarm/pair/wl/sn/rtcm reset）：
        CommandBus.post({module=ModuleId::X, verb, args}, timeout)
          → kLink 本轮开头取出 → 模块 on_command() → CommandResult{seq}
          → console 按 seq 匹配结果 → 打印（超时打印 "timeout"）
```

### 4.4 存储（以白名单为例）

```
LinkManager 白名单变化（配对成功 / wl clear）
  → storage_module.mark_dirty(Key::kWhitelist)
  → poll()：dirty 且距上次变化 ≥ 500 ms → 写 NVS
        ├─ 成功 → StoreStatus{ok}
        └─ 失败 → 退避 1 s、2 s、4 s … 30 s 后重试，StoreStatus{error, 次数}
```

### 4.5 以后加一个组件（例如 GNSS UART）

```
1. 新建 main/gnss_module.{hpp,cpp}（纯逻辑放进新的纯组件）
2. app_main：kModules[] 加一行 {name="gnss", id=kGnss, executor=kIo, ...}；首次使用 kIo 时启用该执行器
3. 需要哪些就登记哪些：kRoutes[]（协议指令）、kCmds[]（控制台）、kKeys[]（存储）、Topic::kGnss
4. 与其他模块通信：rtcm_module 的 Pipe → gnss_module 读取后写 UART
→ 不修改 app_fw、执行器、CommandRouter、rtk_console、DataCenter 的代码
```

## 5. 报警器重连（bt_reconnect，纯组件，本轮只实现组件和 host 测试）

- RTK 端：只做服务端（Q2）。ALARM 会话 3 s 无有效帧就断开；链路监督超时 8 s 不改。
- 状态机：`Idle → Discovering → Connecting → Connected → Backoff`，另有 `Stopped`。
  - 前 30 s 快速窗口，固定 1 s 重试；之后退避 1/2/4/8/16/30 s，每次 ±20% 抖动（种子由调用方注入）。
  - 连上并在线满 10 s 后才复位退避。
  - 缓存 SCN，连续 2 次 connect 失败才重新做 SDP。
- 输出只有“动作”（`kStartSdp`、`kConnect(scn)`、`kWait(ms)`），由调用方执行，所以 host 上可以全覆盖测试。
- 接入：本轮接入 `test_apps/rtk_spp_peer`，目标 MAC 存 NVS，上电自动重连。ExternalSpeak 加蓝牙时复用（Q3）。

## 6. 文件改动范围

- 新增：`components/app_fw`、`components/bt_reconnect`、`components/rtk_console`、`rtk_os/executor.hpp`（含 CommandBus）、`rtk_core/command_router.hpp`、`rtk_core/services/*`、`main/*_module.*`、`test/app_fw`、`test/services`、`test/bt_reconnect`。
- 删除或缩减：`main/app_link.cpp` 拆分完成后删除；`main/app_console.cpp` 缩减为各模块的命令表；`command_dispatcher.*` 在 J2 验收后移入 `reference/`。
- 不动：rtk_bt 驱动、rtk_proto、stream_demux、LinkManager、AlarmSource、LinkService、ExternalSpeak。

## 7. 允许的行为变化（其余一律视为回归）

1. `status` 改为按主题分段输出，新增 `exec:` 行；`stats:`、`rtcm:` 两行格式冻结不变（上板脚本依赖）。
2. 白名单落盘重试改为指数退避。
3. 报警器测试固件上电自动重连，并带退避。

## 8. 分步与分工

| 步骤 | 内容 | 执行 | 依赖 |
| --- | --- | --- | --- |
| J0 | 冻结接口头（app_fw、executor、command_bus、command_router、services、rtk_console、bt_reconnect）；更新 AGENTS.md；任务卡；frozen.sha256 | Claude | — |
| J1 | rtk_os Executor 实现；把 app_link 拆成 bt_link、storage、rtcm、system 模块；`main/app_data.hpp` 主题；console 改走 Executor::call + Slot 读取 | pi `codex/gpt-6.1-sol` | J0 |
| J2a | CommandRouter 与 4 个服务；golden 等价测试；test/services | pi `workbuddy/deepseek-v4.1-flash` | J0（与 J1 并行，文件不重叠） |
| J2b | LinkService 切换到 CommandRouter，main 接线，旧 dispatcher 移入 reference | pi `workbuddy/deepseek-v4.1-flash` | J1、J2a |
| J3 | rtk_console 组件；现有 7 条命令迁移到各模块的命令表 | pi `antigravity/gemini-3.8-flash` | J1 |
| J4 | bt_reconnect 组件与 host 测试；接入 peer 固件；FanoutRing 实现；test/app_fw（Slot、LockGuard、StoreKey、FanoutRing）。app_fw 的 CMakeLists 与 result.cpp 已由 J0 提供 | pi `antigravity/gemini-3.8-flash` | J0（可与 J1 并行） |
| J5 | 集成验收：门禁、烧录两块板、HIL 回归 | Claude | J1–J4 |

## 9. 门禁

1. `idf.py build` 零告警；frozen 校验通过；全部 host 测试（host、stream_demux、app_fw、services、bt_reconnect）在普通构建和 ASan/UBSan 下都通过。
2. CommandRouter 与旧 dispatcher 的 golden 向量逐字节一致；main_host_check 两种策略全部通过。
3. 资源：静态 DRAM 增量不超过 4 KB；链路任务栈余量不少于 1 KB；app 分区剩余不低于 55%；kLink 每轮 overrun 在正常负载下为 0。
4. 上板：I3 表 4.1 和表 4.2 复测不低于基线；spp_stress 窗口 32 的速率不低于 145 rep/s；peer 复位后重连 p95 ≤ 3 s。
