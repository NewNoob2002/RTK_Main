# P3b 输出闭环（ADR-0012 §2.4–§2.6）

- 分支：`feat/gnss`；基线 `5d2f927`（P3a 合并后）
- 任务卡：`docs/tasks/P3b-gnss-output-closure.md`
- 结论：门禁 1–5 全部 PASS；末行 `P3b DONE`

## 1 做了什么

1. **单一输出计划**：`main/data/GnssOutput.{hpp,cpp}` 新增 `app::data::make_output_plan()`，由 `gnss/rate` + `gnss/gsv` + `gnss/out` + 接收机能力生成 `gnss::OutputPlan`；删除 `classify_gnss_sentence()` 及其私有辅助（`nmea_sentence_is`/`starts_with`/`SentenceType`/tag 常量），语句分类改由驱动 `classify_frame()` 负责（ADR-0012 §2.6）。
2. **驱动侧下发**：`Receiver` 新增 `build_output()`（纯虚之外的默认实现返回 `kNotSupported`）与 `OutputPlan.rate_hz`；三家驱动实现 `build_output()`/`output_mask()`，流动站脚本不再写死任何报文输出。
3. **Controller kOutput 阶段**：基础脚本（模式/端口/波特率）完成后逐项下发输出命令并等 ACK；`kNotSupported` 记 `unsupported_mask`、失败记 `failed_mask`、成功记 `applied_mask`；新增 `request_output()` 只重跑输出阶段（`output_seq` 自增，供 main 判断落地）。
4. **两阶段提交**（`GnssModule`）：`gnss out` / `gnss rate` 先暂存并请求，等 `output_seq` 前进且 `failed_mask == 0` 才写 NVS；失败/超时回退暂存值、重发旧配置、回 `ERR`，NVS 保持旧内容。
5. **能力位透传**：`GnssService` 由「只认 HEADINGA」改为通用 `set_supported_mask()/supported_mask()`（默认 `0x3B`：六类里除 HEADINGA），不支持的类型两个 interval 分支都回 `02 02`；`BtLinkModule` 用接收机 `supported_mask` 更新服务。
6. **控制台**：`gnss out show` 增加 `receiver=on|unsupported|failed|-` 列，`gnss status` 增加 `output` 行；`gnss out`/`gnss rate` 等待落地（50 ms × 60，3 s 超时）后打印逐项结果。

## 2 输出计划规则（§1.1 规则）

`make_output_plan(out, rate_hz, gsv, port, caps)`，下标即 `gnss::OutputMsg`（`kGga=0 … kGsa=5`，与 App `type_idx` 一致）：

| 条件 | 周期 |
| --- | --- |
| `mask` 位为 0 | `0`（关闭；运行时重跑时驱动据此发 UNLOG / VALSET 0） |
| GGA / RMC / GSA / GST 且 interval 等于 `GnssOutConfig::defaults()` 的同项值 | `1000 / rate_hz` |
| SATSINFOA / HEADINGA 且等于默认值 | `1000`（HEADINGA 无 `cap::kHeading` 时为 0） |
| 用户改过 interval | `GnssService::interval_to_period_ms(interval)`（正数=Hz、负数=秒、0=关） |

（审查时修正，见 §10：原实现 GST/SATSINFOA 默认走 interval，与卡 §1.1 不符。）
| `kGsv` | `gsv ? 1000 : 0` |

开机首轮（脚本刚跑完）只下发周期非 0 的项，因此**默认配置下发给模块的字节与 P3a 黄金脚本逐条等价**；只有运行时改配置（`request_output()`）才会给关闭项补发 UNLOG，让「关掉某类语句」真正生效。

## 3 各驱动新旧脚本对照

### 3.1 Unicore（UM980/UM981/UM982）

