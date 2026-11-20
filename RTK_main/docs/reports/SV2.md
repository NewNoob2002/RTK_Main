# SV2 设置服务、GNSS 能力和网页配置

分支 `feat/sv-settings`，基线 `1105c35`。接手前确认报告路径无同名文件及 Git 历史。工作区保留 AGY 添加的依赖表行，沿用并修订了其设置注册表和主机测试。

## 改动

| 文件 | 改动 |
| --- | --- |
| `AGENTS.md` | 保留 settings 依赖表行。 |
| `components/settings/CMakeLists.txt`、`include/settings/{Registry,Service,Types}.hpp`、`src/{Registry,Service}.cpp` | 16 键注册表，支持按调用方校验权限、工厂解锁、值与取值域，并提供快照读取和异步凭据。输出修改编码为单项补丁，归属模块以当时的配置为基础应用。 |
| `components/gnss/include/gnss/Receiver.hpp`、`include/gnss/drivers/UnicoreReceiver.hpp`、`src/Receiver.cpp`、`src/drivers/UnicoreReceiver.cpp` | 增加型号能力查询和 CM680 实现。缺少型号证据时返回空集合。CM680 驱动拒绝亚秒 GSA。 |
| `components/web_proto/CMakeLists.txt`、`include/web_proto/Writer.hpp`、`src/Writer.cpp` | JSON 输出器支持整数数组与能力范围对象。移除 `include/web_proto/KeyTable.hpp` 和 `src/KeyTable.cpp`，键表归入 settings。 |
| `main/CMakeLists.txt`、`main/AppContext.hpp`、`main/app/Executors.cpp`、`main/app/SettingsBackend.{hpp,cpp}` | 装配设置服务，将归属模块映射到 kLink 或 kIo 执行器。纯组件通过后端适配器使用 rtos，组件编译依赖为 app_fw。 |
| `main/data/AppData.{hpp,cpp}`、`main/def/GnssSubDef.hpp` | 增加 GNSS 能力、SN 快照与配置版本，删除网页 GNSS 专用命令、结果、取消及门控落盘通道。 |
| `main/modules/gnss/GnssModule.{hpp,cpp}`、`GnssModule{Cmds,Maintenance,Probe}.cpp` | 发布型号与有效周期，按执行时能力再次验证设置。完成接收机应答和持久化后通过执行器回执。保存或退出前强制落盘也走执行器。 |
| `main/modules/storage/StorageModule.{hpp,cpp}` | 发布已保存的 SN 和配置版本。 |
| `main/modules/web/WebLogic.{hpp,cpp}`、`WebModule.{hpp,cpp}`、`main/web/ui/app.js`、`docs/web/ws-protocol.md` | 网页设置走 Service 的 post/poll/cancel，按注册表生成 config。hello 包含 caps，能力变更后推送完整 caps。页面使用设备给出的选项和显示格式，保留乐观更新与失败回滚。 |
| `test/settings/{CMakeLists.txt,TestSettingsRegistry.cpp,TestSettingsService.cpp}`、`test/gnss/TestGnssUnicoreReceiver.cpp` | 验证设置权限、取值域、补丁编码、异步凭据及驱动拒绝非法周期。 |
| `test/web_proto/{CMakeLists.txt,test_web_proto_writer.cpp,test_web_proto_error.cpp}` | 原 `test_web_proto_key_table.cpp` 的键表覆盖迁入 settings，协议错误映射测试迁入 `test_web_proto_error.cpp`，并验证 caps 所需 JSON 结构。 |
| `tools/main_host_check/{CMakeLists.txt,GnssOutputCheck.cpp,GnssResetCheck.cpp,WebModuleCheck.cpp,WebServerCheck.cpp}` | 测试真实命令队列、取消、配置与能力推送、门控落盘失败和退出前重试。 |
| `tools/ci/run_gates.sh`、`tools/tidy_check/tidy_check.py`、`tools/web_mock/web_mock.py` | 设置测试纳入普通和 sanitizer 门禁，静态检查覆盖新组件。格式检查遍历所有仍存在的版本控制文件和未跟踪文件。模拟器提供 CM680 能力样例。 |

工作范围为 SV2。网页协议仍使用 v1，原有 config 字段保持原位置。手机报文与手机专用通道仍由现有流程处理。控制台保留既有同步入口。kLink 和 kIo 内的网页路径使用异步执行器接口，避免与蓝牙等待争用任务通知槽。

## AGY 交叉评审修订

