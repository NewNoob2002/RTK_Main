# Q1-gga-quality-gate 实现报告

- 任务卡：`docs/tasks/Q1-gga-quality-gate.md`
- 分支：`feat/gnss-optisens-q1`（基线 `215851d`，构建前缀 `builds/gq-*`，叠加于 `G-optisens-family` 之上）
- 结论：实现完成，除 `G-optisens-family` 授权的 2 个冻结头触发的 `G1-frozen` 外，全部自动门禁（34/35）与 `web_mock.py --selftest` 均 `PASS`；HIL 未执行（`NOT_RUN`）。本卡零冻结文件改动。

## 1. 改动清单

### 1.1 纯逻辑与协议组件（无 IDF 依赖，无堆）

| 文件 | 改动摘要 |
| --- | --- |
| `components/gnss/private_include/GgaQualityGate.hpp`（新增） | 定义 `gnss::GgaGateConfig`（默认关闭、`minSats=8`、范围 `0..40`、放行迟滞 `kReleaseStreak=3`）、`gnss::GgaGateStats` 与 `gnss::GgaQualityGate` 状态机（含 `evaluate()`、`setConfig()`、`reset()`、`ggaAllowed()`、`stats()`）。 |
| `components/gnss/src/GgaQualityGate.cpp`（新增） | 实现无浮点、无堆的 NMEA GGA 第 6 字段（定位质量 `1..8`）与第 7 字段（使用卫星数 `>= minSats`，支持前导零如 `08`）解析；缺省/空字段/畸形按不达标处理；实现“放行态 1 坏即断、阻断态连续 3 达标放行、关闭态全部放行、关闭开关立即解除阻断”。 |
| `components/gnss/CMakeLists.txt` | 将 `src/GgaQualityGate.cpp` 加入 `_gnss_srcs`。 |
| `components/web_proto/include/web_proto/KeyTable.hpp` | 新增 `KeyKind::kIntRange` 及 `KeySpec::minValue`/`maxValue`，注册 `gnss.gga_gate.enabled`（`kBool`）与 `gnss.gga_gate.min_sats`（`kIntRange`，`0..40`）。 |
| `components/web_proto/src/KeyTable.cpp` | 实现 `validateIntRange()` 严格十进制范围校验，越界或非法格式返回 `ErrorCode::kBadValue` 且保持输出原值。 |
| `components/link_core/services/PhoneCompatService.cpp` | 添加 `TODO(Q1)` 注释，注明 Configure app 的 GGA 质量门控消息等待 app 团队定义消息 ID 与负载格式后再接入。 |

### 1.2 应用集成、控制台、存储与网页 UI

