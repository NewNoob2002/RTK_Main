# SV3 评审（omp，只读）

评审对象：分支 `feat/sv3-phone-console` 工作区未提交改动（`git diff HEAD`，HEAD `5915dd6`），
新增 `tools/ci/check_no_cli_deps.py`、`docs/reports/SV3.md`。
依据：`docs/tasks/SV-settings-service.md` §0/§3、`docs/adr/ADR-0018-settings-service.md`。
本评审**未修改任何代码**，只写了本文件。

## 0 我实际执行的验证

| 命令 | 结果 | 说明 |
| --- | --- | --- |
| `python3 -I tools/ci/check_no_cli_deps.py --build builds/sv3-none-wifi` | PASS | NONE+Wi-Fi ELF 无 `cli::fail/init/dispatch`、无 `app::*Cmds()` |
| `sha256sum -c docs/frozen.sha256` | 全部 OK | 冻结清单内 0 变更（见 P3-4 的清单覆盖问题） |
| `rg "phonePpp\|phoneSat\|webGnss" main components` | 0 命中 | 旧通道确实删净 |
| `stat builds/sv3-idf/rtk_main.bin`、`builds/sv3-none-wifi/rtk_main.bin` | 1,450,992 / 1,359,856 B | 与 `SV3.md` §5 完全一致 |
| `xtensa-esp32-elf-nm -C --defined-only` 两个 ELF | 见 P3-8/§7 | 核对 DEV/NONE 两侧的 cli 与命令表符号 |
| `pdftotext` CM680 V1.2.8 手册 + `rg` | 见 §6 | 核对 `pppSupported=false` 与 MASK/SBAS 依据 |
| `rg "TASK_NOTIFICATION_ARRAY_ENTRIES" sdkconfig` | `=1` | 单通知槽前提成立 |

未执行：`idf.py build`、host/ASan 测试、`clang-format`、`doc_check`、`g7b_check`、上板。`SV3.md` 的门禁结论按未复核处理。

## 1 结论总表（对应任务卡的 6 个重点）

| # | 重点 | 结论 |
| --- | --- | --- |
| 1 | executor 任务内不得调用 `rtos::Executor::call()` | **通过**。全仓库 `call(` 只有 `main/support/ConsoleSupport.cpp:105`（仅 REPL 任务）与 `main/modules/system/FactoryReset.cpp:37`（`freset`/`factory erase`，仅控制台）；`BtLinkModule`/`WebModule`/`StorageModule`/`GnssModule` 路径只有 `settings::Service::post/poll/cancel`（`BtLinkModule.cpp:141,153,247,291,352,402`；`WebModule.cpp:509,548,560`）。`Executor::call` 自身对 `owner == current` 返回 `kInvalidState`（`components/rtos/Executor.cpp:445-451`）兜底。 |
| 2 | 控制台输出文字与旧版一致 | **不完全一致**：成功路径文字全部相同（`reportOutputPlan`、`printGgaGateStatus`、`sn saved; reboot to apply` 均原样）；差异见 P2-1（探测未完成/维护窗口由成功变为 `ERR`）与 P3-3（`config` 帮助文本）。 |
| 3 | 0x06/0x33/0x24/0x08 异步延迟应答 | **未发现 ticket/credit 泄漏**；取消/断线/超时路径闭合，见 §5.2。应答时序新增 ~1 个执行器轮次（数 ms 量级），另见 P3-2 的 4 槽上限风险与 §5.3。 |
| 4 | `kSetHeight`/`kSetSnPn`、`configRev`、NVS 失败回滚 | **与旧行为等价**（回执早于落盘、失败仅重试不回滚，均为迁移前既有语义）；`configRev` 递增次数正确，见 §5 与 P3-7。 |
| 5 | CM680 `pppSupported=false` 依据 | **成立**，且与 HEAD 行为无差异（`gnss::supportsPpp(kCm680)` 早已为 false），见 §6。 |
| 6 | 新键权限与网页 `config` 帧过滤 | **通过**，见 §7。 |

---

## 2 P1

无。

## 3 P2

### P2-1 探测未完成/维护窗口的控制台 `gnss rate`/`gnss out` 由成功变为 ERR（违反任务卡 §3.2）

- 旧（HEAD）：`gnss rate`/`gnss out` 走 `dispatchStandardCmd` → `cmdSetRate`/`cmdSetOut`。
  探测未完成（state ∉ {kRunning, kMaintenance}）时进 not-running 分支（现文件仍在
  `main/modules/gnss/GnssModuleCmds.cpp:308-326`、`:352-372`），落 NVS、`apply_seq_++`、回 `kOk`，
  控制台打印输出计划并返回成功；state == kMaintenance 时走两阶段提交，同样回 `kOk`
  （随后 `submitGnssAndWait` 最长等 6 s 到 apply 完成，多数情况下打印计划并成功）。
