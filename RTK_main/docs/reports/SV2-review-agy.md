# SV2 交叉评审报告（只读评审）

- **评审对象**：分支 `feat/sv-settings` 未提交改动（基线已验收 SV1 提交 `1105c35`），实现报告 `docs/reports/SV2.md`
- **依据文档**：`docs/tasks/SV-settings-service.md`（§0、§2）、`docs/adr/ADR-0018-settings-service.md`、`docs/web/ws-protocol.md`、`AGENTS.md`、`CM680系列高精度模组产品数据接口协议V1.2.8.pdf`
- **评审结论**：**有条件通过（建议修复 P1 #1 并优先处理 P2 #2/#3/#4/#5 后合入或作为 SV2 修订提交）**。整体分层清晰、`components/settings` 纯净无堆且不依赖 IDF/FreeRTOS、执行器任务内零 `call()`、存储模块失败回滚完整、门禁除授权冻结项外全部通过；但存在 1 项 P1 级状态一致性问题（`gnss.gga_gate.*` 写盘失败未回滚 RAM/DataCenter）、4 项 P2 级设计与边界问题（含 615 ms 保存耗时根因）以及若干 P3 级文档/细节项。

---

## 1. 按严重度排列的发现

### P1（高）

1. **`gnss.gga_gate.*` NVS 持久化失败时不回滚 RAM 与 DataCenter，设备行为与网页回滚状态永久脱节**
   - **位置**：`main/modules/gnss/GnssModuleCmds.cpp:171-184`、`:446-454`、`:488-497`；`main/web/ui/app.js:568-573`
   - **问题**：
     - `kApplyGgaGate` 进入 `cmdSetGgaGate()` 后，立即将新配置写入 `gga_gate_cfg_`、`gga_gate_` 并发布 `DataCenter::ggaGateStatus` 和 `ggaAllowedFlag`（`:446-449`），再将凭据 `gateTicketSeq_` 挂起 500 ms 等待 `tickGgaGateSave()` 落盘。
     - 若 500 ms 后 `ctx().storage.saveKeys()` 失败（`:488-497`），模块调用 `completeSetting(gateTicketSeq_, res)` 向网页返回错误，但**未回滚 `gga_gate_cfg_`、`gga_gate_` 与 `ggaGateStatus`**，且保留 `gga_gate_save_pending_ = true` 在 1 s 后后台静默重试。
     - 浏览器收到 `ack {ok:false, err:"failed"}` 后丢弃乐观值并将控件恢复旧值，造成“网页显示已回滚、设备实际按新配置阻断/放行 GGA”；且 1 s 后若后台重试成功会递增 `configRev`，页面会突然跳回刚才报错失败的值。对比 `GnssModuleProbe.cpp:225-236`（`rate`/`out` 失败回滚）与 `StorageModule.cpp:651,705,741`（失败保留旧值），仅 GGA 门控破坏了“返回失败 = 保持旧值”契约。
   - **建议**：`kApplyGgaGate` 改为直接同步写本次变更的键（取消网页路径的 500 ms 去抖，详见 §3）或缓存变更前 `prev_gga_gate_cfg_`；首次写 NVS 失败即回滚 `gga_gate_cfg_`、`gga_gate_`、`ggaGateStatus`/`ggaAllowedFlag` 并清 `gga_gate_save_pending_`（同时保留控制台 `kSetGgaGate` 原有去抖行为或统一语义）。

### P2（中）

