# C5a 实施报告（GNSS 工作模式与基准站底座）

基线提交：`ee1b73c`（工作区含 C5a 文档提交 `980e425`）。硬件在环验收（HIL）未执行，留待上板验证。

## 改动清单

| 文件 | 改动 |
| --- | --- |
| `components/gnss/include/gnss/Types.hpp`、`NmeaParser.hpp`、`src/NmeaParser.cpp`、`NmeaParserPrivate.hpp`、`NmeaParserPosition.cpp` | 新增 `Fix::hAccCm`（初值 `kU16Unknown`）与 `NmeaStats::gst`；实现 `$GNGST`/`$GPGST` 纯定点解析（`sqrt(lat_err^2 + lon_err^2) → cm`），空误差字段回退为未知，负数或字段残缺计入 `malformed`。 |
| `components/gnss/include/gnss/Receiver.hpp`、`src/Receiver.cpp`、`include/gnss/Controller.hpp`、`src/ControllerImpl.hpp`、`src/ControllerMaintenance.cpp` | 新增 `kBaseRtcmIds`（12 项）、`ModeAction`（`kRover/kBasePrep/kBaseSurvey/kBaseFixed`）、`ModeRequest` 与 `Receiver::modeStep()`；新增 `Maintenance::kMode` 与 `Controller::requestMode()`，复用 `State::kMaintenance` 驱动多步命令序列化发送、应答匹配及 `stepRetries` 重试。 |
| `components/gnss/include/gnss/drivers/{Unicore,Ublox}Receiver.hpp`、`src/drivers/UnicoreScript.cpp`、`UbloxWire.hpp`、`UbloxUbx.cpp` | Unicore 生成 `MODE ROVER`、`MODE BASE TIME <s>`、`MODE BASE <lat> <lon> <h>`（纯定点格式化 9 位/3 位小数）及 11 路 `RTCM<id> COMn <period>` / `UNLOG COMn RTCM<id>`（跳过 1230）；u-blox F9P 生成 `CFG-VALSET`（RAM 层）配置 `CFG-TMODE-*`、`CFG-UART{1,2}OUTPROT-RTCM3X` 及 10 路 `CFG-MSGOUT-RTCM_3X_TYPE*_UART{1,2}`（跳过 1006/1033）；ComNav 沿用基类返回 `false`（`kUnsupported`）。 |
| `main/modules/gnss/GnssBaseConfig.{hpp,cpp}`、`GnssKeys.hpp`、`modules/storage/StoreCatalog.cpp` | 新增 NVS 组 `gnss.base` 全部 10 键（`mode`、`base_type`、`svy_s`、`svy_acc_cm`、`svy_to_s`、`fix_lat` 8B i64 LE、`fix_lon` 8B i64 LE、`fix_h_mm` 4B i32 LE、`rtcm_out` 14B blob v1、`msm`），实现默认值、MSM4/7 互换与无浮点十进制定点解析。 |
| `main/modules/gnss/GnssWorkMode.{hpp,cpp}`、`GnssModule.hpp`、`GnssModule{,Cmds,Frames,Maintenance,Mode,Probe}.cpp`、`main/def/GnssSubDef.hpp` | 在 `main` 实现上层工作模式状态机（`kRover / kBaseConfig / kSurveyWait / kSurvey / kBaseTx / kFallback`）；处理开机自启动切换（不发 `SAVECONFIG`）、手动切换成功后触发 `SAVECONFIG`、`kRunning` 前唯一覆盖槽、`svy_to_s` 总超时回退 `MODE ROVER` 并保持 NVS `mode` 不变、`gnss freset yes` 恢复 `gnss.base` 默认值。 |
| `main/data/BaseRtcmRing.hpp`、`AppData.{hpp,cpp}`、`main/def/RtcmDef.hpp`、`modules/bt_link/BtLinkModule.{hpp,cpp}`、`BtLinkSessions.cpp`、`BtLinkGnss.cpp`、`modules/rtcm/RtcmForwarder.hpp`、`RtcmModule.cpp`、`RtcmCmds.cpp` | 新增 4 KiB 静态定界 `BaseRtcmRing`；`kBaseTx` 且 PHONE 流就绪时由 `kIo` 将 GNSS RTCM3 原样入环、`kLink` 经 `spp_server_stream_reserve`/`tx_commit` 下发；无 PHONE 会话不推送不计 drop；满或发送失败丢整帧并计数；基站激活期间手机上行 RTCM 计入 `rejected_mode` 并丢弃。`rtcm stats` 新增 `base_tx` 与 `rejected_mode` 字段并保留原 `rtcm:` 行格式。 |
| `main/modules/gnss/GnssCmdsMode.cpp`、`GnssCmds.cpp`、`GnssCmdsStatus.cpp`、`GnssCmdsPrivate.hpp` | 注册并实现 `gnss mode [rover|base|smartbase]`、`gnss base [survey|fixed]`、`gnss out rtcm [default|msm|id rate]`，并在 `gnss status` 中展示 `mode` 行与 `gst` 计数。 |
| `test/gnss/TestGnss{Nmea,Controller,UnicoreReceiver,UbloxReceiver,ComnavReceiver}.cpp`、`tools/main_host_check/{CMakeLists.txt,GnssModeCheck.cpp,GnssResetCheck.cpp,shim/NvsShim.c}` | 新增 GST、多步 `kMode`、`Unicore`/`u-blox`/`ComNav` 模式命令字节、`GnssBaseConfig`、`GnssWorkMode`、`BaseRtcmRing` 与 `freset` 默认值校验；`NvsShim` 单命名空间条目上限由 16 扩至 32 以容纳 `"gnss"` 命名空间全部 17 个键。 |