- 新：`gnss rate`/`gnss out <single>` 先经 `settings::Service::set`，域检查要求 `caps.valid`
  （`components/settings/src/Registry.cpp:393-409`、`:414-437`）：
  - 探测未完成 → `kUnsupported` → `reportGnssSettingApply` 打印 `ERR gnss not ready`
    （`main/modules/gnss/GnssCmds.cpp:280-283`）；
  - 探测已完成但 state == kMaintenance → `cmdSetting` 的 `!= kRunning` 守卫回 `kBusy`
    （`main/modules/gnss/GnssModuleCmds.cpp:178-182`）→ 打印 `ERR gnss busy`。
- 影响：同一命令在两种旧版返回成功的窗口里现在返回 `ERR`（文本 `gnss not ready` / `gnss busy`），
  且开机探测窗口约 1–3 s。任务卡 §3.2 要求“输出文字保持不变（现有 HIL 脚本依赖）”，
  这里不只是文字变化，还有成功/失败翻转。
- 修复建议：二选一并写进 `SV3.md` §6/§7：
  1. 由 Architect 接受该文本变化，更新 HIL 期望；
  2. 在 `settings::Service::set` 中把“能力尚未发布（`gnssEnabled && !caps.valid`）”与“型号不支持”分开，
     前者映射 `kBusy`，控制台即恢复 `ERR gnss busy`，后者仍是 `ERR gnss not ready`。

### P2-2 `gnss rate` / `gnss out <single>` 失去“接收机未运行时先落 NVS”的能力

- 旧：`cmdSetRate`/`cmdSetOut` 的 not-running 分支保存 NVS、`apply_seq_++`、返回 `kOk`
  （`main/modules/gnss/GnssModuleCmds.cpp:308-326`、`:352-372`）。
- 新：服务路径必经 `cmdSetting`，该处在 `controller().state() != ::gnss::State::kRunning` 时直接
  `kBusy`（`main/modules/gnss/GnssModuleCmds.cpp:178-182`），not-running 分支在设置服务下不可达。
  因此“接收机还没起来时先把频率/输出存进 NVS，下次配置时生效”这条能力只剩 `gnss out all|default`（仍走 `kSetOut`）。
- 影响：产线/调试脚本若在探测前写频率，现在得到 `ERR`，旧固件得到 `OK` 且掉电重启后生效。
- 修复建议：这是设计取舍而非实现错误，需 Architect 在 ADR-0018 §2.4 上补一句明确语义。
  若需保留，可让 `cmdSetting` 在 `state != kRunning` 时沿用 `cmdSetRate`/`cmdSetOut` 自己的
  “存 NVS + 更新计划”分支（两函数已具备该分支，去掉外层 `!kRunning` 守卫即可），
  但这会削弱“能力域”校验，必须在 ADR 中写明。

## 4 P3

### P3-1 `0x06` Query 在“无星座能力”型号上由默认值变为全 0

`components/link_core/services/PhoneCompatService.cpp:699-712`：`capsMask == 0` 时直接返回全 0；
HEAD 版本无条件返回 `readSatConfig()`（默认 `15,1,1,1,1,0`）。
ZED-F9P / K803（`gnss::supportsSatMask == false`，`main/modules/gnss/GnssModuleProbe.cpp:344-349` 置 `constMask=0`）
上 Survey App 查询现在拿到 `cutoff=0, 全关`。
建议：在 `SV3.md` 与手机 HIL 清单中写明，并请 App 团队确认 0 值不会被误解为“关闭跟踪”。

### P3-2 延迟应答槽 4 条满时手机收不到任何回包

`components/link_core/include/core/LinkService.hpp:24` `kMaxDeferredReplies = 4`；
`LinkService::registerDeferred`（`components/link_core/LinkService.cpp:56-77`）在无空槽时释放 credit 并调
`hook.cancel`，**不回帧**。SV3 把 0x24/0x08 也变成延迟应答后，单会话可能同时有 0x33/0x06/0x24/0x08 四类在途，
第 5 条（或两个 SPP 会话并发时）只能靠 App 自己超时。
建议：`registerDeferred` 失败路径改为回一条 `02 02`（需要把 `DeferredCancelFn` 扩成可写 `Reply`，
或让 `PhoneCompatService` 在自己的 cancel 钩子里记录“已取消”，由链路层补发失败帧）；或把上限提到 6
（每项 ~40 B 静态 RAM，代价可忽略）。至少要在 `SV3.md` §8 记录。