2. **网页设置往返耗时中位数 ~615 ms 的代码根因：`kApplyGgaGate` 固定等待 500 ms 去抖；`kPatchOutput` 单项改动重发全部 5 条 NMEA 且每条短应答被 `uart_read_bytes(512, 20ms)` 拖满 20 ms**
   - **位置**：`main/modules/gnss/GnssModuleCmds.cpp:171-184`、`:211-246`、`:457-498`；`main/modules/gnss/GnssModule.cpp:263-290`；`main/modules/gnss/GnssModuleProbe.cpp:407-420`；`components/gnss/src/ControllerConfigure.cpp:191-233`
   - **问题**：详细拆解见 §3。核心在于：
     1. `kApplyGgaGate` 对网页单在途请求强制套用 `kGgaGateDebounceMs = 500U`（且 `app.js:282` 的 stepper 本身已有 700 ms 防抖），再做 2 次 `nvs_commit`，导致门控设置固定 ≥ 550–620 ms；
     2. 修改单条 `gnss.out.*` 时 `cmdPatchOutput()` 仍通过 `cmdSetOut()` → `requestOutput(plan)` 串行重发 CM680 全部 5 条 NMEA（`gnss.rate` 则发 `UNLOGALL` + `MODE ROVER` + 5 条输出共 7 条），而 `GnssModule::wait(20 ms)` 中 `uart_read_bytes(port, buf, 512, 20ms)` 对约 40 B 的 `$command` ACK 必然等满 20 ms 超时，叠加 CM680 应答处理、同步 `nvs_commit` 和 `kLink` 20 ms 轮询周期，推高整体保存往返。
   - **建议**：
     - `kApplyGgaGate` 在网页路径跳过去抖立即落盘，且按 `cmd.args[0]` 标志位只写变更的那 1 个 NVS 键（而非每次固定写 2 个键、提交 2 次）；
     - 后续优化 `kPatchOutput` 仅向接收机下发变更的那 1 条输出命令；在 `kConfiguring`/`kOutput` 期间缩短或事件驱动 UART 读等待，避免每条短 ACK 固定浪费 20 ms。

3. **`SettingsBackend::getSetting()` 对两个同为 `KeyKind::kBool` 的键返回不同 `ValueKind`，与单测桩不符，存在 SV3 误用隐患**
   - **位置**：`main/app/SettingsBackend.cpp:141-157`、`components/settings/src/Registry.cpp:536,556`、`test/settings/TestSettingsService.cpp:98-100`
   - **问题**：注册表中 `gnss.gga_gate.enabled` 与 `bt.pair_next` 均为 `KeyKind::kBool`，但 `SettingsBackend::getSetting()` 对前者返回 `ValueKind::kBool`（`.boolVal`），对后者返回默认 `ValueKind::kInt`（`.number = 0/1`），目的是迁就 v1 `config` 帧中 `"bt.pair_next":0/1` 的线格式。而在 `TestSettingsService.cpp:98-100` 中却模拟 `kBtPairNext` 返回 `ValueKind::kBool` 并断言 `v.boolVal`。虽然 SV2 网页编码结果正确，但 SV3 控制台/手机调用 `Service::get("bt.pair_next")` 若按 `KeyKind::kBool` 读 `boolVal` 会读到恒 `false`。
   - **建议**：`SettingsBackend::getSetting()` 对所有 `KeyKind::kBool` 统一返回 `ValueKind::kBool`（`.boolVal`）；将 `"bt.pair_next"` 按 0/1 整数编码的 v1 线格式兼容规则显式放在 `WebLogic.cpp:buildConfigFrame()` 中，并在 `WebServerCheck.cpp` 补充 `bt.pair_next` 序列化断言。

