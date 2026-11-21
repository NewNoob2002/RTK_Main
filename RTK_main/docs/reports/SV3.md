# SV3 手机、控制台接入设置服务与 NONE+Wi-Fi 档位

分支 `feat/sv3-phone-console`，基线 `main` `5915dd6`（含 SV1+SV2+H1），依据 `docs/adr/ADR-0018-settings-service.md` 与 `docs/tasks/SV-settings-service.md` §0/§3 实施。

## 1. 改动清单

| 文件 | 改动 |
| --- | --- |
| `components/settings/include/settings/{Types,Registry}.hpp`、`components/settings/src/{Registry,Service}.cpp` | 注册表扩至 20 键：新增 `gnss.ppp`、`gnss.sat_track`、`phone.height`、`phone.sn_pn`（`kPhoneAndLocal` / `kPhoneInternal` 权限）；`GnssCaps` 新增 `pppSupported` 与 `const_bit::kSbas`；`Domain` 新增 `mask`；`ValueKind` 新增 `kBytes`；`CanonicalValue::bytes` / `Value::text` 扩至 `fw::kCommandArgBytes`（32 B）；`Service::set()` 在 `gnssEnabled && !caps.valid` 时对 `gnss.rate`/`gnss.out.*` 仅做静态合法范围校验（`domain()` 仍保持 `supported=false` 供网页隐藏 caps）。 |
| `components/link_core/include/core/LinkService.hpp`、`components/link_core/private_include/PhoneCompatService.hpp`、`components/link_core/services/PhoneCompatService.cpp` | `kMaxDeferredReplies` 由 4 增至 6（P3-2）；`0x24` 天线高与 `0x08` Nav SN/PN 改为异步延迟应答（`startHeight`/`heightResult`/`cancelHeight`、`startSnPn`/`snPnResult`/`cancelSnPn`），`0x33` PPP 与 `0x06` Survey 补齐 `cancelPpp`/`cancelSatConfig`；`0x06` Survey 在 `capsMask!=0` 时按位遮蔽、`capsMask==0` 时查询恢复旧行为返回存储/默认值（`15,1,1,1,1,0`）且设置拒收；Configure App 门控报文保留 `TODO(SV3/Q1)`。 |
| `main/def/GnssSubDef.hpp`、`main/def/StorageDef.hpp` | `GnssSubVerb::kPhonePpp`/`kPhoneSatCfg` 重命名为 `kApplyPpp`/`kApplySatTrack`（保留原编号 11/12）；`StorageVerb` 新增 `kSetHeight`（6）与 `kSetSnPn`（7）。 |
| `main/data/AppData.{hpp,cpp}` | 删除 `phonePppCommand`/`phonePppResult`/`phoneSatCommand`/`phoneSatResult` 专用命令结果槽；`phonePppEnabled`/`phoneSatConfig` 重命名为 `gnssPppEnabled`/`gnssSatTrack`；新增 `phoneHeight` 与 `phonePn` 共享快照槽。 |
| `main/modules/storage/StorageModule.{hpp,cpp}` | 启动加载与写入同步发布 `phoneHeight`/`phonePn` 及 `gnssPppEnabled`/`gnssSatTrack`；`onCommand` 新增 `StorageVerb::kSetHeight` 与 `kSetSnPn` 处理（`kSetSnPn` 复用 `saveSn` 并去抖保存 PN，避免重复递增 `configRev`）。 |
| `main/modules/gnss/GnssModule.{hpp,cpp}`、`GnssModule{Cmds,Frames,Ppp,Probe}.cpp` | `publishCaps()` 填充 `pppSupported` 与 `constMask`（CM680：`pppSupported=false`、`constMask=0x1F`，依据 V1.2.8 手册 §4.9 与 §5.1–5.3；UM98x：`pppSupported=true`、`constMask=0x1F`）；`cmdSetting()` 对 `kApplyRate`/`kPatchOutput` 在未运行/探测未完成时委托 `cmdSetRate`/`cmdSetOut` 立即落盘返回 `kOk`，在 `kMaintenance(kSave)` 时走延迟两阶段提交（P2-1/P2-2）；`cmdApplyPpp`/`cmdApplySatTrack` 经 `kIo` 执行器延迟结单（`fw::kDetailDeferred` → `completeSetting`），移除旧 `phonePpp*`/`phoneSat*` 轮询路径，并在 `abortPendingSettings()` 中止在途脚本。 |
| `main/app/SettingsBackend.cpp` | 补齐 4 个新动词 `static_assert` 与 `getSetting` 快照读取（`gnss.ppp`、`gnss.sat_track`、`phone.height` 16 字节大写 hex、`phone.sn_pn` 32 B `kBytes`）。 |
| `main/modules/bt_link/BtLinkModule.{hpp,cpp}` | `PhoneCompatOps` 全部回调统一经 `self->context_` 取上下文（P3-5），`0x06`/`0x33`/`0x24`/`0x08` 全面接入 `settings::Service`（`Caller::kPhone`，仅异步 `post`/`poll`/`cancel`，绝不调用 `Executor::call()`）；`0x08` 完成后同步更新 `DeviceInfo.sn`；`BtLinkModule::poll()` 在同执行器 `StorageModule` 凭据在途时返回 `true` 以立即派发；`wait()` 在 `kIo` 凭据在途时将等待上限收窄至 50 ms。 |
| `main/modules/web/WebLogic.{hpp,cpp}`、`main/modules/web/WebModule.cpp` | `webGnssStateName`/`webGnssFixName` 重命名为 `gnssStateName`/`gnssFixName`；`buildConfigFrame()` 增加 `(spec.access & settings::access::kWeb) != 0U` 过滤，`writeSetting()` 对 `ValueKind::kBytes` 显式归入返回 `false` 分支并附注释，防止手机/本地专用键泄漏到网页 v1 `config` 帧。 |
| `main/support/ConsoleSupport.{hpp,cpp}`、`main/modules/gnss/GnssCmds.cpp`、`main/modules/system/FactoryCmds.cpp`、`main/modules/storage/ConfigCmds.cpp` | 新增仅控制台任务可用的 `submitSetting()`（`Caller::kConsole` + 20 ms 轮询 + 超时 `cancel`）；`gnss rate`、单条 `gnss out <gga\|gsa\|gsv\|sats\|gst\|rmc>`、`gnss gate`、`factory sn` 均接入 `settings::Service` 并保持既有参数解析与输出文本不变；`config` 保留既有分组查询输出不变，新增 `config get <key>` 与 DEV 专属 `config set <key> <value>`（production 档返回 `read-only in production`）。 |
| `test/settings/TestSettings{Registry,Service}.cpp`、`test/services/TestPhoneCompat.cpp`、`tools/main_host_check/{CMakeLists.txt,GnssResetCheck.cpp,MainHostCheck.cpp}` | 扩展 20 键注册表、`Caller::kPhone` 权限与预探测静态校验测试（MQTT 仍全 20 键返回 `kLocked`）；新增 `0x24`/`0x08` 延迟应答、`cancel` 钩子与 `satCapsMask`（含 `capsMask==0` 旧查询行为）测试；`GnssResetCheck` 改为经真实 `rtos::Executor` 投递 `kApplyPpp`/`kApplySatTrack` 并补齐探测未完成与 `kMaintenance(kSave)` 两窗口回归。 |
| `configs/none-wifi.defaults`（新增）、`tools/ci/check_no_cli_deps.py`（新增）、`tools/ci/run_gates.sh` | 新增 `configs/none-wifi.defaults`（P3-9）；`run_gates.sh` 引用该文件新增 `G3-idf-none-wifi` 构建项与 `G7c-no-cli-deps` 检查脚本（核验 web/phone/settings `.obj` 零 `cli::`/console 符号引用，且 NONE+Wi-Fi ELF 链接 Web+Phone+Settings 而不链接 REPL、访问器及 `kConfigCmds`/`kGnssCmds` 等命令表对象符号，P3-8）。 |