| 旧（P3a 脚本，写死） | 新（基础脚本 + kOutput） |
| --- | --- |
| ① `UNLOG COM2` | ① `UNLOG COM2` |
| ② `GPGGA COM2 <period_for(rate)>` | — 由 `build_output(kGga)` 下发 `<name> COM2 <period>` |
| ③ `GPRMC COM2 <period>` | — `build_output(kRmc)` |
| ④ `GPGSA COM2 <period>` | — `build_output(kGsa)` |
| ⑤ `GPGST COM2 <period>` | — `build_output(kGst)` |
| ⑥ `SATSINFOA COM2 1` | — `build_output(kSatsInfo)`（周期来自计划） |
| ⑦ `GPGSV COM2 1`（gsv 开） | — `build_output(kGsv)` |
| ⑧ `HEADINGA COM2 1`（双天线） | — `build_output(kHeading)`，无能力返回 `kNotSupported` |
| ⑨ `MODE ROVER` | ② `MODE ROVER` |
| ⑩ `CONFIG COM2 <baud>` | ③ `CONFIG COM2 <baud>`（`port_baud == 0` 时省略） |

周期文本沿用基线格式（`1`/`0.5`/`0.2`/`0.1`/`0.05`，只写有效数字），非 10 ms 整数倍返回 `kInvalidArg`。

### 3.2 ComNav（K803 等）

| 旧 | 新 |
| --- | --- |
| ① `UNLOGALL COM2` | ① `UNLOGALL COM2` |
| ② `GPGGA COM2 <period>` | — `build_output(kGga)` → `LOG COM2 GPGGA ONTIME <period>` |
| ③ `GPRMC …` / ④ `GPGSA …` / ⑤ `GPGSV …`（gsv） | — 同名 `LOG … ONTIME <period>` |
| ⑥ `MODE ROVER` | ② `MODE ROVER` |
| ⑦ `COM COM2 <baud>` | ③ `COM COM2 <baud>` |

`SATSINFOA`/`HEADINGA` 返回 `kNotSupported`（`output_mask` 无这两位）。

### 3.3 u-blox（ZED-F9P）

| 旧 | 新 |
| --- | --- |
| ① `VALSET{RateMeas, Uart1InprotRtcm3x, Uart1OutprotNmea}` | ① 同左 |
| ② `VALSET{MsgoutGga/Rmc/Gsa/Gst=1, Gsv=gsv}`（一条多键） | — 每类报文一条单键 `VALSET`，值 = `round(period_ms × rate_hz / 1000)` 历元（0 = 关） |
| ③ `VALSET{Uart1Baudrate}` | ② `VALSET{Uart1Baudrate}`（`port_baud == 0` 时省略） |

**已知差异（写入契约偏离）**：GSV 的历元数由周期换算（1 s × 频率），基线固定写 `1`；2 Hz 以上时二者数值不同（多键分组也不同，按卡 §1.6 只比 `(key, value)` 集合，且 GSV 键在比对中显式剔除，见 `test/gnss/golden/PlanCheck.hpp` 的 `drop_key`）。

## 4 无接收机时的行为变化（§1.5）

- 探测全失败 → `Controller::receiver()` 返回 `null_receiver()`：`output_mask()` = 0、`build_output()` 全部 `kNotSupported`、`classify_frame()` = `kNone`、`frame_tag()` 长度 0。
- 因此 `publish_status()` 算出的 `supported_mask = 0`，`GnssService` 对全部 6 类语句都回 `02 02`（旧行为是只对 `0x1F` 回 `02 02`，其余照常订阅）。手机侧表现为「GNSS 未就绪时不产生空推送」。
- kOutput 阶段在无驱动时全部记 `unsupported_mask`，不进入 `kFailed`；模块状态与退避重探逻辑不变。

## 5 门禁结果

### 5.1 IDF 构建