4. **`Cm680Receiver::outputPeriodsMs()` 对不支持的 `OutputMsg::kSatsInfo` 返回有效周期，与 `outputMask()` / `buildOutput()` 矛盾**
   - **位置**：`components/gnss/src/drivers/UnicoreReceiver.cpp:281-283`、`:312-314`、`:379-381`；`main/modules/gnss/GnssModuleProbe.cpp:318`；`main/modules/gnss/GnssModuleCmds.cpp:221-223`
   - **问题**：`UnicoreReceiver::outputMask(Model::kCm680)` 显式排除 `OutputMsg::kSatsInfo`，`buildOutput(kSatsInfo, ..., kCm680)` 返回 `kNotSupported`（CM680 不支持 `#SATSINFOA`）。但 `Cm680Receiver::outputPeriodsMs()`（`:379-381`）为迁就 `main` 侧 `GnssOutConfig` 用下标 1（`kSatsInfo`）复用存 GSV 周期，把 `kSatsInfo` 和 `kGsv` 一起返回 `{0, 1000, 5000}`，破坏了 `gnss::Receiver` 公共接口自洽性。
   - **建议**：`Cm680Receiver::outputPeriodsMs()` 仅在 `OutputMsg::kGsv` 返回 `{0, 1000, 5000}`，`OutputMsg::kSatsInfo` 返回空 `{}`；在 `main` 侧（`GnssModuleProbe.cpp:318`、`GnssModuleCmds.cpp:223`、`GnssModuleProbe.cpp:370`）把 GSV 键或 `kSatsInfo` 槽位（当驱动 `outputMask` 不含 `kSatsInfo` 时）映射到 `OutputMsg::kGsv` 再查询驱动。

5. **`GnssModule` 延迟维护凭据 `maintenanceTicketSeq_` 在不支持 `saveStep()` 的驱动与后台自动保存交错时可能漏回执；`cmdProbe`/`cmdReset` 未中断并结单在途凭据**
   - **位置**：`main/modules/gnss/GnssModuleMaintenance.cpp:127-154`、`:341-357`、`:400-404`；`main/modules/gnss/GnssModuleCmds.cpp:145-152`、`:256-280`
   - **问题**：
     1. 若 `tickMaintenance()` 发起 `auto_save_in_flight_ = controller().requestSave()` 时驱动不支持 `saveStep()`，`requestSave()` 当场调 `finishMaintenance(kUnsupported)` 使状态保持 `kRunning` 但 `maintenanceSeq` 尚未被 `completeMaintenance()` 消费；下一轮 `Executor::run()` 先执行 `processCommand()`，若此时进来 `kSaveReceiver`，会通过 `state() == kRunning` 检查并登记 `maintenanceTicketSeq_`，随后的 `pollController()` → `completeMaintenance()` 命中 `if (auto_save_in_flight_)`（`:341`）而跳过 `:354-356`，导致该凭据永不 `complete()` 直至 6 s 超时。
     2. `cmdMaintenance()` 未判忙 `maintenanceTicketSeq_ != 0U`；控制台 `gnss probe` / `gnss reset` 在 `kSaveReceiver` 或 `pending_apply_` 在途期间不被 `:145` 拦截，重置控制器后将使网页 `kSaveReceiver` 挂满 6 s 超时、或 `pending_apply_` 5 s 超时后反写覆盖新状态机的输出计划。
   - **建议**：`cmdMaintenance()` 入口增加 `auto_save_in_flight_ || boot_baud_save_in_flight_ || maintenanceTicketSeq_ != 0U` 判忙；`cmdProbe()`/`cmdReset()` 若存在未结单的 `applyTicketSeq_`/`maintenanceTicketSeq_`，主动以 `fw::Result::kInvalidState` 调用 `completeSetting()` 并清理 `pending_apply_`。

### P3（低）

6. **CM680 协议手册表号引用错误：代码注释与报告均写“表 2-1”，实为“表 1-1”**
   - **位置**：`components/gnss/src/drivers/UnicoreReceiver.cpp:355`、`docs/reports/SV2.md:29-30`
   - **问题**：核对 `/home/gtc/Desktop/workspace/ESP32_PROJ/P2 手持式GNSS接收机项目/CM680系列高精度模组产品数据接口协议V1.2.8.pdf`，`log com1 gpgga ontime 1` 及“1、0.5、0.2、0.1 分别对应输出频率 1Hz、2Hz、5Hz、10Hz”与 `unlog` 记载在 **表 1-1 常用指令集**（第 9 页）；**表 2-1 高精度定位模组指令分类**（第 12 页）仅列指令分类名，不含频率与周期值。数值本身正确，仅出处编号写错。
   - **建议**：将两处“表 2-1”修正为“表 1-1”。