## 2. 冻结文件

- **在 `docs/frozen.sha256` 清单内改动的冻结文件（1 项，经评审修复授权）**：
  - `components/link_core/include/core/LinkService.hpp`：第 24 行 `kMaxDeferredReplies` 由 `4` 调整为 `6`（对应 OMP 评审 P3-2）。`docs/frozen.sha256` 本身未改动，留待 Architect 验收后重算。
- **不在 `docs/frozen.sha256` 清单内的组件公共头（2 项，对应 OMP 评审 P3-4，由 Architect 决定是否补录清单）**：
  - `components/settings/include/settings/Types.hpp`
  - `components/settings/include/settings/Registry.hpp`
- `sdkconfig.defaults` 未修改。

## 3. 遗留标识符清理验证

实际执行 `rg "phonePpp|phoneSat|webGnss" main components` 输出为空（退出码 `1`，0 处命中）：

```text
$ rg "phonePpp|phoneSat|webGnss" main components
(no output, exit code 1)
```

## 4. 门禁结果

环境：`IDF_PATH=$HOME/esp/esp-idf-v6.1`，`IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`，评审修复后执行 `tools/ci/run_gates.sh --base 5915dd6 --prefix sv3b`。
汇总：**39 passed, 1 failed**（唯一失败为评审修复 P3-2 授权改动的 `G1-frozen`：`components/link_core/include/core/LinkService.hpp`，日志目录 `builds/sv3b-logs/`）。

