# ADR-0012：GNSS 接收机驱动化（Receiver 基类 + 厂家驱动）与输出配置闭环

- 状态：**已接受**（用户 2026-11-16 m07594）
- 日期：2026-11-16
- 修订：ADR-0010 §2.1/§2.2/§2.7/§2.8/§3（方言描述符 → 驱动类）；承接 ADR-0010 §2.9 与 ADR-0011 §6.2 中 G-C 的剩余项
- 不变：协议帧格式、NVS 命名空间与键（`gnss/family|model|baud|rate|gsv|out`）、蓝牙名、ADR-0011 的数据路径与内存预算

## 1. 背景

ADR-0010 用“家族方言 = constexpr 函数指针表”接入 UM98x/F9P/K803。K4 之后出现三个问题：

1. **厂家逻辑泄漏到 main。** 现状（`feat/gnss` @1548de8）：
   | 位置 | 泄漏内容 |
   | --- | --- |
   | `main/modules/gnss/GnssModule.cpp:23,134-139` | include `UnicoreHash.hpp`；按解码器名字 `"UnicoreHash"` / `"UnicoreResp"` 截日志标签 |
   | `GnssModule.cpp:211-223` | `infer_probe_family()` 按解码器猜家族 |
   | `GnssModule.cpp:644` | 只认 `nmea` / `unicore_hash` 两个解码器作 App 语句来源 |
   | `GnssModule.cpp:911-960` `apply_out_to_receiver()` | 手写 `UNLOG COMx` / `LOG COMx ... ONTIME`、Unicore 报文名表、ComNav 特例；**直接写 UART，不等应答**，失败无从得知 |
   | `GnssModule.cpp:997,1019` | NVS 读回的上界写死 `Family::kComnav` / `Model::kK803` |
   | `GnssCmds.cpp:84-91`、`debug/TraceText.cpp:389-396` | 两份相同的家族名 switch |
   | `main/data/GnssOutput.cpp:118-121` | `#SATSINFOA,` / `#HEADINGA,` 前缀判断 |
   新增一个接收机（例如 F9P 的 UBX 输出、K803）需要改 main 的 5 个文件。
2. **输出配置没有闭环。** 流动站脚本（`rover_step`）里写死了 GST/SATSINFOA/HEADINGA，控制台 `gnss out` 另走一条不校验应答的旁路；两者可能互相覆盖，失败也会被当成成功持久化（违反 ADR-0011 §6.2“失败不持久化”）。
3. **命名与规范。** `gnss::Reply` 与 `core::Reply` 重名（CODING-STYLE 规则 4，P2 推迟到本 ADR）；`build_output` 这类“目标不支持”的结果没有统一的返回类型（规则 1）。

用户决定（2026-11-16，m07295/m07305/m07319）：
- 基类 + 按厂家派生驱动，需要时再派生型号子类；
- 每个驱动自己适配输出，做不到的报“不支持”；
- 主板**没有板型 ID**，靠启动探测选驱动（同 SparkFun Flex 的 `gnssSupportRoutines[]`）；
- 没有 F9P / K803 实物：只做主机测试，报告标“未上板验证”。

## 2. 决策

### 2.1 分层（修订 ADR-0010 §2.1）

```
components/gnss（纯 C++20，无 IDF；依赖 stream_demux、app_fw）
  Types.hpp            Family/Model/cap、Fix、SatTable；新增 OutputMsg、family_name()、family_from_u8()/model_from_u8()
  Receiver.hpp         抽象基类 gnss::Receiver + CmdBuf/ConfigStep/RoverConfig/ProbeResult/Ack/OutputPlan
  NullReceiver.hpp     “无接收机”驱动：所有操作返回 kNotSupported
  drivers/UnicoreReceiver.hpp   UM980/981/982（N4 平台 ASCII）
  drivers/UbloxReceiver.hpp     ZED-F9P（UBX VALSET）
  drivers/ComnavReceiver.hpp    K803（实验性，默认不编译）
  DriverList.inc       驱动登记表（X-macro），唯一需要为新驱动改动的公共文件
  Controller.hpp       探测/配置/输出/监视状态机：持有 const Receiver*，不做 IO、不计时
  NmeaParser.hpp       不变
main/modules/gnss      只依赖 gnss::Receiver 接口；不再 include 任何 drivers/* 或厂家解码器头
```

