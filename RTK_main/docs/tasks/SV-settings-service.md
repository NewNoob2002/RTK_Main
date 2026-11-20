# SV：设置服务与异步命令总线（SV1 → SV2 → SV3）

- 依据：ADR-0018（全文，尤其 §2.2–§2.5）；用户 2026-11-20 要求：网页可选项与设备能力一致；配置不依赖控制台；
  为 4G（NTRIP/MQTT）等更多调用方统一接口。
- 前置：W2（含 W8 网页 e0a3001）合入 main 并通过 HIL。本卡基线 = main `8c28078`（W2+W8 已合入；W2 的蓝牙 RTCM 10 分钟负载测试仍待做）。
- 分支 `feat/sv-settings`，工作树 `RTK_Main-sv`，构建前缀 `builds/sv1-*` / `sv2-*` / `sv3-*`。
- **三段顺序实施，每段单独交报告 `docs/reports/SV<n>.md`，Architect 验收后才开始下一段。**
- 执行者不提交、不烧录、不连串口；HIL 由 Architect 做。
- 授权修改的冻结文件（报告中逐个列出，由 Architect 重算 `docs/frozen.sha256`）：
  `components/rtos/include/rtos/Executor.hpp`、`components/web_proto/include/web_proto/*.hpp`、
  `components/gnss/include/gnss/Receiver.hpp` 及各驱动头、`docs/web/ws-protocol.md`（只做 §2 caps 增量）。其余冻结文件不得改动。

## 0 通用约束

- AGENTS.md 全部规则：无堆、静态任务、`FW_LOG*`、中文注释、Doxygen `@brief`、clang-format、门禁 `tools/ci/run_gates.sh`。
- 不改执行器数量、优先级、核绑定；不改流数据路径（RtcmPipe、BaseRtcmRing、GnssAppRing、FanoutRing）。
- 网页协议 v1 现有字段语义不变；只**增加** `caps`。旧页面必须仍能工作。
- 手机协议报文格式不变（`docs/protocol/phone_protocol.md`）。
- 每段结束：门禁全过；报告写明内存变化（`idf.py size` 的 DRAM/IRAM/bin 与 SV 前对比）。

## 1 SV1：Executor 异步接口

1. 在 `rtos::Executor` 增加（签名可微调，须在报告说明）：
   ```cpp
   enum class Caller : uint8_t { kConsole, kWeb, kPhone, kInternal, kMqtt /* 预留：仅电子围栏键，ADR-0018 §6 Q2 */ };
   struct Ticket { uint32_t seq = 0; uint8_t slot = 0xFF; };
   enum class PollState : uint8_t { kPending, kDone, kCancelled, kInvalid };
   fw::Result post(const fw::Command &cmd, Caller caller, Ticket &out) noexcept;
   PollState  poll(const Ticket &t, fw::CommandResult &out) noexcept;
   void       cancel(const Ticket &t) noexcept;
   void       complete(uint32_t seq, const fw::CommandResult &r) noexcept; // 仅本执行器任务内调用
   fw::Result call(const fw::Command &cmd, fw::CommandResult &result, uint32_t timeoutMs) noexcept; // 保留
   ```
2. 完成表：静态 `kMaxPending = 8` 项，存于 `ExecutorStorage`（与 cmdArea 同处，静态分配）；临界区保护；
   `seq` 单调递增跳过 0；表满或队列满 `post` 返回 `kBusy`。
3. 延迟完成：`onCommand` 置 `result.detail = fw::kDetailDeferred`（新常量放 `Executor.hpp`，不改 `fw::Result`/`Command.hpp`）
   表示“已受理，稍后 `complete`”；执行器此时不写完成。
4. `call()` = `post` + `xTaskNotify` 等待（`waiterTask` 记在完成表）+ `poll`；超时则 `cancel` 并返回 `kTimeout`；
   从本执行器任务调用仍返回 `kInvalidState`。原“只允许一个调用者”的限制取消。
5. `cancel`：未执行 → 执行器取出时跳过，状态 `kCancelled`；执行中/延迟中 → 结果到达时丢弃并释放表项。
6. 主机单测（`test/host` 冻结，新增测试放 `tools/main_host_check` 或 rtos 组件自带测试，按现有门禁结构选择并说明）：
   多生产者并发 post、FIFO 顺序、表满 kBusy、取消未执行/延迟中、延迟完成、seq 回绕、`call` 超时。
7. 现有调用方行为不变（控制台仍用 `call`；三条专用通道 SV2/SV3 才删）。