| 门禁 / 命令 | 结果 | 证据 / 备注 |
| --- | --- | --- |
| `G1-frozen` (`sha256sum -c --quiet docs/frozen.sha256`) | FAIL（预期） | 仅 `components/link_core/include/core/LinkService.hpp` 1 项不匹配（P3-2 授权修改，由 Architect 重算） |
| `G2-*` / `G2b-*-asan`（12 个主机目录 + `gnss-all`，共 26 项） | PASS | 普通 + ASan/UBSan 全通过；`main_host_check` 81/81（含 P2-1/P2-2 两窗口回归） |
| `G5-format` (`clang-format --style=file --dry-run --Werror`) | PASS | 0 格式违规 |
| `G6-doc_check` (`doc_check.py --base 5915dd6`) | PASS | 改动函数 110 个，不合格 0 个 |
| `G3-idf`（默认 DEV 构建） | PASS | 0 warning，`rtk_main.bin` 1,451,568 B |
| `G3-idf-trace-off` | PASS | 0 warning |
| `G3-idf-wifi-off` | PASS | 0 warning |
| `G3-idf-production` | PASS | 0 warning |
| `G3-idf-console-none` | PASS | 0 warning |
| `G3-idf-none-wifi`（`configs/none-wifi.defaults`） | PASS | 0 warning，`rtk_main.bin` 1,360,496 B |
| `G3b-rtk_pure-esp32` / `G3b-rtk_pure-esp32c3` | PASS | 0 warning |
| `G5b-tidy` (`tidy_check.py`) | PASS | 141 个源文件，0 条告警 |
| `G7b-runtime` (`g7b_check.py --build builds/sv3b-idf`) | PASS | 130 个自有 `.obj`，0 违规堆符号，0 `__cxa_guard`，异常/RTTI 白名单 6/6 |
| `G7c-no-cli-deps` (`check_no_cli_deps.py --build builds/sv3b-none-wifi`) | PASS | web/phone/settings `.obj` 零 `cli::`/console 引用；ELF 含 `WebModule`/`PhoneCompatService`/`settings::Service`，无 REPL、访问器及 `kConfigCmds`/`kGnssCmds` 等命令表对象符号 |
| `git diff --relative --check` | PASS | 无空白错误 |
| 上板实测（蓝牙 / 控制台 HIL / NONE+Wi-Fi） | NOT_RUN | 禁止操作串口与烧录，交由 Architect 上板验收 |

## 5. 内存与镜像大小

单位：字节（B）。基线参考 `docs/reports/SV2.md`（注：主线合入 H1 后默认栈与堆预算已调整，下表为 `sv3b-idf` 与 `sv3b-none-wifi` 实测值）。

| 指标 | SV2 (`builds/sv2-idf`) | SV3b 默认 (`builds/sv3b-idf`) | SV3b NONE+Wi-Fi (`builds/sv3b-none-wifi`) | NONE+Wi-Fi 相对默认 |
| --- | ---: | ---: | ---: | ---: |
| Flash Code (`.text`) | — | 1,039,948 | 974,072 | -65,876 |
| Flash Data (`.rodata` 等) | — | 245,444 | 220,692 | -24,752 |
| IRAM | 114,715 | 114,715 (87.52%) | 114,699 (87.51%) | -16 |
| DRAM | 97,659 | 95,851 (76.94%) | 93,403 (74.97%) | -2,448 |
| DRAM `.data` | 51,275 | 51,307 | 50,875 | -432 |
| DRAM `.bss` | 46,384 | 44,544 | 42,528 | -2,016 |
| `rtk_main.bin` | 1,536,992 | 1,451,568 | 1,360,496 | -91,072 |
| `index.html.gz` | 13,719 | 13,719 | 13,719 | 0（<= 24 KiB） |