依赖方向不变（main → gnss → stream_demux/app_fw）。gnss 新增依赖 `app_fw` 仅为使用 `fw::Status`（app_fw 纯 C++，主机可编，已被 link_core 等依赖）。

### 2.2 Receiver 基类

用**虚函数**替代函数指针表。理由：派生驱动可以共享家族内的辅助函数与默认实现（`build_output` 默认“不支持”就是一行虚函数默认体），型号子类按需覆盖；函数指针表做不到默认实现，每加一个字段都要改所有描述符。

约束（保持 ADR-0010 的性质）：
- 驱动**无可变状态**：所有成员函数 `const noexcept`，同样输入产生同样字节，便于重试与主机逐字节测试；型号差异通过参数 `Model` 传入。
- 实例为 `constinit` 静态对象，**不用 new/堆**；析构函数 `protected` 且非虚（`~Receiver() = default`），类型保持可平凡析构，满足 `-Wnon-virtual-dtor` 与 R2 的 constinit 约束。vtable 在 flash，每个实例只占一个 vptr。
- 时序、超时、重试仍集中在 `Controller`，驱动不做 IO、不计时。

接口草案（`components/gnss/include/gnss/Receiver.hpp`，冻结头）：

```cpp
namespace gnss {

/** 应答判定（原 gnss::Reply，按 CODING-STYLE 规则 4 改名，避免与 core::Reply 重名）。 */
enum class Ack : uint8_t { kNone, kAck, kNack };

/** 一次输出计划：每类报文的期望周期，0 = 关闭。由 main 根据 NVS gnss/rate、gnss/gsv、gnss/out 生成。 */
struct OutputPlan {
    std::array<uint32_t, kOutputMsgCount> period_ms{};
    uint8_t port = 2;        // 模块上接 ESP32 的口号（UM980 COM2；F9P UART1=1）
};

class Receiver {
public:
    // ---- 身份 ----
    [[nodiscard]] virtual Family family() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;       // "unicore" 等，诊断用
    /** 本驱动需要额外挂到 UART 分流器上的解码器（NMEA、RTCM3 由 main 固定挂载，按地址去重）。 */
    [[nodiscard]] virtual std::span<const demux::Decoder *const> decoders() const noexcept = 0;
    [[nodiscard]] virtual uint32_t caps(Model model) const noexcept = 0;

    // ---- 探测 ----
    [[nodiscard]] virtual bool build_probe(CmdBuf &out) const noexcept = 0;
    [[nodiscard]] virtual bool match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                           ProbeResult &out) const noexcept = 0;

    // ---- 基础配置脚本（流动站模式、RTCM 输入口、波特率；不含报文输出） ----
    [[nodiscard]] virtual bool rover_step(size_t index, Model model, const RoverConfig &cfg,
                                          ConfigStep &out) const noexcept = 0;
    [[nodiscard]] virtual Ack classify_reply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                             std::span<const uint8_t> sent) const noexcept = 0;

    // ---- 报文输出（本 ADR 新增） ----
    /** 本型号能输出的报文（位 = OutputMsg）。App 订阅与控制台据此拒绝不支持的报文。 */
    [[nodiscard]] virtual uint32_t output_mask(Model model) const noexcept;               // 默认：GGA|RMC|GSA|GSV
    /** 生成“把 msg 设为 period_ms（0 = 关闭）”的一条命令。不支持返回 kNotSupported，周期不可表示返回 kInvalidArg。 */
    [[nodiscard]] virtual fw::Status build_output(OutputMsg msg, uint32_t period_ms, Model model,
                                                  const OutputPlan &plan, CmdBuf &out) const noexcept; // 默认 kNotSupported
    /** 把分流器给出的一帧归类为 OutputMsg；非 App 报文返回 OutputMsg::kNone。默认实现识别 NMEA（忽略 talker）。 */
    [[nodiscard]] virtual OutputMsg classify_frame(const demux::Decoder &dec,
                                                   std::span<const uint8_t> frame) const noexcept;
    /** 日志用的短标签（"GNGGA"、"#VERSIONA"、"$command"、"UBX 05-01"），写入 out，返回长度。默认按 NMEA/RTCM 通用规则。 */
    virtual size_t frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                             std::span<char> out) const noexcept;

    // ---- 配置读回（可选，本 ADR 新增） ----
    /** 生成查询模块当前配置的命令；不支持返回 false。 */
    [[nodiscard]] virtual bool build_readback(CmdBuf &out) const noexcept;                // 默认 false
    /** frame 是否属于读回结果；是则把一行可读文本写入 out 并返回长度，否则 0。 */
    virtual size_t match_readback(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  std::span<char> out) const noexcept;                   // 默认 0

protected:
    constexpr Receiver() = default;
    ~Receiver() = default;
    // 禁止拷贝/移动：身份即地址（与 demux::Decoder 相同约定）
};

}  // namespace gnss
```

