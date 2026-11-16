# P3b：main 去厂家化 + 输出配置闭环 + 订阅“不支持”回 02 02（ADR-0012 §2.4–§2.6、§4）

状态：ISSUED 2026-11-16，基线 `feat/gnss` c745093（P3a）。实施：pi + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0012-gnss-receiver-drivers.md`（依据，冲突以 ADR 为准；本卡对 ADR 的细化以本卡为准）、`docs/reports/P3a.md`，
再读 `components/gnss/**`、`test/gnss/**`、`main/modules/gnss/**`、`main/data/GnssOutput.{hpp,cpp}`、`main/def/GnssDef.hpp`、`main/modules/bt_link/BtLinkModule.cpp`（`set_heading_supported` 附近）、`components/link_core/include/core/services/GnssService.hpp` 与其实现、`test/services/TestGnssService.cpp`、`components/app_fw/include/fw/Command.hpp`。

## 0 基线事实（c745093）

- 流动站脚本 `rover_step` 里写死报文输出：Unicore `UNLOG COMn` → GPGGA/GPRMC/GPGSA/GPGST（周期=1/rate）→ SATSINFOA 1 → GPGSV 1（gsv）→ HEADINGA 1（有航向能力）→ `MODE ROVER` → `CONFIG COMn <baud>`（不等应答，切波特率）。u-blox/ComNav 同理（见各驱动）。
- `GnssModule.cpp`：`kSetRate`（≈544）先写 NVS 再 `request_reconfigure`，立即回 OK；`kSetOut`（≈566）先写 NVS，运行中再调 `apply_out_to_receiver()`（≈911，直写 UART、不等 ACK）；Configuring→Running（≈796）仅当 `out_cfg_differs_from_default` 时再调它。**失败也会被持久化**，且默认启动 SATS=1 s / GSA=1/rate，而任一 `gnss out` 修改后 SATS=5 s / GSA=3 s，两套规则不一致。
- 厂家泄漏点：`describe_frame`（≈124）、`infer_probe_family`（≈209，用于 `GnssTrace::kProbeSend`）、`classify_gnss_sentence`（`main/data/GnssOutput.cpp:97`，GnssModule ≈646 调用）、`apply_out_to_receiver`。
- `GnssService`（冻结头）只有 `set_heading_supported(bool)`，由 BtLinkModule ≈464 按 `caps & kHeading` 设置；0x1F 不支持时已回 `02 02`。
- `fw::Command` 执行是同步的（无挂起/延迟完成机制），而等待模块 ACK 要靠 kIo 任务继续收帧，**不能在命令处理函数里阻塞等待**。

## 1 要做

### 1.1 OutputPlan 生成规则（单一来源）

`OutputPlan`（`gnss/Receiver.hpp`，本卡可修订）增加 `uint8_t rate_hz`（u-blox 需要用导航频率把周期换算成每历元次数）。main 新增纯函数（放 `main/data/GnssOutput.{hpp,cpp}`，主机可测）：

```
OutputPlan make_output_plan(const GnssOutConfig &out, uint8_t rate_hz, bool gsv, uint8_t port, uint32_t caps)
```

逐项规则（App 6 类，下标 = OutputMsg = 现有 type_idx）：
- mask 位关 → 0；
- 该项 interval **等于** `GnssOutConfig::defaults()` 的同项值 → 沿用旧流动站默认：GGA/RMC/GSA/GST = `1000/rate_hz`，SATSINFO = 1000，HEADING = 1000（无 `cap::kHeading` 时为 0）；
- 否则 → `GnssService::interval_to_period_ms(interval)`。
- GSV = gsv ? 1000 : 0（与旧脚本一致）。

这样默认配置下实际发给模块的字节与 P3a 黄金向量**等价**（见 1.6），而用户改过的项按设置生效；在报告里写清这条规则，供我向用户说明。

### 1.2 驱动

- `rover_step` 删掉全部报文输出步骤，只保留：Unicore `UNLOG COMn`、`MODE ROVER`、`CONFIG COMn baud`；u-blox/ComNav 同理只保留模式/口/导航频率/波特率相关步骤（具体由你按驱动现有脚本划分，报告列出每家新旧步骤对照）。
- 各驱动覆盖 `output_mask(model)` 与 `build_output(msg, period_ms, model, plan, out)`：
  - Unicore：`period_ms==0` → `UNLOG COMn <name>`；否则 `<name> COMn <period>`，period 文本与旧 `period_for()`/旧 apply 一致（秒，支持 0.05/0.1/0.2/0.5/1/2/5…，不可表示 → `kInvalidArg`）；HEADING 仅 `cap::kHeading` 型号，否则 `kNotSupported`。
  - ComNav：沿用旧 `apply_out_to_receiver` 的 `LOG COMn <name> ONTIME <p>` / `UNLOG`；SATSINFO、HEADING → `kNotSupported`。
  - u-blox：`CFG-MSGOUT-NMEA_ID_*_UART<port>` VALSET，值 = 历元数 `round(period_ms * rate_hz / 1000)`，0 关；不能整除且结果为 0 时按 1；SATSINFO、HEADING → `kNotSupported`。
  - GSV 不属于 App 订阅类，但同样经 `build_output(kGsv, …)` 下发。
- `NullReceiver` 保持全部 `kNotSupported`。

### 1.3 Controller：kOutput 阶段

- 基础脚本完成后进入输出阶段：对 `OutputMsg` 0..kGsv 逐项 `build_output`，`kOk` 发送并按 `classify_reply` 等 ACK（超时/NACK 沿用现有每步重试规则），`kNotSupported` 记入 `unsupported_mask` 跳过，`kInvalidArg` / 重试用尽记入 `failed_mask` 并继续下一项（单项失败**不**进入 kFailed）。全部处理完进入 `kRunning`。
- 新增 `bool request_output(const OutputPlan &plan, uint32_t now_ms)`：仅在 `kRunning` 接受，只重跑输出阶段（不探测、不复位、不跑基础脚本）；其它状态返回 false。`init()` 增加初始 plan 参数（或 `set_output_plan`，由你定，保持主机可测）。
- `ControllerStats` 增加 `applied_mask`、`unsupported_mask`、`failed_mask`（最近一次输出阶段）与 `output_runs`；再加一个单调递增的 `output_seq`，每次输出阶段**结束**时 +1（main 用它判断“我请求的那次完成了”）。
- 状态机原有转移、超时、重探测逻辑不变；`State` 若需新增 `kOutput` 枚举值则追加在末尾（NVS 不存它，但 trace/控制台有打印，同步更新）。

### 1.4 main：两阶段提交（替代立即写 NVS）

- `kSetOut` / `kSetRate` 处理函数：校验参数 → 把新值存为 **staged**（不写 NVS）→ 运行中调用 `request_output`（rate 变化需重跑基础脚本时用 `request_reconfigure`，其后照常进入输出阶段）→ 立即返回 `kOk`，`detail` = 期望的 `output_seq`（当前值 + 1）。非运行状态（未探测到接收机等）返回 `kBusy` 或现有合适的错误码，不改 NVS。
- 输出阶段结束时（`output_seq` 变化）：若有 staged 且 `failed_mask == 0` → 写 NVS、清 staged；否则丢弃 staged，并用 NVS 中旧值的 plan 再 `request_output` 一次（尽力恢复，结果只记 stats）。
- `GnssStatus`（`main/def/GnssDef.hpp`）增加 `output_seq`、`applied/unsupported/failed_mask`、`last_apply`（`kNone/kOk/kFailed/kRestored`）。
- 控制台 `gnss out ...` / `gnss rate N`：提交命令后轮询 `GnssStatus` 直到 `output_seq >= detail` 或 3 s 超时（每 50 ms 读一次 Slot，不阻塞 kIo），然后：成功 → 打印每项 applied/unsupported 结果，`OK`；失败 → `ERR output failed: <名字列表>`（NVS 未改）；超时 → `ERR timeout`。`gnss out show` 增加一列实际状态（applied/unsupported/failed/off）。
- 删除 `apply_out_to_receiver`、`out_cfg_differs_from_default`、`infer_probe_family`、`describe_frame`、`classify_gnss_sentence`（及其头文件声明与测试）：
  - 帧归类 → `receiver().classify_frame()`（结果 `OutputMsg` → type_idx；kGsv/kNone 不进 App 环）；
  - 日志标签 → `receiver().frame_tag()`；
  - `kProbeSend` 的家族 → 由 Controller 提供当前候选驱动（例如 `Action` 带 `const Receiver *` 或 `Controller::probe_receiver()`，你选一种并说明）。
- `GnssOutput.cpp` 中与厂家无关的部分保留。

### 1.5 GnssService：支持掩码 → 02 02

- 冻结头 `core/services/GnssService.hpp` 修订：`set_heading_supported(bool)` / `heading_supported()` 替换为 `set_supported_mask(uint32_t)` / `supported_mask()`（bit = type_idx；构造默认值 = 6 类全开但去掉 heading 位，即 `0x3F & ~(1U << 2)`，与旧的 heading 默认 false 一致）。
- `interval != 0` 订阅：该 type 不在 supported → `02 02`、不记订阅（现有 0x1F 分支推广到全部 6 类）。`interval == 0` 查询行为不变。
- main：每次输出阶段结束发布 `supported = output_mask & ~unsupported_mask & ~failed_mask`（只取 App 6 类），BtLinkModule 在原 `set_heading_supported` 处改为 `set_supported_mask(gnss_st.supported_mask)`（字段加到 `GnssStatus`）。未探测到接收机时 supported = 0（全部 02 02）——**注意这改变了无接收机时 GGA 订阅的回复**，在报告里列出新旧行为。
- `test/services/TestGnssService.cpp`：原 heading 用例改写为掩码用例，并新增：0x05 不支持 → 订阅 `02 02` 的字节级用例、支持时 `01 01`、掩码变化后已订阅项不受影响（不主动取消）。

### 1.6 测试

- test/gnss：
  - **等价性**：对每个驱动、P3a 黄金向量里的每个 rover 组合，用新 `rover_step` 全部步骤 + 对 `make_output_plan(defaults, rate, gsv, port, caps)` 等价的 plan 逐项 `build_output` 得到的命令集合，与黄金脚本的命令集合比较：Unicore/ComNav 按**命令字节多重集**相等；u-blox 按 VALSET 的 (key, value) 集合相等（报文分组可不同）。顺序不要求一致，但 `UNLOG COMn`（全部关闭）必须在任一输出命令之前、波特率切换步骤必须仍是基础脚本最后一步。黄金文件不得改。
  - `build_output` 各驱动：全部 OutputMsg × {0, 50, 100, 200, 500, 1000, 2000, 5000, 3 ms 等不可表示值}；不支持项返回 kNotSupported。
  - Controller：输出阶段 ACK/NACK/超时/不支持混合；`request_output` 只在 kRunning 接受；`output_seq` 每次结束 +1；输出失败不进入 kFailed。
- tools/main_host_check：`make_output_plan` 规则（默认、改单项、关单项、无航向能力）；两阶段提交（成功写 NVS、失败不写并恢复，用现有桩/模拟方式，若无法在主机上构造则写明并补足 Controller 侧测试）。
- 用例数各套件只增不减（被删除函数的测试迁移到对应新接口）。

## 2 门禁（写入 `docs/reports/P3b.md`，逐项 PASS/FAIL/NOT_RUN，附命令与关键输出）

1. IDF：`builds/p3b`、`builds/p3b-off`（`CONFIG_RTK_GNSS_ENABLE=n`）、`builds/p3b-comnav`（`CONFIG_RTK_GNSS_DRV_COMNAV=y`）0 warning；DRAM/flash 与 P3a（`builds/p3a`）差值并解释。
2. 主机：test/gnss（普通 + ASan，ComNav ON/OFF）、test/host、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/cli、tools/main_host_check（含 ASan）、tools/t13_host_check 全过。
3. G7b PASS；G5b 0 新增；clang-format 0 差异；`docs/frozen.sha256` 更新后 `sha256sum -c` 全 OK。
4. `rg -n 'Unicore|Ublox|Comnav|ComNav|K803|SATSINFO|HEADINGA|UNLOG|COM%' main/` 只剩：注释、协议 msg_id 映射/名称表（`gga|sats|heading…` 这类用户可见名）、`family_name` 调用；逐条列在报告里。
5. 报告列出：每家驱动新旧脚本步骤对照；1.1 规则；1.5 无接收机时的行为变化；所有修订的冻结文件；我上板要做的 HIL 清单（见 §3）。

报告末行 `P3b DONE` 或 `P3b BLOCKED: <原因>`。

## 3 HIL（审查方执行，供你写清单参考）

UM980：冷启动 `gnss` 显示 applied 掩码与旧行为一致、NMEA 频率不变；`gnss out gsa -3` → OK 且重启后保持；`gnss out heading 1` → OK 但显示 unsupported（UM980 无航向）；`gnss rate 5` → GGA 5 Hz；手机侧（tools/hil/spp_gnss_sub.py）0x04 订阅 `01 01`、0x1F 订阅 `02 02`；RTCM 通路无回退。

## 4 禁止

- 改协议帧格式、NVS 命名空间与键及 blob 格式（`GnssOutConfig` 不升版本）、蓝牙名、执行器参数。
- 修改 P3a 黄金向量；改本卡未提到的冻结文件；改 `test/host`、`reference/`、`AGENTS.md`、ADR 文档（不一致处写入报告）。
- 在命令处理函数或 kIo 任务里阻塞等待 ACK；`new`/堆、RTTI、异常；驱动内保存可变状态。
- 烧录、打开串口、联网、安装软件、git commit/切分支。构建目录只用 `builds/p3b*`。