## 关键设计与待决问题

1. **状态发布与冻结头文件选择**：`main/def/GnssDef.hpp` 在冻结清单内，因此工作模式扩展枚举、`GnssSubVerb::kMode` 与 `GnssWorkModeStatus` 全部置于非冻结的 `main/def/GnssSubDef.hpp`（待后续 Architect 合并 `docs/frozen.sha256`）。
2. **`BaseRtcmRing` 存储位置**：`tools/main_host_check/BootAllocCheck.cpp` 强校验 `BootArena` 阶段与静态缓冲区计数，为不干扰 `BootBuffers` 既有基线，4 KiB `BaseRtcmRing` 直接静态置于 `DataCenter` 的 `.bss` 段内并用 `DataCenter::mux` 保护；零堆分配。
3. **NVS 大精度坐标存储格式**：`StoreKeys` 整数键只支持无符号 `kU8/kU16/kU32`，故 `fix_lat`、`fix_lon`（8 B 小端 `int64_t`，`1e-9` deg）与 `fix_h_mm`（4 B 小端 `int32_t`，mm）采用 `kBlob` 存储，并在 `GnssModule::readBaseConfig()` 与 NVS 回调中由 `OptionalMuxGuard` 同步。
4. **待决问题（建议确认）**：
   - **u-blox F9P 导航周期与长周期 RTCM 换算**：当前 `rtcmPeriodToEpochs()` 按当前 `navRateHz` 计算 `round(periodMs * rateHz / 1000)` 并钳制到 `255` 历元；若后续高定位率（20 Hz）与超过 12.75 s 的慢周期 RTCM 并存，F9P `CFG-MSGOUT-*` 单字节历元间隔上限为 255，如需更慢周期需调整速率表或拒绝超界组合。
   - **断开重连后的 RTCM 基站推送计数边界**：无 PHONE SPP 流期间 `kIo` 不写 `BaseRtcmRing` 且不计 `drop`；`drainBaseRtcm` 单轮上限 16 帧，高负载流控满时按整帧丢弃并计入 `base_tx.drop`。
   - **参数修改与当前状态联动**：`gnss base survey|fixed` 与 `gnss out rtcm` 修改参数后只写 NVS，不自动重配运行中基站，并输出 `use 'gnss mode base' to apply` 提示（符合 ADR-0014 §2.5/§2.6）。

## 主机测试

| 套件 | 普通构建 | ASan/UBSan |
| --- | ---: | ---: |
| `test/gnss`（默认驱动：`builds/c5a-gnss-def{,-asan}`） | PASS 8/8 | PASS 8/8 |
| `test/gnss`（全驱动：`builds/c5a-gnss{,-asan}`） | PASS 9/9 | PASS 9/9 |
| `tools/main_host_check`（`builds/c5a-main-host{,-asan}`） | PASS 59/59 | PASS 59/59 |
| `tools/t13_host_check`（`builds/c5a-t13_host_check{,-asan}`） | PASS 44/44 | PASS 44/44 |
| `test/services`（`builds/c5a-services{,-asan}`） | PASS 6/6 | PASS 6/6 |
| `test/app_fw`（`builds/c5a-app_fw{,-asan}`） | PASS 8/8 | PASS 8/8 |
| `test/host`（`builds/c5a-host{,-asan}`） | PASS 7/7 | PASS 7/7 |
| `test/cli`（`builds/c5a-cli{,-asan}`） | PASS 3/3 | PASS 3/3 |
| `test/bt_reconnect`（`builds/c5a-bt_reconnect{,-asan}`） | PASS 1/1 | PASS 1/1 |
| `test/nvs_store`（`builds/c5a-nvs_store{,-asan}`） | PASS 12/12 | PASS 12/12 |
| `test/stream_demux`（`builds/c5a-stream_demux{,-asan}`） | PASS 4/4 | PASS 4/4 |
| 合计 | 161/161 PASS | 161/161 PASS |

## 门禁结果