`CmdBuf`、`ConfigStep`、`RoverConfig`、`ProbeResult` 原样从 `Dialect.hpp` 移入。`RoverConfig` 删除 `gsv` 与 `rate_hz` 以外的报文输出含义：基础脚本只负责模式/口/波特率，报文输出全部走 §2.4 的输出计划。

`OutputMsg`（`Types.hpp`，值即 OutputPlan 下标，只能追加）：
`kNone=0xFF`、`kGga=0`、`kSatsInfo`、`kHeading`、`kGst`、`kRmc`、`kGsa`、`kGsv`、`kCount`。
前 6 个与 main 现有 `type_idx`（0x04/0x05/0x1F/0x21/0x22/0x23）顺序一致，`main/def/GnssSubDef.hpp` 只保留“协议 msg_id ↔ OutputMsg”映射。

### 2.3 驱动登记、选择与缓存（修订 ADR-0010 §2.2 探测顺序、§2.8 Kconfig）

**登记表** `components/gnss/include/gnss/DriverList.inc`（顺序即探测优先级）：

```cpp
// GNSS_DRIVER(Kconfig 后缀, 家族, 类型, 头文件)
GNSS_DRIVER(UNICORE, kUnicore, UnicoreReceiver, "gnss/drivers/UnicoreReceiver.hpp")
GNSS_DRIVER(UBLOX,   kUblox,   UbloxReceiver,   "gnss/drivers/UbloxReceiver.hpp")
GNSS_DRIVER(COMNAV,  kComnav,  ComnavReceiver,  "gnss/drivers/ComnavReceiver.hpp")
```

`Registry.cpp` 展开它生成 `enabled_receivers()`（按 Kconfig 过滤）与 `receiver_for(Family)`。新增驱动 = 新建 `drivers/XxxReceiver.{hpp,cpp}` + 表中加一行 + Kconfig 加一项 + `Family` 追加枚举值；**main 不改**。