| 文件 | 改动摘要 |
| --- | --- |
| `main/def/GnssSubDef.hpp` | 新增 `GnssSubVerb::kSetGgaGate = 13`、标志位常量 `kGgaGateFlagEnabled`/`kGgaGateFlagMinSats`/`kGgaGateCmdArgLen` 与 `struct GgaGateStatus`。 |
| `main/data/AppData.hpp` / `main/data/AppData.cpp` | 新增 `fw::Slot<GgaGateStatus> ggaGateStatus` 与无锁原子 `std::atomic<bool> ggaAllowedFlag`（附 `static_assert(fw::kAtomicUsable<bool>)`）及公共接口 `bool ggaAllowed() const noexcept`（供蓝牙直传、0x04 query 与未来 4G NTRIP 客户端共用）。 |
| `main/modules/gnss/GnssKeys.hpp` | 在 `gnss_store::kConfigKeys`（`StoreCatalog` 的 `gnss.config` 组，位于 `baud` 之前以保持 `baud` 末项不变量）追加 `gnss/gga_gate`（u8 `0..1`）与 `gnss/gga_min_sats`（u8 `0..40`）。 |
| `main/modules/gnss/GnssModule.hpp` / `GnssModule.cpp` / `GnssModuleStore.cpp` / `GnssModuleFrames.cpp` / `GnssModuleCmds.cpp` / `GnssModuleMaintenance.cpp` | 初始化与 NVS 加载/保存回调；`publishAppFrame()` 仅对 `OutputMsg::kGga` 过门控（阻断时丢弃、不更新 `gnssLatest`、不入 `gnssAppRing`；GST/RMC/GSA/GSV 照常推送）；`cmdSetGgaGate()` 立即生效并安排 500 ms 去抖 + 失败指数退避落盘；`resetStoredSettings()` 随出厂复位恢复默认；对外暴露 `GnssModule::ggaAllowed()`。 |
| `main/modules/bt_link/BtLinkGnss.cpp` | `onGnssQuery()` 对 0x04（GGA）检查 `ctx().data.ggaAllowed()`，阻断时返回失败（`GnssService` 回复 `02 02`）；`drainGnssStream()` 同步丢弃阻断前滞留环中的 GGA。 |
| `main/modules/system/SystemStatusCmds.cpp` / `main/modules/gnss/GnssCmdsStatus.cpp` | 在 `status gnss`、`status` 概览与 `gnss status` 增加 `gate` 行：`gga_gate=on/off  min_sats=<n>  blocked=yes/no  gga_blocked=<n>  gate_trans=<n>`。 |
| `main/modules/gnss/GnssCmdPack.hpp` / `main/modules/gnss/GnssCmds.cpp` | 新增 `packGnssGate()` 与控制台子命令 `gnss gate [on|off] [min_sats <n>]`（无参打印当前值；PRODUCTION 档位遵循现有白名单仅允许只读查询）。 |
| `main/web/ui/index.html` / `main/web/ui/app.css` / `main/web/ui/app.js` | 在 GNSS 卡片新增英文 UI 控件 `GGA Quality Gate`（开关 `#cfg-gga-gate` 与 `0..40` 数字框 `#cfg-gga-min-sats`），同步解析 `config.gga_gate` 对象并通过 `gnss.gga_gate.enabled` / `gnss.gga_gate.min_sats` 下发配置。 |
| `tools/web_mock/web_mock.py` | 模拟服务端 `config` 增加 `gga_gate: {"enabled": False, "min_sats": 8}`，处理两个新键及范围校验，并补充 `--selftest` 断言。 |

### 1.3 测试覆盖

| 文件 | 覆盖场景 |
| --- | --- |
| `test/gnss/TestGnssGgaGate.cpp`（新增） / `test/gnss/CMakeLists.txt` | 默认关闭全放行；质量 0 / 卫星不足 / 空质量 / 空卫星 / 畸形字段 / 短帧 / 非 GGA 立即阻断；连续 3 条达标才放行（中途坏帧清零计数）；`min_sats` 0..40 边界与越界拒绝；运行中关闭开关立即解除阻断。 |
| `test/web_proto/test_web_proto_key_table.cpp` / `test_web_proto_writer.cpp` | `gnss.gga_gate.enabled` 与 `gnss.gga_gate.min_sats` 合法与越界（`-1`/`41`/`08`/`abc`）校验；`JsonWriter` 输出 `config` 嵌套对象 `"gga_gate":{"enabled":true,"min_sats":8}`。 |
| `tools/main_host_check/GnssStreamCheck.cpp` | `testGgaQualityGateQueryAndStream()`：阻断时 0x04 query 回复 `02 02`、直传丢弃 GGA 且 GSV 照常推出；放行后 0x04 query 恢复返回真实 GGA。 |
| `tools/main_host_check/GnssResetCheck.cpp` / `GnssOutputCheck.cpp` | `testGgaGateNvsAndModuleFlow()`：默认关闭/8 星、下发 `kSetGgaGate` 立即生效、500 ms 去抖后写入 NVS、低星 GGA 阻断而 RMC 照常更新缓存、连续 3 条达标恢复放行、新实例重启从 NVS 回读生效。 |

## 2. 门禁与验证结果

执行命令：
```sh
python3 tools/web_mock/web_mock.py --selftest
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
. "$IDF_PATH/export.sh" >/dev/null
tools/ci/run_gates.sh --base 215851d --prefix gq
```