## 6. 行为说明与设计边界

1. **执行器通知槽硬约束**：`BtLinkModule` 与 `WebModule` 运行在 `kLink` 执行器上，所有经 `settings::Service` 发往 `kIo`（`gnss.ppp`/`gnss.sat_track`）或同执行器 `kLink`（`phone.height`/`phone.sn_pn`）的请求均只走 `post()`/`poll()`/`cancel()`，未在任何执行器任务内调用 `Executor::call()`。控制台 `submitSetting()` 仅在 REPL 任务轮询，同样不占用 `Executor::call()` 通知槽。
2. **CM680 能力依据（V1.2.8 手册）**：
   - §4.9（p.27–28）：支持 `CONFIG SBAS DISABLE` / `CONFIG SBAS ENABLE AUTO`。
   - §5.1–5.3 & 表 5-4（p.30–32）：支持 `MASK <elev>`（-90..90°）与 `MASK`/`UNMASK` GPS/BDS/GLO/GAL/QZSS。
   - 手册无 `CONFIG PPP`：因此 `GnssModuleProbe::publishCaps()` 对 CM680 发布 `pppSupported=false`、`constMask=0x1F`（GPS/BDS/GLO/GAL/SBAS），`0x33 Survey` 查询恒回 0、设置直接回 `02 02` 不发 UART 命令；`0x06 Survey` 按 `constMask` 正常应答。
3. **保留未并入 `settings::Service` 的项**：
   - 手机 `0x12` NTRIP（112 B）、`0x32` JSON 服务器（42 B）、`0x33` Nav Thingsboard（86 B）均大于冻结的 `fw::kCommandArgBytes=32`，且 ADR-0018 §5 将 4G/NTRIP/MQTT 归入后续 ADR，仍由 `StorageModule` 去抖 Blob 管理。
   - 手机 Configure App GGA 质量门控协议报文未定稿，已在 `PhoneCompatService.cpp` 保留 `TODO(SV3/Q1)`。
   - 蓝牙模式下 `bt trust`/`bt untrust` 需同步修改 `BtLinkModule::link_manager_` 运行态白名单，继续投递 `BtLinkModule`（`LinkVerb::kWhitelistAdd/Del`）；`bt.wl.add`/`bt.wl.del` 仍用于 Wi-Fi 配置模式及 `config set`。
   - `gnss out default`/`all`/`heading` 因涉及多键原子重置或非 5 项标准 NMEA 键（`heading`），保留原 `kSetOut` 通道；单条 `gga`/`gsa`/`gsv`/`sats`/`gst`/`rmc` 走 `settings::Service`。
   - `gnss.rate` / `gnss.out.*` 在探测完成（`caps.valid == true`）后按 `gnssCaps` 严格校验（例如 1 Hz 定位下 `gnss out gga 2` 会被域校验拒绝并返回 `ERR invalid argument`），与网页共用同一校验真源；在接收机尚未探测完成（`gnssEnabled && !caps.valid`）时，`Service::set()` 仅做静态范围校验（rate `1/2/5/10/20`、输出周期为 `periodMsToInterval` 合法值），`GnssModule::cmdSetting()` 委托既有 `cmdSetRate`/`cmdSetOut` 立即落盘 NVS、推进 `apply_seq_` 并返回 `kOk`（控制台打印输出计划）；在 `kMaintenance(kSave)` 期间同样委托既有两阶段提交分支，等待保存结束后自动下发并结单，与 HEAD 行为保持一致（P2-1/P2-2）。
   - `phone.sn_pn` 权限为 `kPhoneInternal`（仅手机 `0x08` 与内部可写，控制台 `config set phone.sn_pn ...` 按设计返回 `ERR locked`，修改 SN 走 `factory unlock` + `factory sn`；P3-6）。
   - `kSetHeight` 沿用迁移前既有去抖语义：收到即更新 RAM 与快照并返回 `kOk`（手机收到 `01 01`），实际 NVS 写入延迟 500 ms 由 `pollPhoneBlob` 完成，失败按 1 s→30 s 退避重试（即**回执早于落盘**；重复写入相同值亦递增 `configRev`，但无网页消费者受影响）。`kSetSnPn` 沿用既有分步语义：先同步 `saveSn`（失败回滚 `sn_`），再登记 PN 500 ms 去抖（P3-7）。
   - `0x06` Survey Query 在无星座能力的型号（`capsMask == 0`，如 ZED-F9P / K803）上保持旧行为返回 `readSatConfig()` / 默认值 `15,1,1,1,1,0`，仅 Set 返回 `02 02`（P3-1）。