**Kconfig**（`components/gnss/Kconfig`）：
| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `RTK_GNSS_DRV_UNICORE` / `_UBLOX` / `_COMNAV` | y / y / n | 替代 `RTK_GNSS_DIALECT_*`；`sdkconfig.defaults` 与 builds/* 同步改名 |

**选择流程**（无板型 ID，同 SparkFun Flex）：
1. NVS `gnss/family` + `gnss/baud` 有效且对应驱动已编译 → 先用它在该波特率探测一次；
2. 未命中 → `enabled_receivers()` 顺序 × 波特率表（不变，ADR-0010 §2.2）；
3. 命中 → `Controller` 记下 `const Receiver*`，main 从 `Controller::receiver()` 取用；探测结果写回 NVS（不变）；
4. 全表无应答 → 当前驱动保持 `NullReceiver`（不是 nullptr），状态 `kFailed` 并按原逻辑退避重探。main 任何地方都不需要判空。

`family_from_u8()` / `model_from_u8()`（`Types.hpp`）负责 NVS 读回的范围校验，替代 main 里写死的 `kComnav` / `kK803` 上界；`family_name()` 替代 main 的两份 switch。

### 2.4 输出计划闭环（承接 G-C / ADR-0011 §6.2）

- **单一来源**：main 由 `gnss/rate`、`gnss/gsv`、`gnss/out` 生成 `OutputPlan`（GGA/RMC/GSA 周期 = 1000/rate；GSV 由 gsv 决定；GST/SATSINFO/HEADING 等来自 `gnss/out`）。流动站脚本不再写死任何报文输出。
- **Controller 新增 kOutput 阶段**：基础脚本完成后，对 `OutputMsg` 逐项调用 `build_output()`：
  - `kOk` → 发送并按 `classify_reply` 等 ACK，NACK/超时按现有重试规则；
  - `kNotSupported` → 跳过，置入 `unsupported_mask`（不是错误）；
  - `kInvalidArg` → 记为该项失败。
  完成后进入 `kRunning`。全部结果（`applied_mask`、`unsupported_mask`、`failed_mask`）进 `ControllerStats`，`gnss status` 显示。
- **运行中修改**（控制台 `gnss out` / `gnss rate`）：命令经 CommandBus 交给 kIo，`Controller::request_output(plan)` 只重跑 kOutput 阶段，不重新探测、不复位模块。只有**全部支持项 ACK** 才写 NVS 并回 `OK`；否则回 `ERR` 并列出失败项，NVS 不变（ADR-0011 §6.2）。
- 删除 `GnssModule::apply_out_to_receiver()` 的直写 UART 旁路。

### 2.5 App 订阅与“不支持”的告知

协议 0530 已有失败结果码 `02 02`，**不扩展线上协议**：
- kIo 每次 kOutput 阶段结束后把 `supported = output_mask(model) & ~unsupported_mask & ~failed_mask` 发布到 `GnssAppSubState`（与 mask 同一把锁）；
- `GnssService` 收到 Type=0、interval≠0 的订阅时，若该报文不在 `supported` 中，回 `02 02` 而不是 `01 01`，不记订阅；
- interval=0 的查询行为不变（无缓存/不支持 → `02 02`）。

这样 App 不会订阅一个永远不会来的报文（例如 F9P 上的 0x05 SATSINFO、UM980 上的 0x1F HEADING）。需要修订冻结头 `core/services/GnssService.hpp` 增加 `supported` 入参或查询钩子，实施时给出最小改动。

### 2.6 帧归类下沉

- `GnssModule` 对每帧调用 `receiver().classify_frame()` 得到 `OutputMsg`，再更新最新句缓存与 App 环；main 不再出现 `#SATSINFOA` / `#HEADINGA` / 解码器名比较。
- `GnssOutput.cpp` 的 `classify_gnss_sentence()` 删除；NMEA 通用部分（忽略 talker、按语句类型）作为基类默认实现，Unicore 驱动覆盖以追加 `#SATSINFOA`/`#HEADINGA`。
- 日志标签走 `frame_tag()`，替代 `GnssModule.cpp:134-139` 的按名截取。

### 2.7 配置读回（UM980 `$CONFIG`，原 T4 已知缺口）

- `UnicoreReceiver::build_readback()` 发 `CONFIG\r\n`；模块逐行回 `$CONFIG,<口>,<配置>*hh`。
- 现在这些行被 `UnicoreResp` 解码器拒收（`byte 1 is 'C', expected 'c'`，并丢弃约 436 B）。修订 `demux::unicore_resp`（冻结头，更新 `docs/frozen.sha256`）同时接受 `$command,` 与 `$CONFIG,`，校验规则同 `$command`（含 `$` 的 XOR）。
- 控制台新增 `gnss config`：触发读回并逐行打印 `match_readback()` 的结果，结尾 `OK`；超时 1 s 无新行即结束。其它驱动默认返回“不支持”。
- 第一阶段只用于诊断，不做自动比对与修复（避免误把用户经 COM3 保存的配置当成错误）。
- **数据通路（2026-11-17 补充，P3c T0）**：
  - `Controller` 新增 `State::kReadback`（枚举末尾追加）与 `request_readback(now)`：仅 kRunning 且 `receiver().build_readback()` 成功时接受；下一次 `poll()` 写出该命令，进入等待；`on_frame()` 中每次 `match_readback()` 返回 > 0 计一行并把静默截止推到 now + 1000 ms；静默到期或总时长 5000 ms 到期回 kRunning，`stats.readback_seq`+1。读回期间不做 kRunning 的静默重探测。`readback_lines`/`readback_seq` 加入 `ControllerStats`。
  - 行文本不经控制器：GnssModule（kIo）在把帧交给 `on_frame()` 的同一处，`state()==kReadback` 时调用 `receiver().match_readback()` 取文本，追加到 `DataCenter::gnss_readback_text`（1536 B 交接缓冲，满则丢行并置 truncated）；结束时一次 `gnss_readback.write()` 发布 `GnssReadback{seq,lines,len,supported,truncated}`（Slot 上限 512 B，故文本不进 Slot）。
  - 控制台 `gnss config`：投递 `GnssSubVerb::kReadback`，等 `gnss_readback.seq` 变化（≤ 6 s），按 len 打印文本、再打印 `lines=<n>`（截断时加 `truncated`）与 `OK`；驱动不支持 → `ERR not supported`；状态不是 kRunning → `ERR busy`。

### 2.8 型号子类（2026-11-16 修订，P3s；原“本期不建”作废）

用户要求命令/应答操作以类组织、型号差异用派生表达，而不是在家族驱动里继续堆 `switch (model)`。

- **切换钩子。** `Receiver` 新增 `[[nodiscard]] virtual const Receiver &for_model(Model model) const noexcept;`，默认返回 `*this`。Controller 在 `match_probe` 命中后令 `detected_.receiver = &probe_receiver_->for_model(result.model)`；此后配置、输出、应答判定、帧归类全部经型号驱动。`caps` 的发布同样改用 `detected_.receiver`。`enabled_receivers()` / `receiver_for(Family)` / DriverList.inc 仍按家族登记（探测与 NVS 提示只认家族），型号子类不进登记表。
- **家族驱动可派生。** 去掉 `final`；命令生成与应答解析改为 `protected static` 成员（`cmd_unlog`、`cmd_mode_rover`、`cmd_config_baud`、`cmd_log_msg`、`cmd_unlog_msg`、`cmd_version_query`；`resp_has_valid_tail`、`resp_find_tag`、`resp_echo_matches`、`resp_text`），子类直接复用；型号差异点：`protected virtual rover_script()`（返回脚本步骤表，型号要加配置步骤时覆盖）与已有的公有虚函数 `caps(Model)`；`output_mask`、`build_output` 一律经 `caps()` 判断 HEADING 等能力，所以型号子类通常只需覆盖 `caps`。
- **家族驱动只描述基准型号。** `UnicoreReceiver` 描述 UM980/UM981（UM981 的 `kIns` 能力位仍在 `model_caps` 内，等真正使用惯导时再建 `Um981Receiver`）；不再认 HEADING。`Um982Receiver final : public UnicoreReceiver`（同一 .cpp，`constinit` 静态实例，访问器 `um982_receiver()`）覆盖 `caps`（加 `kHeading`），HEADINGA 随之进入 `output_mask` 与 `build_output`。对家族驱动直接以 `Model::kUm982` 调用得到的是 UM980 能力，这是有意的：调用方必须先 `for_model`。
- **规则。** 只在型号真有差异时派生；继承最多两层（Receiver → 家族 → 型号）；新增型号 = 子类 + 家族 `for_model` 一个分支，不改 Controller、main。u-blox、ComNav 本期只做“去 final + protected static 命令/应答”整理，不建子类。
- **代价。** 每个子类一个 vtable（几十字节 flash）+ 一个静态实例（一个 vptr）。

## 3. 冻结接口（修订 ADR-0010 §3）

- 新冻结：`components/gnss/include/gnss/{Receiver,NullReceiver}.hpp`、`gnss/DriverList.inc`、`gnss/drivers/*.hpp`
- 修订：`gnss/Types.hpp`（OutputMsg、family_name、*_from_u8）、`gnss/Controller.hpp`（Receiver*、kOutput 阶段、request_output、stats 掩码；P3c：kReadback、request_readback、readback 统计）、`demux/decoders/UnicoreResp.hpp`（`$CONFIG`）、`core/services/GnssService.hpp`（supported 掩码）、`main/def/GnssDef.hpp`（stats 字段）
- 删除：`gnss/Dialect.hpp`、`gnss/dialects/*.hpp`
- `docs/frozen.sha256` 同步重算

## 4. 实施拆分

| 卡 | 范围 | 执行 | 门槛要点 |
| --- | --- | --- | --- |
| **P3a** | gnss 组件：Receiver/NullReceiver/三个驱动/DriverList/Registry/Kconfig 改名/Types 辅助函数/Ack 改名/Controller 用 Receiver*（暂不加 kOutput；`rover_step` 保持现有字节，含 GST/SATSINFOA/HEADINGA，P3b 再把输出移出） | deepseek | **黄金向量**：改动前先用旧方言把探测命令、全部 `rover_step` 脚本（各型号 × rate × gsv × port × baud）、应答判定导出成 `test/gnss/golden/*.inc`；新驱动逐字节一致；test/gnss 用例数只增不减 |
| **P3b** | main 去厂家化：GnssModule/GnssCmds/TraceText/GnssOutput 改用 Receiver 接口；OutputPlan + Controller kOutput 阶段 + request_output；删除 apply_out_to_receiver；GnssService supported → `02 02` | deepseek | main 中 `rg -n 'Unicore|Ublox|Comnav|K803|SATSINFO|HEADING|UNLOG|COM%d' main/` 只剩注释与协议映射表；控制台 `gnss out` 失败不写 NVS 的主机用例；0x05 在不支持时回 `02 02` 的字节级用例 |
| **P3s** | 型号子类（§2.8）：`for_model` 钩子、家族驱动去 final + protected static 命令/应答、`Um982Receiver`、Controller 改用 for_model；u-blox/ComNav 同步整理 | deepseek | 黄金向量逐字节一致（测试调用改经 `for_model`，期望字节不变）；新增 for_model 与家族/型号能力差异用例；frozen 只允许本节列出的头变化 |
| **P3c** | `$CONFIG` 读回：UnicoreResp 修订 + 驱动 readback + `gnss config` | deepseek | demux 4096/边界用例；UM980 HIL：`gnss config` 列出全部行、无 `unknown bytes dropped` |

每张卡结束都跑全部门禁（4 种 IDF 配置 0 警告、9 个主机套件普通 + ASan/UBSan、G7b、G5b、clang-format、frozen），并上板（UM980）验证：冷启动探测、NVS 记忆后一次命中、`gnss out` 修改与重启后保持、手机 0x04/0x05 订阅与断开清零、RTCM 通路无回退。F9P/K803 只做主机测试，报告标“未上板验证”。

## 5. 代价与风险

- 虚函数调用比函数指针多一次间接，调用频率为每帧一次（≤ 数百次/秒），可忽略；flash 增加约 1–2 KB（vtable + 默认实现），DRAM 增加 3 个 vptr（12 B）。
- Kconfig 改名会让旧的 `sdkconfig` 回落默认值；仓库内 `sdkconfig.defaults*` 与 builds/* 一并更新，HIL 前全量重建。
- 输出阶段等待 ACK 会让冷启动多约 6×(往返 ~20 ms) ≈ 0.1–0.2 s。

## 6. 已决问题（用户 2026-11-16 m07594）

1. **0x0D 保持现状**（同 ADR-0011 Q3）：32 B COM 查询、SET 不回；持久化只走控制台 `gnss out` / `gnss rate`。
2. **“不支持”用 `02 02` 告知**（§2.5），不改协议、不在 0x02 加字段。
3. **不做 `RTK_GNSS_FIXED_DRIVER`**：只靠 NVS 提示 + 探测选驱动。