### P3-3 `config` 帮助/usage 文本变化

`main/modules/storage/ConfigCmds.cpp:303-307`：帮助串由
`config [identity|link|gnss|debug|build]: show saved settings (read-only)` 改为含 `get <k>`/`set <k> <v>`；
`cmdConfig` 的 usage（`:270-271`）与 `cli::printUsage` 输出随之变化。分组查询输出未变。
建议：若有脚本/文档匹配帮助行，一并更新。

### P3-4 `components/settings/include/**` 不在冻结清单

AGENTS §4 规定 `components/*/include/**` 为冻结契约，但 `docs/frozen.sha256` 未收录 `components/settings/include/`
（`rg "settings/include" docs/frozen.sha256` 无结果），本次实际改了 `settings/Types.hpp`、`settings/Registry.hpp`
（+35/−9），`G1-frozen` 仍 PASS。
建议：由 Architect 决定是否把 settings 公共头补录进 `frozen.sha256`，否则“新增公共头”等于绕开冻结门禁。

### P3-5 `PhoneCompatOps::ctx` 语义不一致

`main/modules/bt_link/BtLinkModule.cpp:233`（`onReadHeight` 用 `self->context_`）与
`:172-215`（`phoneReadNtrip/phoneReadJsonServer/phoneReadThingsboard/phoneReadLatestGsv` 忽略 `ctx`、
直接用全局 `appContext()`）混用两种取上下文方式；同一张回调表里 `ctx` 有时必须是 `BtLinkModule *`、
有时被完全忽略。当前功能正确（全局单例），但这是“回调表自带上下文”契约的破坏，测试替身也容易踩坑。
建议：统一从 `self->context_` 取，删除 `static_cast<void>(ctx)` 的旁路。

### P3-6 `config set phone.sn_pn` 恒失败，报告未说明

`components/settings/src/Registry.cpp:807-808`：`phone.sn_pn` 权限 `kPhoneInternal`（phone+internal，无 console/web/mqtt），
所以控制台 `config set phone.sn_pn …` 返回 `ERR locked`（`main/modules/storage/ConfigCmds.cpp:246-255`），
只有手机 0x08 能写。若这是有意的（SN/PN 仅手机侧可改），请在 `SV3.md` §7 的 `config get/set` 清单里注明，
否则 HIL 会把它当成缺陷。

### P3-7 `kSetHeight` 的 `configRev` 与落盘时序（记录用，与旧版等价）

- `StorageModule::onCommand` 对成功结果统一 `configRev++`（`main/modules/storage/StorageModule.cpp:882-884`），
  而 `setPhoneBlob` 在值未变化时直接返回 true（`:489-505`），所以“改成相同值”也会让 `configRev` 前进一次。
  `phone.height`/`phone.sn_pn` 无网页消费者，目前无副作用。
- `kSetHeight` 回执在 NVS 落盘之前：`setPhoneHeight` 只置去抖标志，`pollPhoneBlob` 500 ms 后写、失败按
  `1 s→30 s` 退避重试（`:25`、`:507-526`）。手机已收到 `01 01`。旧版 `setPhoneHeight` 同样走这套去抖，
  **不是回归**，但 SV3 把它搬进了 `settings::Service`，语义应在报告里写明“回执≠已落盘”。
- `kSetSnPn` 先同步 `saveSn`（含失败回滚 `sn_`、`configRev++`），再置 PN 去抖；PN 置位失败时返回
  `kBusy` 而 SN 已生效（`main/modules/storage/StorageModule.cpp:789-830`）。与旧 `setPhoneSnPn` 的分步语义相同。

### P3-8 `check_no_cli_deps.py` 的 ELF 断言只覆盖访问器符号

`tools/ci/check_no_cli_deps.py:32-40` 的对象检查用 `nm -u`（只看未定义符号）；
`:45-60`、`:106-130` 的 ELF 断言匹配 `app::configCmds\(` 这类**访问器函数**。
实测 DEV ELF 同时存在 `T app::configCmds()`（能被匹配）与 `d app::(anonymous namespace)::kConfigCmds`（匹配不到）。
因此“无命令行表符号”这句只在访问器被链接时才成立。当前 NONE 构建两者都没有，结论有效；
但建议再加一路 `--defined-only` + `kConfigCmds`/`kGnssCmds` 正则或链接 map 检查，避免将来只保留对象表时漏检。

### P3-9 NONE+Wi-Fi 的配置源写进构建目录