7. **`restoreOutputPeriods()` 在低频重启时覆写已保存的高频输出偏好，且对超出表上限的历史周期直接关闭语句**
   - **位置**：`main/modules/gnss/GnssModuleProbe.cpp:360-392`、`docs/reports/SV2.md:34`
   - **问题**：在 10 Hz 下把 `gnss.out.gga` 设为 200 ms，随后降频到 1 Hz（此时未重启，RAM/NVS 保留 200 ms，`makePlan()` 动态钳制为 1000 ms）；若在 1 Hz 下重启，首次探测触发 `restoreOutputPeriods(1)`，因 `200 < minPeriod(1000)` 直接把 RAM 的 `out_cfg_` 改写为 1000 ms，与 `SV2.md:34`“保留已保存的输出偏好”在跨重启后不完全一致。另若历史 NVS 存有 2000 ms 的 GGA，`lower_bound({0,100,200,500,1000}, 2000)` 返回 `end()` 会直接关闭 GGA（0 ms）。
   - **建议**：`restoreOutputPeriods()` 仅修正不在驱动周期表内的非法周期，受当前定位频率约束的钳制交由 `makePlan()` 处理；对高于最大表项的 GGA 历史值可回落到最慢支持档（1000 ms）而非直接关断。

8. **`app.js` 控件微细节：`W.stepper` 未动态消费 `caps` 上下界、`showVal` 对非整除周期会显示长小数**
   - **位置**：`main/web/ui/app.js:159`、`:211`、`:264-290`
   - **问题**：`W.stepper` 校验与 `min`/`max` 属性仍读 `SCHEMA` 静态值 `0`/`40`，未读取 `caps["gnss.gga_gate.min_sats"]`（当前两端同为 0..40 无表现差异）；`showVal` 的 `1000 / Number(v) + " Hz"` 对 CM680 现有值（100/200/500/1000/5000）无误差，但未来若有 3000 ms 等周期会输出 `0.3333333333333333 Hz`。
   - **建议**：`W.stepper` 优先读 `caps[f.key]` 的 `min`/`max`；`showVal` 对非整数 Hz 做两位小数截断。

9. **防御性与文档小项**
   - `main/modules/gnss/GnssModuleCmds.cpp:225`：`kMsPerSecond / ensureRoverCfg().rateHz` 建议同 `GnssModuleProbe.cpp:320` 加 `std::max(..., uint8_t{1U})` 防除零。
   - `main/modules/web/WebModule.cpp:594-603`：若未来 `buildCapsFrame()` 超 1024 B 返回 0，`pollCapsPush()` 会每轮返回 `more = true` 使 `kLink` 退化为 1 ms 轮询（当前 CM680 `caps` 约 400 B、`hello` 约 520 B，余量充足）。
   - `components/gnss/src/drivers/UnicoreReceiver.cpp:110-111`：`caps(Model::kCm680)` 仍置位 `cap::kRate20Hz`，与 `navRatesHz()` 不含 20 Hz 略有分歧（`SV2.md:29` 已说明为兼容既有单测保留）。
   - `main/modules/storage/StorageModule.cpp:577`：`setPhoneSnPn()` 直接调 `saveSn()` 绕过 `onCommand()`，未递增 `configRev`（蓝牙与 Wi-Fi 互斥，无现网影响，SV3 迁移手机通道后消除）。
   - `AGENTS.md:59`：纯组件括号清单未同步补入 `settings`（表格第 54 行已补，Architect 重算冻结清单时可顺带评估）。

---

## 2. 六项重点逐项核查结果