| 命令 | 结果 |
| --- | --- |
| `idf.py -B builds/p3b build` | **PASS**，`Project build complete`，0 warning |
| `idf.py -B builds/p3b-off -DCONFIG_RTK_GNSS_ENABLE=n build` | **PASS** |
| `idf.py -B builds/p3b-comnav -DCONFIG_RTK_GNSS_DRV_COMNAV=y build` | **PASS** |
| `idf.py -B builds/p3b size` | 见下表 |
| `idf.py -B builds/p3b-base build/size`（`git archive 5d2f927` 到 `builds/p3b-base-src`） | **PASS**（基线对照） |

| 指标 | 基线 5d2f927 | P3b | 差值 |
| --- | --- | --- | --- |
| Flash Code | 542460 | 544072 | **+1612** |
| Flash Data | 144504 | 144712 | **+208** |
| IRAM | 86187 (65.76%) | 86187 (65.76%) | 0 |
| DRAM | 63855 (51.26%) | 64167 (51.51%) | **+312** |
| Total image | 793830 | 795650 | +1820 |

DRAM +312 B 的来源（均为静态成员，无堆）：`Controller::output_cmd_` `CmdBuf` 164 B（+对齐）、`Controller::plan_` `OutputPlan` 32 B、`output_index_`/`output_rerun_` 及对齐 ~16 B、`ControllerStats` 新增 3 个掩码 + `output_runs`/`output_seq` 20 B、`GnssModule` 暂存态（`pending_prev_out_` 8 B + 2×`uint32_t` + 2×`uint8_t`）~24 B、`GnssStatus` 新增 5×`uint32_t` + `GnssApply` ~24 B。Flash +1612 B 来自三份 `build_output()`/`output_mask()`、`make_output_plan()`、kOutput 状态机与控制台新输出。

### 5.2 主机测试

| 构建 | 结果 | Unity 用例数 |
| --- | --- | --- |
| `cmake -S test/gnss -B builds/p3b-gnss-off -DRTK_GNSS_DRV_COMNAV=OFF` | **PASS** 7/7 | 56（P3a：54） |
| `cmake -S test/gnss -B builds/p3b-gnss-comnav -DRTK_GNSS_DRV_COMNAV=ON` | **PASS** 8/8 | 68（P3a：65） |
| `… -B builds/p3b-gnss-asan -DSANITIZE=ON`（ComNav ON） | **PASS** 8/8 | 68 |
| `… -B builds/p3b-gnss-asan-off -DSANITIZE=ON`（ComNav OFF） | **PASS** 7/7 | 56 |
| `cmake -S test/host -B builds/p3b-host` | **PASS** 7/7 | 79 |
| `cmake -S components/app_fw -B builds/p3b-app_fw`（卡写 test/app_fw，见偏离 7.1） | **PASS** 6/6 | 47 |
| `cmake -S components/stream_demux -B builds/p3b-stream_demux` | **PASS** 4/4 | 38 |
| `cmake -S test/bt_reconnect -B builds/p3b-bt_reconnect` | **PASS** 1/1 | 13 |
| `cmake -S test/services -B builds/p3b-services` | **PASS** 6/6 | 27 |
| `cmake -S test/services -B builds/p3b-services-asan -DCMAKE_CXX_FLAGS=…sanitize…` | **PASS** 6/6 | 27 |
| `cmake -S test/cli -B builds/p3b-cli` | **PASS** 3/3 | 22 |
| `cmake -S tools/main_host_check -B builds/p3b-mhc` | **PASS** 44/44 | 44 个检查目标 |
| `cmake -S tools/main_host_check -B builds/p3b-mhc-asan -DCMAKE_CXX_FLAGS=…sanitize…` | **PASS** 44/44 | 同上 |
| `cmake -S tools/t13_host_check -B builds/p3b-t13` | **PASS** 43/43 | 43 |