- 网页门控在当前命令内提交单个 NVS 键，保存成功后才发布新配置。写入失败保持原门控对象、跨任务状态和放行标志，且该网页命令不会登记重试。控制台仍使用 500 ms 去抖和失败重试，网页退出可强制落盘已在等待的控制台设置。
- 定位或输出在途时重探、复位会恢复未保存的配置并用 `kInvalidState` 结单。已有自动保存、启动切速保存或维护凭据时，新的维护命令返回 `kBusy`。
- 设置服务将 `bt.pair_next` 作为布尔值读取，网页 config 帧沿用 v1 的整数 0/1。手机路径保存 SN 成功也会更新配置版本。
- CM680 能力查询仅对实际 GSV 返回周期。应用层将复用的卫星输出槽位映射到实际语句。网页步进控件使用 caps 提供的范围，频率标签最多保留两位小数。能力帧编码失败时等待下个版本再发送。

## 后续

- `kPatchOutput` 只下发改动的那条输出语句。
- 优化 `kConfiguring` 和 `kOutput` 阶段的 UART 读取等待，减少短 ACK 引起的固定等待。

## CM680 证据及待确认项

| 能力 | 本次发布的值 | 出处 |
| --- | --- | --- |
| 定位输出 | 1、2、5、10 Hz | 《CM680系列高精度模组产品数据接口协议 V1.2.8》表 1-1。指定手册未给出 20 Hz，能力位同步清除。 |
| GGA、GST、RMC | 关闭，100、200、500、1000 ms，再按当前定位频率过滤 | 手册表 1-1 的通用 LOG 周期、UNLOG 和 §6.2 的对应语句。 |
| GSA | 关闭，1000 ms | 手册 §6.2 的 1 s 示例。G-CM680 修订 B 的实板记录显示接收机拒绝 100、200、500 ms。 |
| GSV | 关闭，1000、5000 ms | 手册 §6.2 的 1 s 示例，ADR-0018 §1 的 5000 ms 实测记录。旧 `gnss/gsv=0` 仍关闭 NMEA GSV，此时域仅提供 0。 |

首次识别时仅修正驱动表外的旧 NVS 周期，优先选相邻较慢档，超出表上限时取最慢档。恢复会打 `FW_LOGW`。周期补丁超出当前域时返回 `bad_value`。定位频率降低及低频重启后，输出计划按当前定位周期限制实际输出，保留已保存的合法输出偏好。默认跟随定位的 CM680 GSA 以 1 Hz 输出。

待确认型号为 **UM980、UM981、UM982、u-blox ZED-F9P、ComNav K803**。需各型号与固件版本对应的定位频率、逐条语句输出周期和星座能力证据。CM680 的 20 Hz 与其余未列出的周期也待确认。手册 §5.2–5.3 记载 CM680 星座 MASK/UNMASK 命令。网页星座键当前仍为预留项，缺少已接入的设置归属命令和回读值。

## 冻结文件

- `AGENTS.md` 的依赖表行。
- `components/gnss/include/gnss/Receiver.hpp` 和 `components/gnss/include/gnss/drivers/UnicoreReceiver.hpp` 的能力查询。
- `components/web_proto/include/web_proto/Writer.hpp` 的 JSON 写入接口，以及移除的 `components/web_proto/include/web_proto/KeyTable.hpp`。
- `docs/web/ws-protocol.md` §2 的 caps 增量。

当前冻结清单包含上述前三个路径，即 AGENTS.md 和两个 GNSS 头文件。按本次验收约定，这三项差异允许存在，清单由 Architect 重算。

## 实机验收

- 用 CM680 在网页验证每条 NMEA 输出和各档定位频率。检查 GSV 0.2 Hz、GSA 1 Hz、低定位频率下的有效选项，以及域外请求的回执。
- 验证门控开关、最少卫星数、重启回读和 NVS 故障下退出反馈。验证 SN、白名单增删、下次启动配对、接收机保存及重启操作。
- Wi-Fi 配置模式测量运行时 `heap_min`，验收下限 40 KB。关注多会话和持续配置时的最低堆与执行器栈高水位。
- 用旧网页回归 v1，检查手机 0x06/0x33 与控制台冒烟测试。NONE 档加 Wi-Fi 的实机验收留给 Architect。

## 本地验证

本次使用 ESP-IDF v6.1 和 `idf6.1_py3.13_env`，关闭组件管理器。使用本机已缓存的 lizard 1.26.1。完整门禁在沙箱外运行以保留 LeakSanitizer，沙箱内试跑的 LeakSanitizer 因 ptrace 限制退出。最终门禁结果为 **37 PASS、1 FAIL**。日志在 `builds/sv2-logs/`。