| # | 核查项 | 结果 | 核查证据 |
|---|---|---|---|
| 1 | 执行器任务内部禁止 `call()`（`CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES=1`） | **PASS** | `sdkconfig:2838` 确认为 `1`。全仓搜索 `call(` 仅见于控制台任务 `main/support/ConsoleSupport.cpp:101` 与 `main/modules/system/FactoryReset.cpp:37`。逐一检查 `main/modules/web/WebModule.cpp`（`:474,483,498,509,533,548,560`）、`main/app/SettingsBackend.cpp`（`:69,86,208`）、`main/modules/storage/StorageModule.cpp`（`:762-793`）、`main/modules/gnss/GnssModule*.cpp`（`:104-254`）：`kLink` 与 `kIo` 内部全部走 `post`/`poll`/`cancel`/`complete`，零 `call()`。 |
| 2 | `components/settings` 纯净性（无 IDF/FreeRTOS 头、无堆、静态存储、与 `AGENTS.md` 依赖行一致） | **PASS** | `components/settings/{include,src}/*` 仅包含标准 C++20 头与 `fw/Command.hpp`、`fw/Result.hpp`；`kRegistry` 为 `constexpr std::array<KeySpec, 16>`，全部类型定长无堆；`CMakeLists.txt:5` 为 `REQUIRES app_fw`，与 `AGENTS.md:54` 完全一致（比任务卡最初设想的 `app_fw + rtos` 更解耦，经 `Backend` 钩子隔离 `rtos`）；G7b 门禁 0 堆符号违规。 |
| 3 | CM680 `navRatesHz`/`outputPeriodsMs` 与协议 V1.2.8 一致性，非 CM680 返回空 | **PASS（附 P2 #4、P3 #6）** | 实查 PDF：`navRatesHz={1,2,5,10}` 与 GGA/GST/RMC `{0,100,200,500,1000}` 对应表 1-1（p.9）与 §6.2；PDF 未记载 20 Hz/`0.05` 输出（`0.05` 仅见于 §7 INS 标准差）；GSA `{0,1000}` 与 GSV `{0,1000,5000}` 结合 §6.2 与实板/ADR-0018 记录，无猜测值。基类 `Receiver.cpp:13-22` 默认返回空 `{}`，UM980/UM981/UM982/ZED-F9P/K803 均未覆盖，正确返回空（待确认）。存在表号误引（P3 #6）及 `kSatsInfo` 抽象泄漏（P2 #4）。 |
| 4 | 删除 `web_proto` KeyTable 后前后端一致、异步回执、超时、取消、并发保存 | **需修 P1 #1、P2 #3/#5** | 已核对 `docs/web/ws-protocol.md`、`Writer.{hpp,cpp}`、`WebLogic.{hpp,cpp}`、`WebModule.{hpp,cpp}`、`main/web/ui/app.js`、`tools/web_mock/web_mock.py`：`KeyTable` 已彻底删除（无残留引用）；`hello.caps` / `t:"caps"` / `config` / `ack` 前后端格式一致，旧页面向后兼容；单 `inflight_` 槽对并发请求回 `busy`、超时调 `cancel`、客户端断连按 `(fd, gen)` 隔离、TX 背压经 `pendingAck`/`pendingConfig` 与 `hasPendingCtrlBeforeRx()` 保序不丢。需关注 P1 #1（GGA 门控失败未回滚）、P2 #3（`bt.pair_next` `ValueKind`）、P2 #5（维护凭据交错/中断）。 |
| 5 | `StorageModule` 持久化失败与回滚路径 | **PASS** | 逐一核对：`saveSn()`（`:621-656`）先备 `previous`，NVS 写失败恢复 `sn_`、记 E 日志、不更新 `settingsSn` 且 `onCommand` 不递增 `configRev`；`editWhitelistCmd()`（`:677-720`）先备 `previous`，失败恢复 `whitelist_`、不写 `linkStatus`、不增 `configRev`；`setPairNextCmd()`（`:722-750`）与 `consumeBtPairNext()`（`:462-483`）均先写 NVS 成功后再改 RAM 与 `storeStatus`；`eraseForReset()`（`:795-816`）经 `store::closeWritesForErase()` 排他，失败调用 `store::reopenWrites()` 恢复写入许可。 |
| 6 | 实测网页设置保存往返中位数 ~615 ms 耗时来源查明 | **已定位（见 §3）** | 拆解三类设置路径：GGA 门控含固定 500 ms 去抖 + 2 次 `nvs_commit`；GNSS 频率/输出含 5–7 条串行命令 × `uart_read_bytes(512, 20ms)` 满窗超时等待 + 接收机处理 + 同步 `nvs_commit`；存储键仅 ~30–80 ms。 |