黄金向量等价性（`test/gnss/golden/PlanCheck.hpp`）：Unicore 324 个组合、ComNav 108 个、u-blox 108 个（3 Hz 组合跳过：面板只允许 1/2/5/10/20 Hz，驱动不认 3 Hz）；NMEA 驱动按**命令字节多重集**相等，u-blox 按 `(key, value)` 集合相等（GSV 键显式剔除，理由见 §3.3）。黄金文件未修改。

### 5.3 静态检查

| 命令 | 结果 |
| --- | --- |
| `python3 -I tools/g7b_check/g7b_check.py --build builds/p3b` | **PASS** |
| `python3 tools/tidy_check/tidy_check.py --host-build builds/p3b-host --idf-build builds/p3b --test-app-build builds/p3b-rtk_pure-esp32` | **PASS**（30 个文件，0 条） |
| `clang-format --style=file --dry-run --Werror <改动的 .cpp/.hpp/.inc>` | **PASS**（0 差异） |
| `sha256sum -c docs/frozen.sha256` | **PASS**（180 条全 OK） |

`tidy_check` 需要 `test_apps/rtk_pure` 的编译数据库，本次新建 `builds/p3b-rtk_pure-esp32`（`idf.py build`，0 warning）。

### 5.4 `rg` 残留（卡 §2.4）命令与逐条说明

`rg -n 'Unicore|Ublox|Comnav|ComNav|K803|SATSINFO|HEADINGA|UNLOG|COM%' main/` 结果：

| 位置 | 性质 |
| --- | --- |
| `main/modules/gnss/GnssModule.cpp:22` `#include "demux/decoders/UnicoreHash.hpp"` | 驱动登记表给出的解码器类型，main 只是把它们注册进 demux（ADR-0012 §2.3） |
| `main/modules/gnss/GnssModule.cpp:224` 注释 | 注释 |
| `main/modules/gnss/GnssModule.cpp:568` 注释 | 注释 |
| `main/data/GnssOutput.cpp:14` `{"GGA","SATSINFOA",…}` | 协议 `msg_id ↔ type_idx` 的用户可见名称表（`gnss_type_name`） |
| `main/data/GnssOutput.cpp:149-150`、`main/data/GnssOutput.hpp:97-100` 注释 | 注释（输出计划规则） |
| `main/modules/storage/ConfigCmds.cpp:127` `cli::print_kv("gnss_port", "COM%d", …)` | 控制台配置项展示，端口是 Kconfig 值 |

`family_name` 调用集中在 `TraceText.cpp::gnss_family_text()`（改用 `family_from_u8()` 做越界校验，不再写死 `kComnav`）。

## 6 修订的冻结文件

| 文件 | 修订内容 |
| --- | --- |
| `components/gnss/include/gnss/Receiver.hpp` | `OutputPlan` 增加 `rate_hz`；新增 `build_output()` 声明与文件头 P3b 修订说明 |
| `components/gnss/include/gnss/Controller.hpp` | `State::kOutput`、`ControllerStats` 掩码与计数、`init()` 增加 plan 参数、`request_output()/set_output_plan()/output_plan()/probe_receiver()` |
| `components/gnss/include/gnss/drivers/{Unicore,Ublox,Comnav}Receiver.hpp` | 新增 `build_output()`；Ublox/Comnav 另加 `output_mask()` |
| `main/def/GnssDef.hpp` | `GnssApply` 枚举与 `GnssStatus` 的 `output_seq`/`applied_mask`/`unsupported_mask`/`failed_mask`/`supported_mask`/`last_apply` |
| `components/link_core/include/core/services/GnssService.hpp` | `kGnssDefaultSupportedMask`、`set_supported_mask()/supported_mask()` 取代 `set_heading_supported()` |

`docs/frozen.sha256` 已按上表重算，全部 180 条校验通过。

## 7 与契约的偏离