`tools/ci/run_gates.sh:127-136` 用 `cat >builds/<prefix>-none-wifi/none_wifi.defaults` 现场生成 defaults，
仓库里没有对应的 `configs/none-wifi.defaults`（对照既有的 `configs/console-none.defaults`）。
构建可复现性依赖脚本本身，审计时看不到配置内容。
建议：落到 `configs/none-wifi.defaults`（新增文件不受冻结限制），`run_gates.sh` 引用它。

---

## 5 逐项核对细节（供 Architect 复核）

### 5.1 通知槽与 `Executor::call()`

- `sdkconfig:2824` `CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=1`：`BtLinkModule::wait` 的
  `ulTaskNotifyTake`（`main/modules/bt_link/BtLinkModule.cpp:754-771`）与 `Executor::call` 的 `waiterTask`
  共用单槽。当前树中任何执行器任务都不调用 `call()`（见 §1 第 1 行），前提成立。
- `wait()` 的 `fastPoll` 只看 `pending_reset_/ppp_ticket_/sat_ticket_`（`:764-766`），
  不含 `height_ticket_`/`snpn_ticket_`；后者靠 `poll()` 返回 `true`（`:810`）把执行器 `maxWait` 压到 0，
  行为正确但两条机制并存。建议将来合并成一处“在途凭据”判定，避免新增凭据类型时漏改（4G/kNet 会踩）。

### 5.2 0x06/0x33/0x24/0x08 的 ticket / credit 生命周期

| 事件 | ticket（`settings::Ticket` + 执行器 PendingSlot） | credit（`ReplyCredit`） |
| --- | --- | --- |
| 正常完成 | `pollTicket` 收到 `kDone` → `Executor::poll` 释放槽位并写结果；`ticket` 清零（`BtLinkModule.cpp:136-148`） | `sendReply` → `commitReply` |
| 会话断开 | `LinkService::cancelDeferred` → `onCancel*` → `Service::cancel` → `Executor::cancel`：`kRunning→kCancelled`（由模块后续 `complete()` 释放），`kDone` 立即释放，`kQueued` 在出队时释放（`components/rtos/Executor.cpp:368-399`、`:485-510`） | `transport_.cancelReply(credit)`（`LinkService.cpp:37-53`） |
| 控制台/总线超时 | `submitSetting` 超时 `service.cancel(ticket)`（`main/support/ConsoleSupport.cpp:180-186`），同上 | 不涉及 |
| 延迟槽满 | `registerDeferred` 失败 → `hook.cancel` → 同上 | 先 `cancelReply`（`LinkService.cpp:56-77`） |
| 重入（同类在途） | `pppWaiting/satWaiting/heightWaiting/snPnWaiting` 直接回 `02 02`，不 post | 不占 |
| 模块中止（重探/复位） | `abortPendingSettings()` 以 `kInvalidState` 结单（`main/modules/gnss/GnssModuleCmds.cpp:241-267`） | 由 poll 正常回收 |

模块侧不会漏结单：`cmdApplyPpp`/`cmdApplySatTrack` 被 `onCommand` 的
`ppp_seq_.active() || sat_seq_.active()` 守卫挡住（`main/modules/gnss/GnssModuleCmds.cpp:114-122`），
脚本每一步 ≤ `kReplyMs=1000 ms` 且超时即中止（`components/gnss/private_include/PppSequence.hpp:35,89`），
`finishPpp/finishSatTrack` 必调 `completeSetting`（`GnssModulePpp.cpp:146-181`）。
因此不存在“kCancelled 槽位永远不释放”的路径——**前提是模块不停在脚本中间**；`poll()`
在脚本活跃时提前返回、不推进 `pollController`（`main/modules/gnss/GnssModule.cpp:293-300`），
这一点与旧版一致。

### 5.3 应答时序 vs 手机 App 超时

- 0x24/0x08（kLink 同执行器内 kStorage）：请求 → post → 下一轮出队执行 → 同轮 `pollDeferred` 回帧，
  约 1–2 个执行器轮次；若请求来自其他任务且 kLink 正在 `wait()`，最坏再加 `idleWaitMs=20 ms`
  （`main/app/Executors.cpp:100-113`）。比旧版“帧处理器内同步回包”慢数毫秒，远小于任何合理 App 超时。
- 0x33/0x06（kIo 脚本）：与旧版同一套 `PppSequence`/`SatMaskSequence`，回包由 `pollDeferred` 在 kLink 每轮调用
  （`BtLinkModule.cpp:784-790`，`kTickPeriodMs=100 ms` 只影响 `tick()`，不影响 `pollDeferred`），
  不引入额外等待。