---

## 3. 实测网页设置保存往返中位数 ~615 ms 耗时来源拆解

按当前实现（`CONFIG_FREERTOS_HZ = 1000`，`kLink`/`kIo` 的 `idleWaitMs = 20 ms`、`yieldTicks = 1 ms`），三类设置键的端到端往返路径差异很大，**~615 ms 主要来自 `gnss.gga_gate.*` 的 500 ms 固定去抖等待，以及 `gnss.rate` / `gnss.out.*` 的多条串行 GNSS 命令应答等待 + `uart_read_bytes` 满窗阻塞 + 同步 NVS 提交**：

### 路径 A：`gnss.gga_gate.enabled` / `gnss.gga_gate.min_sats`（典型 **570–650 ms**）
1. **浏览器 → httpd → `kLink` 入队**：`WebModule::poll()` 在 `kLink` 空闲时每轮 `vTaskDelay(20 ms)` + `yieldTicks(1 ms)`（`Executors.cpp:116`、`Executor.cpp:611`），平均等待 **~10–21 ms** 后将 `kApplyGgaGate` 投递给 `kIo`。
2. **`kIo` 唤醒取命令**：`kIo` 阻塞在 `GnssModule::wait(20 ms)` → `uart_read_bytes(..., 512, 20 ms)`，平均等待 **~10–21 ms** 后执行 `cmdSetting()`，将 `gateTicketSeq_` 设为延迟完成（`kDetailDeferred`）。
3. **固定 500 ms 去抖等待（最大头）**：`GnssModuleCmds.cpp:459-468` 中 `tickGgaGateSave(nowMs, false)` 强制等待 **`kGgaGateDebounceMs = 500U` ms** 到期才开始写 NVS，到期对齐到下一轮 `kIo` `poll()`（**500–521 ms**）。
4. **2 次同步 NVS 写盘与提交**：`GnssModuleCmds.cpp:480-481` 固定调用 `saveKeys` 写 `gga_gate` 和 `gga_min_sats` 两个键，`StoreKeys.cpp:127-157` 对每个键各自 `nvs_open` + `nvs_set_u8` + `nvs_commit` + `nvs_close`。ESP32 内部 Flash 写会短暂禁用双核 cache，2 次提交约 **20–60 ms**（触发扇区整理时更高）。
5. **`kIo` 结单 → `kLink` 轮询 → WS 发回**：`WebModule::poll()` 在 `inflight_` 在途期间返回 `false`（不忙等），再经 1 轮 `kLink` 20 ms 周期（平均 **~10–21 ms**）+ `httpd_queue_work` + SoftAP Wi-Fi/TCP（**~10–30 ms**）返回 `ack`。
- **合计**：`~15 + ~15 + 510 + ~40 + ~35 ≈ 615 ms`（与实测中位数高度吻合）。

