# G-A：gnss 纯逻辑组件实现 + 主机测试

- 任务卡：`docs/tasks/G-A-gnss-component.md`
- 分支：`feat/gnss`（未切分支、未 commit）
- 日期：2026-11-12
- 执行方：pi + workbuddy/deepseek-v4.1-flash

## 1. 改动文件

新增（全部在归属范围内）：

| 文件 | 说明 |
| --- | --- |
| `components/gnss/src/nmea_parser.cpp` | GGA/RMC/GSA/GSV → `Fix`/`SatTable`，整数定点、无浮点 |
| `components/gnss/src/dialects/unicore.cpp` | `$command` 应答解码、VERSIONA 探测、rover 脚本、ACK/NAK |
| `components/gnss/src/dialects/ublox.cpp` | UBX-MON-VER 探测、CFG-VALSET 三步、ACK/NAK |
| `components/gnss/src/dialects/comnav.cpp` | `<OK/<ERROR` 行解码、`LOG VERSIONA ONCE` 探测、NovAtel 风格脚本（实验性） |
| `components/gnss/src/controller.cpp` | 探测/配置/运行状态机（`kOff→kPowerUp→kProbing→kConfiguring→kRunning/kFailed`） |
| `components/gnss/src/registry.cpp` | `enabled_dialects()`/`dialect_for()`/`model_name()`/`family_of()` |
| `test/gnss/CMakeLists.txt` | 独立 host ctest 工程（Unity、`builds/*`、方言开关裁剪用例、`-DSANITIZE=ON`） |
| `test/gnss/test_gnss_nmea.cpp` | 9 个用例：GGA 字段/负坐标/无效质量、RMC、GSA used 合并、GSV 成轮、半轮不发布、畸形、reset、talker 映射 |
| `test/gnss/test_gnss_dialect_unicore.cpp` | 8 个用例：应答流式解码、拒绝/窗口、探测、型号/固件、脚本逐字节、ACK/NAK、能力位、与 NMEA 共存不抢帧 |
| `test/gnss/test_gnss_dialect_ublox.cpp` | 5 个用例：探测帧、MON-VER 解析、VALSET 键值/小端、Fletcher、ACK/NAK、能力位 |
| `test/gnss/test_gnss_dialect_comnav.cpp` | 6 个用例：`<OK` 解码、`LOG VERSIONA ONCE`、K803 识别、脚本逐字节、ACK/NAK、能力位 |
| `test/gnss/test_gnss_controller.cpp` | 9 个用例：完整流程、探测命中、超时耗尽、NACK/超时重试、切波特率、hint 优先、静默重探、回绕、状态名 |
| `test/gnss/test_gnss_registry.cpp` | 4 个用例：开关组合下的方言集合、family/model 映射 |

未改动：`components/gnss/CMakeLists.txt`、`components/gnss/Kconfig`、`components/gnss/include/**`、`main/**`、所有冻结文件。

## 2. 门槛结果

| # | 命令 | 结果 |
| --- | --- | --- |
| 1 | `cmake -S test/gnss -B builds/ga-host && cmake --build builds/ga-host && ctest --test-dir builds/ga-host` | **PASS**：5 个测试可执行、35 个用例，100% 通过 |
| 1b | `cmake -S test/gnss -B builds/ga-asan -DSANITIZE=ON && cmake --build builds/ga-asan && ctest --test-dir builds/ga-asan` | **PASS**：5/5，ASan+UBSan 无报告 |
| 2a | `cmake -S test/gnss -B builds/ga-off -DRTK_GNSS_DIALECT_UBLOX=OFF` + build + ctest | **PASS**：4/4（ublox 用例随方言裁剪） |
| 2b | `cmake -S test/gnss -B builds/ga-comnav -DRTK_GNSS_DIALECT_COMNAV=ON` + build + ctest | **PASS**：6/6（含 comnav 用例，41 个用例） |
| 3 | `IDF_COMPONENT_MANAGER=0 idf.py -B builds/ga build` | **PASS**：exit 0，全日志 `warning:` 计数 0（gnss 组件同样 0）；`idf.py size` 总镜像 742766 B（含 G-B 并行的 main 改动，非 G-A 纯增量） |
| 4 | `grep -rnE "\b(float\|double\|new\|malloc\|free\|printf\|sprintf\|snprintf\|sscanf\|strtod\|std::string\|std::vector)\b" components/gnss/src` | **PASS**：空结果（`"0.5"` 等只是命令文本/注释，无浮点类型） |
| 4b | `grep -rnE "#include [\"<](esp_\|freertos/\|driver/\|sdkconfig)" components/gnss/src` | **PASS**：空结果（无 IDF 头） |
| 5 | `clang-format --style=file --dry-run --Werror` 覆盖全部改动 `.cpp` | **PASS**（已 `-i` 后复检通过） |
| 5b | `sha256sum -c docs/frozen.sha256` | **PASS**：全部 OK，exit 0 |