1. **测试目录名**：任务卡写 `test/app_fw`，实际工程里该组件的 host 测试目录是 `components/app_fw`（P3a 已如此），按现有结构执行。
2. **u-blox 端口**：`build_output()` 忽略 `plan.port`，始终用已验证的 `CFG-MSGOUT-…-UART1` 键（P3a 基线同样忽略端口；其余端口的键值 UNKNOWN，不猜）。
3. **u-blox GSV 历元数**：由周期换算而非固定 1（见 §3.3），等价性比对中该键被显式剔除。
4. **关闭项的下发时机**：开机首轮跳过周期 0 的项（与黄金脚本逐字节一致），运行时 `request_output()` 才补发 UNLOG；`Unicore/ComNav` 的 `kOutput` 因此比基线多一种「关闭」命令能力。
5. ~~SATSINFOA 默认周期 5 s~~：审查时按卡 §1.1 改回 1 s（`-5` 只作 App 推送间隔），见 §10。
6. **`gnss out show` 新增列、`gnss status` 新增行**：为满足 §1.4 的可观测性要求，控制台文本有增量（协议帧不变）。
7. **测试新增文件**：`test/gnss/golden/PlanCheck.hpp`（等价性比对库）不在卡的必列文件里，但卡 §1.6 要求该比对，故新增；`golden_gen` 目标仍保留（`Dialect.hpp` 已删，目标不再生成）。
8. **`.clangd`**：host `PathMatch` 增补 `components/(gnss|stream_demux|bt_reconnect)` 与对应 `-I`，修复 gnss 头文件在 LSP 中无解析路径的问题（`.clangd` 不在冻结清单内）。

## 8 未解决问题与风险

- `components/gnss/src/Controller.cpp::poll()` 超过 `.clang-tidy` 的 `readability-function-size` 阈值（kOutput 分支加入后更明显）。该组件不在 `tools/tidy_check` 的检查范围内（脚本只覆盖 device_proto/link_core/spp_server/rtos/nvs_store、`main/*.cpp`、`test/host`、`test_apps`），故门禁仍 PASS；重构留待后续任务。
- pi-lens/clangd 会话缓存里长期残留 `components/gnss/src/NmeaParser.cpp`（20 条）与 `include/gnss/NmeaParser.hpp`（9 条）的「cascade 语法错误」，锚点是本机 LSP 早前没有 gnss 编译数据库时的结果。实测反证：`clangd --check=components/gnss/src/NmeaParser.cpp` 报 0 error；`g++ -std=c++20 -fsyntax-only -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` 编译该文件通过；`idf.py -B builds/p3b build` 与三套 host 构建同样通过。`lens_diagnostics` 的 LSP 探测对该文件始终 `inconclusive/timeout`（`elapsed-ms=0`），无法用它刷新缓存。按工具缺陷记录，未改任何冻结文件。
- RTCM 直通、`GnssAppSubState` 推送时序未改动，P3b 的 `supported_mask` 只影响订阅接受与否。

## 9 HIL 清单（供上板执行）

1. UM980 冷启动：`gnss status` 的 `applied=` 应含 GGA/RMC/GSA/GST/GPGSV（无 HEADINGA），`unsupported=0x04`（HEADINGA）。
2. `gnss out gsa -3` → `OK`，逐项显示 `gsa … receiver=on`；重启后 `gnss out show` 仍为 `-3`。
3. `gnss out heading 1` → `OK` 但显示 `unsupported`（UM980 无航向）；unsupported 不算失败，该值照常写 NVS（换双天线型号后生效）。
4. `gnss rate 5` → 手机侧 GGA 5 Hz；`gnss out gga 1` → GGA 1 Hz 且重启保持。
5. 手机侧订阅：0x04 `01 01`；0x1F `02 02`；0x05（SATSINFOA）在 ComNav/ublox 上应为 `02 02`。
6. `gnss out gsv 0` → 接收机 GSV 停止（运行时应看到 `UNLOG COM2 GPGSV`），重启后不再下发 GSV。
7. 断开后重连：kOutput 不下发、`output_seq` 不前进；RTCM 通路无回退。