- 手机 App 侧的具体超时值仓库内无记录（`docs/protocol/phone_protocol.md` 未写），**无法核对**。

## 6 CM680 `pppSupported=false` 的依据复核（V1.2.8 手册）

用 `pdftotext -layout` 提取 `/home/gtc/Desktop/workspace/ESP32_PROJ/P2 手持式GNSS接收机项目/CM680系列高精度模组产品数据接口协议V1.2.8.pdf` 后核对：

- §4.9（p.27–28，表 4-14）：`CONFIG SBAS Disable` / `CONFIG SBAS Enable Auto|WAAS|…`，与
  `components/gnss/src/PppSequence.cpp:134-135` 发送的 `CONFIG SBAS ENABLE AUTO` / `CONFIG SBAS DISABLE` 一致。
- §5.1–5.3、表 5-2（p.30–32）：`MASK [高度角(可选)]`，高度角范围 **-90°~90°**；系统名 GPS/BDS/GLO/GAL/QZSS；
  与 `MASK <n>` / `MASK|UNMASK <SYS>`（`components/gnss/src/PppSequence.cpp:123-136`）一致。
- 全文只有 1 处 “PPP”（p.14 段落里的“RTK、SPP、PPP 等算法引擎”叙述），**没有 `CONFIG PPP` 指令**。
  因此 `supportsPpp(kCm680)=false`（`components/gnss/src/PppSequence.cpp:26-38`）成立，
  `publishCaps()` 置 `pppSupported=false`、`constMask=0x1F`（SBAS 由 `CONFIG SBAS` 承载，属“可配置”而非 `MASK` 系统）成立。
- **与迁移前的差异：无。** HEAD 的 `phonePppSupported` 也是 `gnss::supportsPpp(model)`，
  CM680 一直回 `0`/`02 02`。SV3 的变化只是把该能力搬到 `gnssCaps`，并让 `gnss.ppp` 成为注册键
  （`config get gnss.ppp` 在 CM680 上恒为 0，`config set` 返回 `ERR unsupported`）。
- 待确认（不在本卡范围）：`outputPeriodsMs` 的 CM680 取值出处属 SV2，未在本次复核。

## 7 权限与网页 `config` 帧

| 键 | access 常量 | 允许 | 说明 |
| --- | --- | --- | --- |
| `gnss.ppp` | `kPhoneAndLocal`=0x0D | console/phone/internal | `Registry.cpp:799-800` |
| `gnss.sat_track` | `kPhoneAndLocal` | console/phone/internal | `Registry.cpp:801-803` |
| `phone.height` | `kPhoneAndLocal` | console/phone/internal | `Registry.cpp:804-806` |
| `phone.sn_pn` | `kPhoneInternal`=0x0C | phone/internal | `Registry.cpp:807-808`，见 P3-6 |

- 四键均不含 `access::kMqtt`（0x10），与 ADR-0018 §6 Q2 一致；`test/settings/TestSettingsService.cpp` 的新用例
  也断言了 web 写入返回 `kLocked`。
- 网页 `config` 帧新增过滤 `(spec.access & settings::access::kWeb) == 0U → 跳过`
  （`main/modules/web/WebLogic.cpp:290`）；`hello.caps`/`caps` 帧此前已有同样过滤（`:221`，且额外要求 `encode != nullptr`）。
  实测 NONE+Wi-Fi ELF 同时含 `WebModule`/`PhoneCompatService`/`settings::Service`，人工核对
  `config` 生成路径不会输出 `gnss.ppp`/`gnss.sat_track`/`phone.*`。
- `writeSetting` 不支持 `ValueKind::kBytes`（`main/modules/web/WebLogic.cpp:180-192`），
  手机专用 `kBytes` 键一旦被放进 `config` 帧会导致整帧构造失败（`return 0`）。当前靠 access 过滤保证不会发生，
  建议在 `writeSetting` 的 `default` 分支之外再加一条 `kBytes → return false` 的显式注释或断言，
  避免将来放宽 access 时静默丢整帧。

## 8 未完成/无法核对

- 上板 HIL（控制台冒烟、手机 0x06/0x33/0x24/0x08、NONE+Wi-Fi 网页配置）未做，按任务卡由 Architect 执行。
- `SV3.md` 的 40 项门禁未重跑（只重跑了 `G7c`、`G1`、遗留标识符、bin 大小核对）。
- 手机 App 侧应答超时值、UM98x 手册中的 `CONFIG SIGNALGROUP`/PPP 支持，仓库内无依据，未核对。