主机测试构建在严格告警集（`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference -Wimplicit-fallthrough -Wundef -Wswitch-default -fno-exceptions -fno-rtti`）下零告警。

## 3. 实现要点

- **NMEA**：`ddmm.mmmmmmm` → 1e-9 度 `int64`（最多 7 位小数，防溢出）；高度/水准面 mm、DOP×100、差分龄期 0.1 s、速度 knots→mm/s；空字段保持未知值；GGA 是唯一更新 `Fix::update_ms` 的语句；GSV 按系统收齐后，在下一条 GGA 到达时整表发布并返回 `kSatsReady`（ADR-0010 §2.5），GSA 的 used 标记在发布时合并。
- **Unicore**：`$command,<echo>,response: OK*XX` 应答解码器与 `demux::nmea` 同时挂载时互不抢帧（测试用 `StaticDemux<nmea::kDecoder, kResponseDecoder>` 证明）；脚本顺序与 ADR §2.4 一致，频率参数 `1/0.5/0.2/0.1/0.05`；`CONFIG COMx <baud>` 不等应答并返回 `switch_baud`。
- **u-blox**：MON-VER 30 B 软件版本 + 10 B 硬件版本 + 30 B 扩展项栅格，`MOD=`/`FWVER=` 扩展；VALSET 使用 RAM|BBR 层；ACK/NAK 只认 CFG-VALSET(0x06,0x8A) 的应答。
- **ComNav**：`<OK`/`<ERROR` 行解码（CRLF，分块在 CR 后断开仍为 `kNeedMore`）；探测命令按 ADR §2.2 为 `LOG VERSIONA ONCE`；应答为 `#VERSIONA`（`demux::unicore_hash`）。
- **Controller**：所有时间比较用 `static_cast<int32_t>(now - deadline) >= 0` 回绕安全；一次一个在途命令；`Action::bytes` 指向成员缓冲；脚本步 NACK/超时重试 `step_retries`，探测全表失败 → 退避 → 电源复位；`switch_baud` 步发送后同步 `host_baud` 与 `Detected.baud`。

## 4. 与契约的偏离 / 需要下游注意

1. **`kSatsReady` 分支同时更新了 fix**：冻结头没有“待上报”标志，GGA 发布卫星轮时 `fix()` 已在同一次 `feed()` 中更新。G-B 接线时，`kSatsReady` 除写 `gnss_sats` 外也应写 `gnss_fix`，否则固定站/流动站的 `GGA` 定位在“恰好发布卫星轮”的那一历元会漏刷新（下一历元 `kFix` 会补上）。
2. **u-blox 键 ID 未实物验证**：源码注释标注出处为 “u-blox F9 HPG 1.32 Interface Description”的 CFG 键表（`0x30210001` RATE-MEAS、`0x10730004` UART1INPROT-RTCM3X、`0x10740002` UART1OUTPROT-NMEA、`0x209100BB/AC/BF/C3` MSGOUT GGA/RMC/GSA/GSV、`0x40520001` UART1-BAUDRATE）。仓库内没有该手册副本，HIL 前需按实物核对。
3. **ComNav 为实验性**：未取得官方手册与实物，`K803` 识别、`LOG/UNLOGALL/COM` 语法按公开 NovAtel 兼容资料实现；文件头已注明。
4. **测试工程按方言裁剪**：`RTK_GNSS_DIALECT_*` 关闭时对应方言的测试可执行文件不生成（否则会引用未编译的 `kDialect` 符号）。
5. **LSP 假阳性**：`.clangd` 的 host 编译数据库固定为 `builds/r7a-host`，不含 `components/gnss`/`test/gnss`，clangd 会报 “file not found / unknown type”。以 `cmake`/`g++` 构建为准；未改 `.clangd`（不在本任务授权内）。

## 5. 未解决 / 后续

- HIL（UM980）未执行（禁止打开串口/烧录），探测、配置、`gnss fix` 的实机验证留给联调阶段。
- u-blox/ComNav 方言需实物或官方手册复核（见 §4）。
- 等 G-B 完成 main 侧接线后，`gnss_fix`/`gnss_sats` 的 Slot 写入需按 §4.1 处理 `kSatsReady`。

G-A DONE