## 2 SV2：设置服务 + 能力 + 网页

1. 新组件 `components/settings`（依赖 `app_fw`、`rtos`；不依赖 ESP 头以便主机测试），AGENTS.md 依赖表补一行：
   - `KeySpec{name, type, owner, verb, encode, domain, persist, access}`（ADR-0018 §2.3），注册表 `constexpr` 数组；
   - `Service::set(key, value, Caller, Ticket&)`：查键 → 权限（`access` 位 + 工厂解锁状态）→ 取值域 → 编码 → `post`；
     错误映射到网页错误码（`unknown_key`/`bad_value`/`locked`/`busy`/`unsupported`）；
   - `Service::get(key)` 读 DataCenter 快照；`Service::domain(key)` 返回当前取值域；
   - `web_proto` 只保留 JSON 读写；`KeyTable.hpp` 的键定义迁入 settings（`web_proto` 引用 settings 的表或删除重复项）。
2. GNSS 能力：
   - 驱动接口增加 `navRatesHz()`、`outputPeriodsMs(OutputMsg)`（列出真实支持值，含 0 关闭与 5000 等低频），按型号返回；
     CM680/UM98x/u-blox/ComNav 的取值以各自手册为准，报告注明出处（CM680：`CM680系列高精度模组产品数据接口协议V1.2.8.pdf`）；
     手册未写明的值不列入，报告列出待确认项。
   - kIo 在识别型号后写 DataCenter `gnssCaps` Slot（含输出周期受当前定位频率限制后的有效集合），频率变化后更新。
   - 设置路径拒绝域外值（`bad_value`）；驱动对域外值的“向下取整”只保留在启动恢复 NVS 旧值时，并打 `FW_LOGW`。
3. 网页：
   - `WebLogic` 的 set 改走 `settings::Service`（Caller::kWeb），`config` 帧由注册表 `forEach` 生成；
   - `hello` 增加 `caps`（ADR-0018 §2.4）；`gnssCaps` 变化时推送 `{"t":"caps",…}`；`docs/web/ws-protocol.md` 增补；
   - `main/web/ui/app.js`：`SCHEMA` 中 select/seg 的选项改为来自 `caps`（显示文本由值格式化：周期 ms → Hz，5000 → "0.2 Hz"）；
     键不在 `caps` 中则隐藏；保留现有乐观更新/回滚/提示；`tools/web_mock/web_mock.py` 发送 `caps` 并支持 `--caps-cm680` 等样例；
   - 删除 `webGnssCommand`/`webGnssOutcome`/`webGnssCancelSeq` 与 `GnssModule::pollWebCommand`；
     `webGnssFlushGga*`（退出前门控落盘）改为一条 `post` 的维护命令。
4. 页面 gz 仍 ≤ 24 KB；Wi-Fi 模式最低堆 ≥ 40 KB。

## 3 SV3：手机、控制台、NONE 档

1. `PhoneCompatService`：0x06 星座、0x33 PPP、其余设置类报文改走 `settings::Service`（Caller::kPhone）；
   删除 `phonePpp*`/`phoneSat*` Slot 与对应轮询；0x06 查询应答按 `gnssCaps` 回答。
   Q1 门控的 Configure App 报文保留 `TODO(SV3/Q1)`（报文号待 App 团队）。
2. 控制台：`config set/get`、`gnss rate/out/gate` 等设置命令改为调用服务（Caller::kConsole），只保留参数解析与打印；
   输出文字保持不变（现有 HIL 脚本依赖）。
3. 门禁：`tools/ci/run_gates.sh` 增加 `RTK_CONSOLE_PROFILE_NONE` + `RTK_WIFI_CONFIG_ENABLE` 构建项；
   检查 web/phone 目标文件不引用 `cli::`/控制台符号（`nm` 或链接 map 检查，脚本放 `tools/ci/`）。
4. 删除旧通道后 `rg "phonePpp|phoneSat|webGnss" main components` 应无结果（报告贴命令输出）。

## 4 报告要求（每段）

- 改动文件清单、冻结文件清单（供重算）、门禁结果、内存对比；
- 迁移前后行为差异（若有）逐条说明；
- HIL 待 Architect 执行的检查项清单（网页每个设置、手机 0x06/0x33、控制台冒烟、NONE 档网页）。

## 5 不做

- 4G/NTRIP/MQTT 本身（另立 ADR）；`kNet` 执行器；RTCM 多来源仲裁。
- 网页协议 v2；手机协议改格式。