## 10 审查与上板（Claude，2026-11-16）

审查修正（均已在本提交内）：
1. `make_output_plan` 默认规则与卡 §1.1 不符：GST 默认按 1 s、SATSINFOA 按 5 s 下发；而等价性测试 `PlanCheck.hpp::plan_from` 手写了旧计划，没有经过 `make_output_plan`，所以「与黄金等价」的结论对 main 实际计划不成立（rate>1 时 GST、以及 SATSINFOA 均不同）。改为 GGA/RMC/GSA/GST 默认 `1000/rate`、SATSINFOA 1000、HEADINGA 1000（有能力）；`GnssOutputCheck` 断言随之修正（10 Hz 下 GST=100、SATS=1000）。
2. 控制台判定竞态：原来看到 `output_seq` 前进就读 `last_apply`，而 `last_apply` 由随后的 `tick_pending_apply` 才写，可能读到上一次的结果。新增 `GnssStatus::apply_seq`（两阶段提交每次判定 +1），控制台等它前进再按 `last_apply` 给 OK / `ERR … reverted` / `ERR … save failed`。
3. 非运行态 `gnss out` 直接落盘却不推进任何序号，控制台必然 3 s 超时报错；现在同步判定（`apply_seq++`、`last_apply=kOk`）。`gnss rate` 非运行态同样直接落盘（原实现会超时回滚，无接收机时无法改频率）。
4. 上一次变更未判定时再次 `gnss out/rate` 会用暂存值覆盖 `pending_prev_*`，回滚到错误的值 → 现在返回 `kBusy`（`gnss busy`）。
5. 控制台等待上限 3 s 短于模块 `kApplyTimeoutMs` 5 s → 改为 6 s；`gnss rate` 原来不等待落地，改用 `submit_gnss_and_wait`。
6. 可观测性：`GnssStatus::rx_period_ms[6]`（当前计划下发给接收机的周期），`gnss out show` 显示 `push=`（App 推送间隔）与 `rx=`（接收机周期）两列；逐项结果对已 ACK 的关闭项显示 `off`（原来显示 `on`）。
7. 还原 agent 对 `.clangd` 的改动（卡外文件，且写死本机绝对路径）。

门禁复跑：idf `builds/p3b`/`p3b-off`/`p3b-comnav` 0 warning；main_host_check 44/44（含 ASan）、services 6、host 7、cli 3、t13 43、gnss ComNav ON 8（含 ASan）/OFF 7；G7b PASS；clang-format 0 差异；frozen 重算 0 失败。DRAM 64,199（较 5d2f927 +344），image 796,290。

上板 UM980 R4.10Build11833（/dev/ttyACM0）：
- 冷启动 `seq=1 applied=0x7B failed=0`，GGA/RMC/GSA/GSV 实测 1.0 Hz（10 s 计数），与 P3a 一致。
- `gnss out gst 5` + `gnss rate 5` → GGA/RMC/GSA 4.8–5.2 Hz、GSV 1 Hz、GST rx=200ms；**重新烧录重启后保持**（rate 5、gst 5）。
- `gnss rate 1` → 回到 1.0 Hz；`gnss out gga off` → GGA 0 Hz、其余 1 Hz；`gnss out gga 1` 恢复。
- `gnss out heading 1` → OK，heading `unsupported`，`unsupported=0x04`。
- `tools/hil/spp_gnss_sub.py F8:B3:B7:25:D8:B2` → 15/15 PASS（订阅 GGA 1 Hz、SATS 5 s、win16×1500 并发、断线清订阅、crc_bad=0）。
- 未在板上做：0x1F 订阅 `02 02`（脚本只覆盖查询 `02 02`，订阅分支由 test/services 字节级用例覆盖）；GSV 开关（控制台无 gsv 命令）；失败回滚路径（UM980 不会 NACK，仅主机测试覆盖）；u-blox/ComNav 无硬件。

P3b DONE