### 路径 B：`gnss.rate` 与 `gnss.out.*`（典型 **250–650 ms+**）
1. **入队与唤醒**：`kLink` + `kIo` 各约 **10–21 ms**。
2. **GNSS 串行配置应答等待（主要大头）**：
   - `gnss.out.*` 即使只改 1 个键，`cmdPatchOutput()`（`GnssModuleCmds.cpp:245`）仍走 `cmdSetOut()` → `controller().requestOutput(plan)`（`output_rerun_ = true`），串行下发 CM680 全部 **5 条** NMEA 命令（`GPGGA`/`GPGST`/`GPRMC`/`GPGSA`/`GPGSV`）；
   - `gnss.rate` 走 `requestReconfigure()`，先发 `UNLOGALL COM1` + `MODE ROVER`，再发最多 **5 条**输出，共 **7 条**串行命令；
   - 每条命令下发后 `GnssModule::poll()` 返回 `false`，`kIo` 进入 `GnssModule::wait(20 ms)` 调用 `uart_read_bytes(port, buf, 512, pdMS_TO_TICKS(20))`（`GnssModule.cpp:271`）。由于 CM680 的 `$command,...,response: OK*xx\r\n` 仅 ~40 B（远小于 512 B），**`uart_read_bytes` 每次必等满整个 20 ms 超时**（若接收机应答稍晚跨界则变成 42 ms），叠加 CM680 自身处理 `UNLOGALL`/`MODE ROVER`/多条 `LOG` 命令的响应耗时（尤其高频输出切换时可达每步 20–80 ms），5–7 步累计约 **150–500+ ms**。
3. **同步 NVS 写盘**：`tickPendingApply()`（`GnssModuleProbe.cpp:221`）在 `outputSeq` 推进后同步写 1 个 NVS 键（`rate` 或 `out` blob）并 `nvs_commit`，约 **10–40 ms**。
4. **回执回传**：`kLink` 下轮轮询 + `httpd` + Wi-Fi 约 **20–50 ms**。
- **合计**：主要取决于 GNSS 配置串行步数 ×（`uart_read_bytes` 20 ms 满窗 + CM680 应答时延）+ 同步 `nvs_commit`，在 `gnss.rate` 或应答跨帧时可达 **300–650 ms**。

### 路径 C：`bt.pair_next` / `bt.wl.add` / `bt.wl.del` / `factory.sn`（典型 **30–80 ms**）
- 归属 `kLink` 上的 `StorageModule`：`WebModule` 自投递 `kLink` 后等待 1 轮 `idleWaitMs + yieldTicks`（21 ms），下轮同步 `nvs_commit`（~10–40 ms）并在同轮 `WebModule::poll()` 立即发 `ack`。此路径不是 615 ms 的主因。

---

## 4. 评审方独立验证记录

执行环境：`IDF_PATH=$HOME/esp/esp-idf-v6.1`、`IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`、`IDF_COMPONENT_MANAGER=0`。

| 检查命令 | 结果 | 说明 |
|---|---|---|
| `tools/ci/run_gates.sh --base 1105c35 --prefix sv2r` | **37 PASS / 1 FAIL** | 唯一 FAIL 为 `G1-frozen`（`builds/sv2r-logs/G1-frozen.log` 仅报 `AGENTS.md`、`components/gnss/include/gnss/Receiver.hpp`、`components/gnss/include/gnss/drivers/UnicoreReceiver.hpp`，属任务卡授权，由 Architect 重算）。主机普通+ASan（含 `test/settings`、`test/web_proto`、`test/gnss` 全驱动、`main_host_check`）、6 档 IDF 构建（含 `console-none`、`rtk_pure` esp32/esp32c3）、`G5-format`、`G6-doc_check`、`G5b-tidy`、`G7b-runtime` 全部 PASS。`rtk_main.bin` = 1,536,400 B。 |
| `python3 tools/web_mock/web_mock.py --selftest` 及 `--caps-cm680` | **PASS** | 两组回环 HTTP/WS 自测均通过。 |
| 嵌入页面大小核验（`builds/sv2r-idf/esp-idf/main/index.html.gz`） | **PASS** | 13,594 B ≤ 24 KB 上限。 |
| `node --check main/web/ui/app.js` | **NOT_RUN** | 本机 PATH 无 `node`，已人工逐行审读 `app.js` 语法与协议交互。 |
| 实机烧录 / 串口 / HIL | **NOT_RUN** | 只读评审边界禁止碰串口与烧录。 |