## 7. 上板验收步骤（供 Architect）

1. **烧录默认固件（`builds/sv3-idf`）并执行既有控制台 HIL 冒烟**：
   - `config`、`config identity|link|gnss|debug|build` 验证输出与 SV2 完全一致。
   - `config get gnss.rate`、`config get gnss.out.gga`、`config get gnss.gga_gate.enabled`、`config get gnss.ppp`、`config get gnss.sat_track`、`config get factory.sn` 验证读回正确。
   - `config set gnss.rate 5`、`config set gnss.gga_gate.enabled 1` 验证生效并落盘；`config set factory.sn X` 验证未解锁返回 `ERR locked`；`config set phone.sn_pn ...` 返回 `ERR locked`（仅手机 `0x08` 可写，属设计预期）。
   - `gnss rate 10`、`gnss out gga 5`、`gnss out gsv 1/5`、`gnss gate on min_sats 10`、`factory unlock <key>` + `factory sn <val>` 验证输出格式与 HIL 脚本完全兼容且重启后 NVS 持久化生效。
2. **手机蓝牙协议实机回归（CM680 与 UM98x）**：
   - Survey `0x06` Query/Set：验证截止角与星座设置经 `kIo` 下发并持久化，重启重探后自动重放。
   - Survey `0x33` Query/Set：CM680 上验证 Query 恒回 0、Set 立即回 `02 02` 且 UART 无 `CONFIG PPP` 下发；UM98x 上验证异步延迟应答、持久化与重探重放。
   - `0x24` 天线高与 Nav `0x08` SN/PN：验证异步延迟应答、`0x08` 修改后 `0x01` 设备信息立即反映新 SN、重启回读一致。
   - 异常测试：在 `0x06`/`0x33` 执行中主动断开蓝牙 SPP，确认无 credit 泄漏，重连后可再次发起请求。
3. **NONE + Wi-Fi 固件（`builds/sv3-none-wifi`）上板验证**：
   - 烧录 `builds/sv3-none-wifi/rtk_main.bin`，确认 UART0 无控制台提示符/REPL 交互（启动日志可见 `console disabled (profile none)`）。
   - 触发进入 Wi-Fi 配置模式，连接热点打开配置页，确认 `hello`/`caps`/`config`/`status` 正常（且 `config` 中不包含 `gnss.ppp`/`gnss.sat_track`/`phone.*` 非网页键），修改配置项、保存与退出回落蓝牙均正常。
   - 蓝牙模式下连接手机 App，确认 `0x06`/`0x33`/`0x24`/`0x08` 正常读写。

## 8. 风险与性能观察（仅记录，本次未优化）

1. **控制台单条 `gnss out` 与 `gnss rate` 的全表下发延迟**：通过 `settings::Service`（`kPatchOutput`/`kApplyRate`）修改单个输出周期或频率时，底层仍触发整轮 `requestOutput` 序列，CM680 下需数百毫秒等待每条语句 ACK（与 SV2 相同，留待功能全部完成后统一优化）。
2. **`gnss gate on min_sats 10` 双键顺序提交**：同时修改开关和阈值时经 `settings::Service` 分两次单键同步 NVS 提交（先写 `min_sats` 再写 `enabled`，每次独立写 NVS 并递增 `configRev`），避免了先用旧阈值开启门控的瞬态，但会触发两次 NVS 写入。
3. **`BtLinkModule` 异步轮询周期**：`0x06`/`0x33` 在途时 `BtLinkModule::wait()` 将最大阻塞收窄至 50 ms；若后续追求更低回复抖动，可在 `kIo` 完成回调中补充任务唤醒信号（本次按不优化原则未改执行器机制）。
4. **`phone.height` / `phone.sn_pn` 的文本 hex 往返**：为了契合 `settings::Service::set(std::string_view)` 统一文本接口，`BtLinkModule` 与 `SettingsBackend` 间经固定栈缓冲做定长 hex 编解码（无堆分配、开销极小）。
5. **延迟应答槽满无回包风险（P3-2）**：`LinkService::kMaxDeferredReplies` 已由 4 提至 6（足以覆盖单会话全部 4 类延迟请求并发 + 冗余槽位），但在极端双会话并发打满 6 槽时，`LinkService::registerDeferred()` 会释放 credit 并调用 `hook.cancel`，**不向对端回发失败帧**，对端只能依赖自身请求超时。