| 门禁 | 命令与实测结果 |
| --- | --- |
| ESP-IDF 主构建 | `PASS`，0 warning（`. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B builds/c5a build`）；生成 `rtk_main.bin` 大小 `0xd1a30`（858,672 B），剩余空间 `0x22e5d0`（73%）。 |
| ESP-IDF 内存统计 | `PASS`（`idf.py -B builds/c5a size`）：`Total image size: 858550 B`，`Flash Code (.text): 592880 B`，`Flash Data (.rodata): 157296 B`，`IRAM: 86287 B (65.83%)`，`DRAM: 73591 B (59.07%; .bss 51816 B, .data 21775 B)`。 |
| 追踪关闭构建 | `PASS`，0 warning（`idf.py -B builds/c5a-trace-off -DSDKCONFIG="$PWD/builds/c5a-trace-off/sdkconfig" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;builds/c5a-trace-off/trace_off.defaults" build`）；`rtk_main.bin` 大小 `0xcd3f0`（840,688 B），根 `sdkconfig` 无改动。 |
| 静态分析（G5b） | `PASS`（`python3 -I tools/tidy_check/tidy_check.py --host-build builds/c5a-host --idf-build builds/c5a --gnss-build builds/c5a-gnss --nvs-build builds/c5a-nvs_store --jobs 4`）：检查 122 个文件，发现 0 条。 |
| 堆与异常白名单（G7b） | `PASS`（`python3 -I tools/g7b_check/g7b_check.py --build builds/c5a`）：自有目标文件 110 个，放行堆符号 0 个，`__cxa_guard` 0 个，ELF 异常/RTTI 符号 6/6 均在白名单内。 |
| 格式检查 | `PASS`（`clang-format --style=file --dry-run --Werror` 对全部改动与新增 `.c/.cpp/.h/.hpp` 文件）；`git diff --check` 无空白错误。 |
| 函数注释检查 | `PASS`（`python3 -I tools/doc_check/doc_check.py --base ee1b73c`）：改动函数 132 个，不合格 0 个。 |
| 黄金向量回归 | `PASS`（`git diff --exit-code -- test/gnss/golden`）：退出码 0，无任何改动。 |
| 冻结清单校验 | `FAIL (按授权范围仅改动 6 个授权 GNSS 公共头)`（`sha256sum -c docs/frozen.sha256`）：仅 `components/gnss/include/gnss/Controller.hpp`、`NmeaParser.hpp`、`Receiver.hpp`、`Types.hpp`、`drivers/UbloxReceiver.hpp`、`drivers/UnicoreReceiver.hpp` 6 个卡内明确授权扩写的公共头校验和不匹配，其余全部文件 `OK`；`docs/frozen.sha256` 未改动。 |
| HIL 上板实测 | `NOT_RUN`（按授权边界未执行烧录、串口或硬件实测；待 Architect 验收）。 |

## Architect 验收（Claude，2026-11-18）

- 复跑：idf `builds/c5a-v` 0 warning，bin 858832 B（较 C4 836784 +22048）；主机 gnss 8/8、all-drivers 9/9、host 7/7、main_host_check 59/59、nvs_store 12/12、services 6/6、t13 44/44；G5b 122 文件 0；G7b PASS；doc_check 0；clang-format；golden 不变；frozen 仅 6 个授权头变化（已重算）。
- **HIL 发现的缺陷（已修）**：`GnssSubVerb::kMode = 6` 与 `GnssMaintenanceVerb::kSave = 6` 撞号，`onCommand` 先按维护命令分流，`gnss mode base` 被当作 `gnss save` → `ERR invalid_arg`。主机测试直接调状态机，未覆盖命令编号。改为 `kMode = 9`，删除空操作的 `kBaseReload`，在 `GnssModuleCmds.cpp` 加 static_assert 防重叠。
- 日志改为句子风格（结尾句号、不用缩写 hAcc/TX active）；ADR-0009 §2.12 增加 `<mode>` 标签。
- HIL 通过（室内无星）：`gnss mode base` 下发 MODE ROVER + RTCM 报文集合 → survey-wait；重启后按 NVS 自动进基站；120 s 超时打 W 回移动站且 NVS 模式仍为 base；`gnss base fixed` → `MODE BASE <lat> <lon> <h>` → base-tx + SAVECONFIG；`gnss mode rover` → UNLOG 全部 RTCM + SAVECONFIG；`gnss freset yes` 后 gnss.base 恢复缺省。
- 未验证：室外有星时 survey-in 完成、RTCM 经蓝牙推到手机（需天线与手机）。
- 遗留（不阻塞）：`gnss out rtcm` 同时列出 1033 与 1230，未按接收机族隐藏不下发的那条；console 线程直接 `saveKeys`（与既有 kIo 保存方式不一致，NVS API 线程安全，风险低）；`GnssModuleMode.cpp` 20 个几乎相同的 load/save 函数可改为表驱动；`BaseRtcmRing` 在自旋锁内逐字节拷贝最长 1 KiB，可改 memcpy 分段；u-blox 探测日志 TX 打印了二进制（既有问题）。