| 命令或门禁 | 结果 | 证据 |
| --- | --- | --- |
| `tools/ci/run_gates.sh --base 1105c35 --prefix sv2` | FAIL | 唯一失败为验收允许的 G1 冻结项。详见 `gates.log`。 |
| G1 `sha256sum -c --quiet docs/frozen.sha256` | FAIL | `AGENTS.md`、`components/gnss/include/gnss/Receiver.hpp`、`components/gnss/include/gnss/drivers/UnicoreReceiver.hpp`。清单重算由 Architect 执行。 |
| G2 与 G2b | PASS | 26 项，含普通与 ASan/UBSan，以及 GNSS 全驱动构建。主机集成测试两档各 81/81。 |
| G3 与 G3b | PASS | 固件默认、trace-off、Wi-Fi-off、production、console-none 及 `rtk_pure` 两个目标，均零告警。 |
| G5 与 G6 | PASS | 格式通过，112 个改动函数的文档检查无违规。 |
| G5b 与 G7b | PASS | clang-tidy 覆盖 141 个源文件，0 条告警。运行时检查覆盖 130 个自有目标文件，0 条额外堆符号或守卫符号。 |
| `idf.py -B builds/sv2-idf size` | PASS | `idf-size.log`。 |
| `python -m esp_idf_size builds/sv1-idf/rtk_main.map` | PASS | `sv1-map-size.log`。 |
| `node --check main/web/ui/app.js`、`python3 -m py_compile tools/web_mock/web_mock.py` | PASS | JavaScript 和 Python 语法检查。 |
| `python3 tools/web_mock/web_mock.py --selftest` 及追加 `--caps-cm680` | PASS | 回环 HTTP 和 WebSocket。CM680 用例验证 GSV 5000 ms、拒绝 GSA 100 ms、降至 1 Hz 后推送新能力。 |
| `python3 tools/web_mock/pack.py --ui-dir main/web/ui --out /tmp/sv2-index.html.gz` | PASS | 独立打包 13,704 B，IDF 嵌入 gzip 13,719 B，均低于 24 KB。 |
| `git diff --relative --check` | PASS | 无空白错误。 |
| 实机、Wi-Fi 最低堆 | NOT_RUN | 需要 Architect 在实机验收。 |

## 内存与镜像大小

单位 B。SV 前数据取自 SV1 验收报告中对 `8c28078` 的测量，SV1 用保留的基线 map 和 bin 核对，SV2 用本次构建结果。

| 指标 | SV 前 `8c28078` | SV1 `1105c35` | SV2 | 相对 SV 前 | 相对 SV1 |
| --- | ---: | ---: | ---: | ---: | ---: |
| IRAM | 114,715 | 114,715 | 114,715 | 0 | 0 |
| DRAM | 97,483 | 97,515 | 97,659 | +176 | +144 |
| DRAM .data | 51,099 | 51,131 | 51,275 | +176 | +144 |
| DRAM .bss | 46,384 | 46,384 | 46,384 | 0 | 0 |
| ELF 估计镜像 | 1,531,178 | 1,531,986 | 1,536,882 | +5,704 | +4,896 |
| `rtk_main.bin` | 1,531,296 | 1,532,096 | 1,536,992 | +5,696 | +4,896 |

## Architect 验收（2026-11-20）

- 交叉评审：AGY（Gemini 4 Neon）`docs/reports/SV2-review-agy.md`，P1×1、P2×4、P3×4 已由 Codex 修订；Architect 抽查 `GnssModule::cmdApplyGgaGate()` 副本应用、写 NVS 成功后再提交，手册表号改为表 1-1。
- 门禁 37/38，唯一失败 G1-frozen 限于卡片授权的 `AGENTS.md`、`Receiver.hpp`、`UnicoreReceiver.hpp`，已重算 `docs/frozen.sha256`。
- P2 v1.41 实机（`builds/sv2-idf`，Wi-Fi 模式，单客户端 60 s，`tools/hil/web_load.py` 取自 feat/h1-heap-budget）：状态 60 帧 0 缺失；设置 ack 中位数 106 ms / P90 175 ms（修订前约 615 ms）；首页中位数 446 ms / P90 760 ms；内部堆 free 74,284 B、min 57,160 B、largest 69,632 B，无复位。本分支未含 H1 分构建门槛，故打印 `internal_budget FAIL (min>=81920)` 属预期；合入 H1 后按 40,960 B 判定为通过。
- 首页时延与 GNSS 单语句下发等性能项按用户要求待全部功能完成后处理。