## 9. 评审修复（依据 OMP `docs/reports/SV3-review-omp.md`）

| 编号 | 处理结果 | 改动摘要 |
| --- | --- | --- |
| P2-1 / P2-2 | 已修复 | `components/settings/src/Service.cpp` 在 `gnssEnabled && !caps.valid` 时对 `gnss.rate`/`gnss.out.*` 做静态合法范围校验（不改 `Service::domain()`，网页 `hello.caps` 仍隐藏未探测能力）；`main/modules/gnss/GnssModuleCmds.cpp` 放宽 `cmdSetting()` 守卫，在未运行/探测未完成时委托 `cmdSetRate`/`cmdSetOut` 立即落盘返回 `kOk`，在 `kMaintenance(kSave)` 时走延迟两阶段提交。补充 `TestSettingsService` 预探测校验与 `GnssResetCheck` 探测未完成 / `kMaintenance` 两窗口 host 回归测试。 |
| P3-1 | 已修复 | `components/link_core/services/PhoneCompatService.cpp` 中 `fillSatTrackQueryPayload()` 在 `capsMask == 0` 时恢复旧行为返回 `readSatConfig()` / 默认 `15,1,1,1,1,0`；同步更新 `TestPhoneCompat.cpp`。 |
| P3-2 | 已修复 | `components/link_core/include/core/LinkService.hpp` 将 `kMaxDeferredReplies` 由 4 提至 6（未改 `docs/frozen.sha256`），并在 §8 记录满槽取消不回包风险。 |
| P3-3 | 已接受 | `config` 帮助文本新增 `get`/`set` 说明，不改。 |
| P3-4 | 交由 Architect | `components/settings/include/` 是否补录 `docs/frozen.sha256` 由 Architect 决定，本次不改 `frozen.sha256`。 |
| P3-5 | 已修复 | `main/modules/bt_link/BtLinkModule.{hpp,cpp}` 将全部 `PhoneCompatOps` 回调统一改为 `BtLinkModule` 静态成员方法，一律经 `self->context_` 取上下文，移除全局 `appContext()` 旁路。 |
| P3-6 / P3-7 | 已记录 | 在 §6/§7 明确说明 `phone.sn_pn` 仅手机/内部可写（`config set` 返回 `ERR locked`），以及 `kSetHeight` 去抖回执早于 NVS 落盘、`kSetSnPn` 分步落盘与旧版语义等价。 |
| P3-8 | 已修复 | `tools/ci/check_no_cli_deps.py` 在 `--defined-only` 检查中增加 `::kConfigCmds`/`::kGnssCmds`/`::kGnssVerbs`/`::kFactoryCmds`/`::kFactoryVerbs`/`::kLinkCmds`/`::kBtVerbs`/`::kSystemCmds`/`::kStatusVerbs`/`::kRtcmCmds`/`::kDebugCmds`/`::kRadioVerbs`/`::kFactoryResetCmds` 命令表对象符号断言。 |
| P3-9 | 已修复 | 新增 `configs/none-wifi.defaults`，`tools/ci/run_gates.sh` 直接引用该文件构建 `G3-idf-none-wifi`。 |
| §7 `writeSetting` | 已修复 | `main/modules/web/WebLogic.cpp` 的 `writeSetting()` 为 `ValueKind::kBytes` 显式归入返回 `false` 分支并附注释。 |

修复后重跑 `tools/ci/run_gates.sh --base 5915dd6 --prefix sv3b`：**39 PASS / 1 FAIL**（唯一 FAIL 为 P3-2 授权的 `G1-frozen`: `components/link_core/include/core/LinkService.hpp`），`G5b-tidy` 141 文件 0 告警，`G6-doc_check` 110 函数 0 违规，`G7b-runtime`、`G7c-no-cli-deps` 全通过。

## Architect 验收（2026-11-20，静态部分）