| 门禁 / 检查 | 结果 | 备注 |
| --- | --- | --- |
| `web_mock.py --selftest` | `PASS` | 覆盖 `gga_gate` 配置回传、开关/卫星数更新与越界 `bad_value` |
| `G1-frozen` | `FAIL`（预期） | 仅 `G-optisens-family` 授权的 2 个冻结头（`Types.hpp`、`UnicoreReceiver.hpp`）；Q1 零冻结文件改动 |
| `G2-host` / `G2b-host-asan` | `PASS` | — |
| `G2-app_fw` / `G2b-app_fw-asan` | `PASS` | — |
| `G2-services` / `G2b-services-asan` | `PASS` | — |
| `G2-stream_demux` / `G2b-stream_demux-asan` | `PASS` | — |
| `G2-bt_reconnect` / `G2b-bt_reconnect-asan` | `PASS` | — |
| `G2-cli` / `G2b-cli-asan` | `PASS` | — |
| `G2-gnss` / `G2b-gnss-asan` | `PASS` | 含新增 `test_gnss_gga_gate` |
| `G2-gnss-all` / `G2b-gnss-all-asan` | `PASS` | 含全驱动 + `test_gnss_gga_gate` |
| `G2-nvs_store` / `G2b-nvs_store-asan` | `PASS` | — |
| `G2-web_proto` / `G2b-web_proto-asan` | `PASS` | 含 `gnss.gga_gate.*` 校验与 `gga_gate` JSON 输出测试 |
| `G2-main_host_check` / `G2b-main_host_check-asan` | `PASS` | 含 0x04 query `02 02`、直传分流、去抖 NVS 保存与重启回读 |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | `PASS` | — |
| `G5-format` | `PASS` | — |
| `G6-doc_check` | `PASS` | — |
| `G3-idf` | `PASS` | `rtk_main.bin` = 905984 B，0 warning |
| `G3-idf-trace-off` | `PASS` | 0 warning |
| `G3-idf-production` | `PASS` | 0 warning |
| `G3-idf-console-none` | `PASS` | 0 warning |
| `G3b-rtk_pure-esp32` / `esp32c3` | `PASS` | 0 warning |
| `G5b-tidy` | `PASS` | 0 warning / 0 error |
| `G7b-runtime` | `PASS` | 无堆新增、无异常/RTTI/`__cxa_guard_*` |
| HIL（实板） | `NOT_RUN` | 按授权边界不烧录、不连接 `/dev/ttyACM0` |

## 3. §文档待改（冻结文档，待 Architect 更新）

1. `docs/protocol/phone_protocol.md`（或现有手机协议文档）：待 app 团队敲定 GGA 质量门控配置消息 ID 与负载格式后补充定义，并与 `PhoneCompatService.cpp` 中 `TODO(Q1)` 对应落地。
2. `docs/web/ws-protocol.md`：
   - §2 `config` 帧补充嵌套对象 `gga_gate: {"enabled": bool, "min_sats": int}`；
   - §4 可改键表补充 `gnss.gga_gate.enabled`（`0`/`1`）与 `gnss.gga_gate.min_sats`（`0`–`40` 整数，越界返回 `bad_value`）。
3. `docs/acceptance.md`：补充 Q1 验收项（`gnss gate` 控制台读写与 PRODUCTION 只读约束、`status gnss` 门控字段、蓝牙直传与 0x04 query 阻断回归、NVS 掉电保持与网页配置项验证）。

## 4. 偏离与未解决问题

- 无契约偏离；W5 设备端 WebSocket 服务器尚未合入当前基线，网页协议解析/校验（`components/web_proto`）、前端 UI（`main/web/ui`）与 `tools/web_mock/web_mock.py` 已全部就绪，待 W5 集成时将 `gnss.gga_gate.*` 接到 `GnssSubVerb::kSetGgaGate` 及 `DataCenter::ggaGateStatus` 即可。
