# G-A gnss 纯逻辑组件实现 + 主机测试（ADR-0010）

执行方：pi + workbuddy/deepseek-v4.1-flash。分支 `feat/gnss`（不要切分支、不要 commit）。
与 G-B 并行：G-B 只改 `main/`、`components/link_core` 的 .cpp、`tools/`、`test/services`、`test/app_fw`；**你只改下面“文件归属”列出的路径**。

先完整阅读 `AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0010-gnss.md`、`docs/adr/ADR-0005*`（stream_demux），
再读冻结头 `components/gnss/include/gnss/{types,dialect,nmea_parser,controller}.hpp`、`components/gnss/include/gnss/dialects/*.hpp`，
以及 `components/stream_demux/include/demux/*.hpp`、`components/stream_demux/src/decoders/{nmea,unicore_hash,ubx}.cpp`、`test/stream_demux/`（host 测试写法参考）。
Unicore 命令手册文本：`/tmp/claude-1000/-home-gtc-Desktop-workspace-ESP32-PROJ-P2-RTK/84006cc8-9758-45f1-a393-77c17b49f97b/scratchpad/unicore.txt`（只读）。

## 文件归属（只允许新建/修改这些）

- `components/gnss/src/**`（nmea_parser.cpp、controller.cpp、registry.cpp、dialects/{unicore,ublox,comnav}.cpp）
- `test/gnss/**`（新建 host ctest 工程，写法同 `test/stream_demux`：add_subdirectory stream_demux 与 gnss，Unity 或现有测试框架同 test/stream_demux）
- `docs/reports/G-A.md`
- `components/gnss/CMakeLists.txt`、`components/gnss/Kconfig` 仅在确有构建错误时可改，须在报告说明。

## 要做

1. **registry.cpp**：`enabled_dialects()` 按 `RTK_GNSS_DIALECT_*` 宏返回 `{unicore, ublox, comnav}` 中已启用者（顺序固定）；`dialect_for(Family)`、`model_name`、`family_of`；`caps(model)` 由各方言实现（见 ADR §2.3）。
2. **nmea_parser.cpp**：按 ADR §2.5 与 `nmea_parser.hpp` 注释。输入是 `demux::nmea` 已校验的完整语句（含 `$`、`*hh`、`\r\n`）。无浮点、无堆、无 `std::string`、无 `sscanf/strtod`；纬经度 1e-9 度 int64；空字段保持未知值；GSV 多系统成轮逻辑；GSA used 合并。
3. **dialects/unicore.cpp**：`kResponseDecoder`（`$command,` 应答；与 `demux::nmea` 同时挂载时互不抢帧——写测试证明），VERSIONA 探测与型号/固件解析（`#VERSIONA` 由 `demux::unicore_hash` 解出），rover 脚本（ADR §2.4，频率参数 1/0.5/0.2/0.1/0.05），`match_reply`：`$command,<发送的命令去掉\r\n>,response: OK*hh` → kAck，同命令其他 response → kNack，不同命令 → kNone（以手册 §应答格式为准，大小写按手册）。
4. **dialects/ublox.cpp**：MON-VER 轮询帧（含 UBX 校验和）、`MOD=` 扩展串 → `kZedF9p`、固件 `FWVER=`；VALSET 三步（键 ID 按 u-blox F9 HPG 1.32 接口描述，键值小端；在源码注释列出每个键 ID 的出处）；ACK-ACK/NAK 匹配 cls/id = CFG-VALSET (0x06,0x8A)。
5. **dialects/comnav.cpp**：实验性，`<OK`/`<ERROR` 行解码器 `kPromptDecoder`、`LOG VERSIONA ONCE` 探测、NovAtel 风格 rover 脚本（`UNLOGALL COM<port>`、`LOG COM<port> GPGGA ONTIME <1/rate>` …）；文件头注明未经手册与实物验证。
6. **controller.cpp**：ADR §2.6 状态机与 `controller.hpp` 注释；所有时间比较回绕安全；`Action::bytes` 指向成员缓冲；一次一个在途命令；静默检测；失败退避后电源复位。不得阻塞、不得分配内存。
7. **test/gnss**：至少覆盖 ADR §4 主机一节列出的场景；每个方言用固定字节数组断言命令逐字节输出；NMEA 用 UM980 实录风格语句（7 位小数、负坐标、空字段、GN/GB talker）；Controller 用假时钟驱动完整流程（探测命中第 N 个波特率、hint 命中只发一条查询、全表失败→kFailed→退避→kPowerUp、配置 NACK 重试、超时重试耗尽→kFailed、switch_baud 步、kRunning 静默→重探、`now` 在 0xFFFFFFFF 附近回绕）。同时提供 ASan/UBSan 构建（`-DSANITIZE=ON` 或同 test/stream_demux 的方式）。

## 门槛（写入 `docs/reports/G-A.md`，逐项 PASS/FAIL 附命令与输出摘要）

1. `cmake -S test/gnss -B builds/ga-host && cmake --build builds/ga-host && ctest --test-dir builds/ga-host` 全过；ASan/UBSan 版本（`builds/ga-asan`）全过；列出用例数。
2. 方言开关组合：host 下 `-DRTK_GNSS_DIALECT_UBLOX=OFF`、`-DRTK_GNSS_DIALECT_COMNAV=ON` 各构建并测试一次通过。
3. `. ~/esp/esp-idf/export.sh && IDF_COMPONENT_MANAGER=0 idf.py -B builds/ga build`：gnss 组件 0 告警（若此时 G-B 的 main 部分尚未完成导致 main 编译失败，记录失败点属于 main 即可，不要去改 main）。
4. 组件内无 `float/double`、`new/malloc`、`std::string`、`printf`、IDF 头（给出 `grep` 命令与空结果）。
5. clang-format 覆盖改动文件；`sha256sum -c docs/frozen.sha256` 全 OK（不得改冻结头）。

报告最后一行 `G-A DONE` 或 `G-A BLOCKED: <原因>`。若发现冻结头有错误或无法实现，**停止并在报告写明**，不要自行修改冻结头。

## 禁止

修改冻结文件与归属外文件；烧录、打开串口、联网安装、git commit/切分支。构建目录只用 `builds/ga*`。