- 评审：OMP 交叉评审 `docs/reports/SV3-review-omp.md`，无 P1；P2-1/P2-2、P3-1/2/5/6/7/8/9 已由执行者修复，P3-3（`config` 帮助文本含 get/set）接受。
- 冻结清单：P3-4 采纳，`components/settings/include/settings/{Registry,Service,Types}.hpp` 补录 `docs/frozen.sha256`；`LinkService.hpp`（`kMaxDeferredReplies` 4→6）重算。
- 门禁：Architect 独立重跑 `run_gates.sh --base 5915dd6 --prefix sv3c`，40 passed, 0 failed（`rtk_main.bin` 1,451,568 B）。
- 未做：上板 HIL（控制台冒烟、手机 0x06/0x33/0x24/0x08、NONE+Wi-Fi 网页配置），由用户按 §7 步骤执行；通过前不合入 main。

## 10. 二轮修复（T1–T10）

| 编号 | 改动摘要 |
| --- | --- |
| T1 | `GnssModuleCmds.cpp` 的 `cmdSetRate` / `cmdSetOut` 在 `kConfiguring` / `kOutput` / `kReadback` 瞬态返回 `kBusy`（不再直写 NVS 导致接收机与 NVS 失步）；`kOff`/`kPowerUp`/`kProbing`/`kFailed` 保持直写 NVS。补齐 `GnssResetCheck` 回归。 |
| T2 | `GnssModuleCmds.cpp` 的 `onCommand` 在 `ppp_seq_`/`sat_seq_` 忙守卫之前放行 `kProbe`/`kReset` 进入 `dispatchStandardCmd`，使 `abortPendingSettings()` 正常中止序列并以 `kInvalidState` 结单。补齐 `GnssResetCheck` 回归。 |
| T3 | `components/settings/src/Registry.cpp` 统一 `factory.sn` 与 `phone.sn_pn` SN 段校验为可见 ASCII `0x21..0x7E`（长度 `1..16`），PN 段同样拒控制字符；`FactoryCmds.cpp` 前置检查保持 `"bad sn"` 并与新规则一致。补齐 `TestSettingsRegistry` 回归。 |
| T4 | `BtLinkModule::poll` 检测 `configRev` 变化后按需从 `settings.get("factory.sn")` 同步 `DeviceInfo.sn`（无变化仅原子读比较）；`StorageModule::setSnPnCmd` 改为先校验并暂存 PN 去抖状态、再执行 `saveSn`（失败回滚 RAM PN 状态），消除 SN 落盘而 PN 报 `kBusy` 失步。 |
| T5 | `GnssModuleProbe.cpp` 的 `restoreSettingsForModel` 在探测出型号不支持当前频率并回退到最近较低值后，同步写回 NVS 并递增 `configRev`（重探同样生效）。补齐 `GnssResetCheck` 20 Hz→10 Hz 回归。 |
| T6 | `tools/ci/check_no_cli_deps.py` 对显式列出的 `bt_link/*.obj` 与 `SettingsBackend.cpp.obj` 缺失报 error（不再静默跳过）；`FORBIDDEN_OBJ_RE`/`FORBIDDEN_ELF_RE` 增检 `app::submitSetting(`、`app::settingsErrorText(`、`cli::outPrintf(`。 |
| T7 | `SystemModule.hpp` / `FactoryCmds.cpp` 暴露 `factoryUnlocked()` 只读查询；`ConfigCmds.cpp` `cmdConfigSet` 传入当前工厂解锁状态，不再硬编码 `false`。 |
| T8 | `FactoryCmds.cpp` `cmdSn` 将 `kUnsupported` 报错由 `"link not running"` 改为 `"settings unavailable"`；`ConsoleSupport.cpp` 将 `settings::Error::kTimeout` 文本改为 `"timeout (may have been applied)"`。 |
| T9 | `GnssCmds.cpp` `cmdGnssGate` 双键提交时若第二键（`enabled`）失败，自动将第一键（`min_sats`）回滚至原值并报错。 |
| T10 | 清理全仓无生产调用者的 `StorageModule::setPhoneSnPn`（`MainHostCheck` 改经 `StorageVerb::kSetSnPn` 验证，`setPhoneHeight` 因 `setHeightCmd` 使用予以保留）；`BtLinkModule::wait()` 与 `poll()` 合并使用 `hasPendingTicket()`，并注明 50 ms 上限与 `kLink` `idleWaitMs=20` 的关系。 |
