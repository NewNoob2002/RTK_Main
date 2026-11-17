# P3c 记录：UM980 配置读回（ADR-0012 §2.7）

日期：2026-11-17。基线 `14469fa`（分支 `feat/gnss`；本任务未 commit）。执行者：pi + 本任务模型。
任务卡：`docs/tasks/P3c-config-readback.md`。

## 结论

**P3c PASS（软件门禁）**。`CONFIG` 读回链路按卡实现：解码器接受 `$CONFIG,` 行、Unicore 驱动提供
`build_readback`/`match_readback`、控制器新增 `kReadback` 状态与 1 s 静默 / 5 s 总时限、模块把每行正文
追加进 `gnss_readback_text` 并发布 `gnss_readback`、控制台 `gnss config` 打印原文与行数。冻结接口一处未改
（`UnicoreReceiver.hpp` 只加了卡允许的两个 override 声明），黄金向量字节未改，全部门禁通过。
上板/串口验证 NOT_RUN（AGENTS.md §6 未授权）。

### 上板必须先看的一件事（软件侧已实测的仲裁行为）

`CONFIG` 行能否交到 Unicore 驱动手里，取决于它的**校验和是否包含 `$`**：

| 行 | 校验和 | demux 结果 |
|---|---|---|
| `$CONFIG,COM1,GPGGA,1*31` | 标准 XOR（不含 `$`） | **nmea 认领**，UnicoreResp 收不到 → 读回必然是 0 行 |
| `$CONFIG,COM1,GPGGA,1*15` | 含 `$` 的变体 | nmea 校验失败 → 重扫 → **UnicoreResp 认领**（`unknown/dropped = 0`） |

`Nmea.cpp` 不限制 talker，而 demux 把 `$` 起点优先给表内靠前的候选（真实表序 `GnssModule.cpp:225-236`
是 `[nmea, rtcm3, driver…]`），所以只有后者能走到读回。ADR-0012 §2.7 记录的现场证据
（`byte 1 is 'C', expected 'c'`、约 436 B 被拒）正是这条「先被 NMEA 拒、再被响应解码器接手」的路径，
本卡的测试也按这条路径固定下来。**HIL 时请先确认 UM980 实际输出的 `$CONFIG` 行属于第二种校验和变体**；
若是标准校验和，需要另行决策（在 demux 表上把 Unicore 响应解码器排到 nmea 之前，或给 nmea 加白名单），
这超出本卡范围，本报告不改。

## 1. 改动文件

| 文件 | +/- | 内容 |
|---|---|---|
| `components/gnss/src/drivers/UnicoreReceiver.cpp` | +76/−10 | 响应解码器从字节 2 起接受 `"command,"` 或 `"CONFIG,"`（`check_prefix` 同时跟踪两条前缀，任一条失败都不算错；`ResponseVerdict` 新增 `alt`，`reject_prefix` 记录第二个期望字节，`explain_verdict` 打印 `expected 'c' or 'C'`）；新增 `kConfigPrefix = "$CONFIG,"`；`build_readback` 写 `CONFIG\r\n`；`match_readback` 只认 `kResponseDecoder` + `$CONFIG,` 起始，取 `$CONFIG,` 与 `*` 之间的正文（按 `out.size()` 截断） |
| `components/gnss/include/gnss/drivers/UnicoreReceiver.hpp` | +5/−0 | 卡允许的两条声明：`build_readback`/`match_readback` override（`Um982Receiver` 继承） |
| `components/gnss/src/Controller.cpp` | +69/−5 | 匿名空间新增 `kReadbackIdleMs = 1000`、`kReadbackTotalMs = 5000`；`ControllerImpl::on_readback`（`dispatch` 新增 `case State::kReadback`）先发一次读回命令，随后按「最后一行 +1 s」或「总时限 5 s」收尾回到 `kRunning` 并 `++stats_.readback_seq`；`request_readback` 只在 `kRunning` 且驱动 `build_readback` 成功时接受；`on_frame` 在 `kReadback` 下只对 `$CONFIG` 行计数并刷新静默计时（NMEA 照常往下传，不延长窗口，也不触发 kRunning 的静默重探） |
| `main/modules/gnss/GnssModule.cpp` | +110/−0 | `handle_frame` 在 `on_frame` 之前调用 `append_readback_line`（`match_readback` → `++readback_lines_`、D 级 `<cfg> RX %s`、正文 + `\n` 追加进 `gnss_readback_text`，放不下则丢弃该行并置 `truncated`）；`start_readback` 先探驱动能力以区分 `not supported` 与 `busy`；`publish_readback` 保证 seq 单调递增；`poll_controller` 收尾检测 `stats().readback_seq` 变化，打 `I` 级 `<cfg> Readback done, %u lines%s` 并发布一次；(起始 `TX "CONFIG"` 由既有 kWrite 日志分支输出，未重复) |
| `main/modules/gnss/GnssModule.hpp` | +32 | 三个新成员函数声明 + doxygen，四个读回账本成员（`readback_len_`/`readback_lines_`/`readback_truncated_`/`readback_base_seq_`/`readback_pub_seq_`） |
| `main/modules/gnss/GnssCmds.cpp` | +60/−2 | `gnss config`：`print_readback`（`not supported` / `busy` → `cli::fail`；成功打印正文 + `lines=<n>[ truncated]`）、`cmd_gnss_config`（记下 `gnss_readback.seq` → 提交 `GnssSubVerb::kReadback` → 轮询 seq 变化，最多 6 s，超时 `ERR timeout`）；`kGnssVerbs` 增项并更新顶层用法串 |
| `test/gnss/CMakeLists.txt` | +2 | 注册 `test_gnss_readback` |
| `test/gnss/TestGnssReadback.cpp` | 新增 681 行 | 见 §2 |

未改动（按卡 §3）：`NmeaParser.*`、`components/gnss/include/gnss/Controller.hpp`、`Receiver.hpp`、
`main/def/GnssDef.hpp`、`main/def/GnssSubDef.hpp`、`main/data/AppData.hpp`、`test/gnss/golden/*.inc` 全部黄金数据。

## 2. 新增测试用例（`test/gnss/TestGnssReadback.cpp`）

| 用例 | 断言 |
|---|---|
| `test_config_line_is_decoded` | `$CONFIG,COM1,GPGGA,1*15\r\n` 由 `kResponseDecoder` 一帧收下（11 步后 `kFrame`），逐字节喂入结果相同 |
| `test_command_line_is_still_decoded` | 既有 `$command,UNLOG COM2,response: OK*…` 仍被接受（本卡不退化解码器） |
| `test_config_line_rejects_bad_frames` | 坏校验和（`00`/`FF`/`A5` 三种变体）、`$CONFIX`、正文含 `$`、`> kResponseMaxFrame` 都被拒；`$CONF` 截断返回 `kNeedMore` 且已收字节在拒绝时清零 |
| `test_config_lines_coexist_with_nmea` | 24 行 `$CONFIG`（含 `$` 变体校验和）+ 24 帧 `$GNGGA` + 1 条命令应答，一次 4 KB 喂入与逐字节喂入：`$CONFIG` 全归 `UnicoreResp`、NMEA 全归 `nmea`、`unknown = 0`、每行一次 NMEA 拒绝（即交接）、共 49 帧 |
| `test_standard_checksum_config_line_is_claimed_by_nmea` | 记录上面那张表的第一种情形（标准校验和 → nmea 认领），把软件仲裁行为钉成回归 |
| `test_oversized_config_line_is_dropped` | 超长 `$CONFIG` 行被整体丢弃：`counters[1] == 0` 且 `stats().overflows > 0` |
| `test_build_readback_command` | Unicore 与 UM982 实例 `build_readback` 都产出 `CONFIG\r\n` |
| `test_match_readback_extracts_line` | 正文 `COM1,GPGGA,1`（12 字节）被取出；`$command,CONFIG,response: OK` 返回 0；换一个解码器返回 0；`out` 只有 4 格时截断返回 4 |
| `test_only_unicore_supports_readback` | u-blox / ComNav / Null 的 `build_readback` 均为 false |
| `test_controller_rejects_readback_before_running` | 上电未进 `kRunning` 时 `request_readback` 为 false 且状态不变 |
| `test_controller_ends_readback_after_idle` | 接受后下一次 `poll()` 写出 `CONFIG`；3 行后静默 1 s → 回 `kRunning`，`readback_seq + 1`、`readback_lines == 3` |
| `test_controller_nmea_does_not_extend_readback` | 只有 NMEA 流时不计行、不延长窗口（1 s 后收尾，`readback_lines == 0`） |
| `test_controller_total_window_caps_stream` | 每 400 ms 一行的连续流在 5 s 总时限收尾，`readback_lines == 13`、状态回 `kRunning` |
| `test_controller_ublox_readback_unsupported` | u-blox 驱动在 `kRunning` 下拒绝读回（`RTK_GNSS_DRV_UBLOX` 守卫） |

`main_host_check`/`t13_host_check` 都不编译 `main/modules/gnss/GnssModule.cpp`（工程内无 `GnssModule`
引用），因此卡 §2.5 的「模块文本拼接/截断」用例无法在 host 侧覆盖，只能靠上板观察（见 §4）。

## 3. 门禁结果

| 门禁 | 命令 | 结果 |
|---|---|---|
| host 测试（默认驱动） | `cmake -S test/gnss -B builds/p3c-gnss -G Ninja && cmake --build … && ctest …` | **PASS** 8/8 |
| host 测试（全驱动） | 同上 + `-DRTK_GNSS_DRV_COMNAV=ON`，`builds/p3c-gnss-all` | **PASS** 9/9 |
| ASan/UBSan（默认） | `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer -g"` 等，`builds/p3c-gnss-asan` | **PASS** 8/8 |
| ASan/UBSan（全驱动） | 同上 + COMNAV，`builds/p3c-gnss-all-asan` | **PASS** 9/9 |
| 其他 host 工程 | `test/host` 7/7、`test/app_fw` 8/8、`test/services` 6/6（ASan 6/6）、`test/stream_demux` 4/4、`test/bt_reconnect` 1/1、`test/cli` 3/3 | **PASS** |
| `tools/main_host_check` | `cmake -S tools/main_host_check -B builds/p3c-tools-main_host_check -G Ninja` | **PASS** 44/44（ASan 44/44） |
| `tools/t13_host_check` | 同上 | **PASS** 43/43（ASan 43/43） |
| IDF 构建 | `idf.py -B builds/p3c build`（`sdkconfig.defaults`，`CONFIG_RTK_TRACE_ENABLE=y`） | **PASS** 0 warning |
| 镜像大小 | `idf.py -B builds/p3c size` | 800 938 B；基线 `git archive 14469fa` → `/tmp/p3c-base` 同样构建 = 799 246 B，**+1 692 B** |
| 冻结清单 | `sha256sum -c docs/frozen.sha256` | 181 OK，**1 FAILED = `components/gnss/include/gnss/drivers/UnicoreReceiver.hpp`**（卡 §4.4 允许的唯一例外） |
| 格式 | `clang-format --style=file --dry-run --Werror …`（7 个改动文件 + 新测试） | **PASS** |
| 函数注释 | `python3 -I tools/doc_check/doc_check.py --base HEAD` | **PASS** 改动函数 17 个，不合格 0 个 |
| 堆分配白名单 | `python3 -I tools/g7b_check/g7b_check.py --build builds/p3c` | **PASS**（G7b PASS） |

构建过程中发现：仓库根有个被忽略的 `sdkconfig` 残留（P3s 的关 trace 实验留下），会让
`builds/p3c` 静默按 trace 关编译（782 918 B 的假象）。已把它移到 `/tmp/sdkconfig.p3s-traceoff-leftover`
并重建，上表数字是 `sdkconfig.defaults` 的新鲜配置。

## 4. 未解决 / 待上板验证

1. **`$CONFIG` 行的校验和变体**（见「上板必须先看的一件事」）：软件侧两条路径都有测试固定；真机若输出
   标准校验和，读回会 0 行且控制台 `ERR timeout`，需要另行决策 demux 仲裁顺序。
2. 真机 `gnss config`：应列出全部 `$CONFIG` 行、结尾 `lines=<n>` + `OK`，且 UART 侧不出现
   `unknown bytes dropped` 增长；`truncated` 只应在超长配置（>1536 B）时出现。
3. 读回期间 NMEA 流不断、`fix`/卫星槽持续更新（本卡测试用合成流覆盖，真机需确认 1 s/5 s 窗口手感）。
4. `GnssModule.cpp` 的两处**既有**问题按「不做无关重构」保留：`<string_view>`（:11）与
   `demux/decoders/UnicoreHash.hpp`（:22）在基线里就没用到；`on_command` 超出 clang-tidy 体量阈值
   （基线即如此，本次只加了 4 行分支）。

## 5. 与任务卡的偏离

- `UnicoreReceiver.hpp`：卡 §1.2 只允许在 `protected:` 加声明，但 `build_readback`/`match_readback` 是
  基类 public 虚函数的 override，只能放在 public 段（与 `build_probe`/`match_probe` 相邻）。冻结条目的
  语义未变（仍只有本卡允许的这两条新增），校验和因此变化，属卡 §4.4 列明的例外。
- 卡 §2.5 的 host 用例无法添加（工程不含 `GnssModule.cpp`），已在 §2 结尾说明。
- `main/` 改动与卡一致，未动 `Controller.hpp` 等冻结头；未 commit、未 flash。

## 6. Claude 验收（2026-11-17）

- 复核 `Nmea.cpp`：确无 5 字符地址规则，报告“只靠含 `$` 的校验和交接”结论成立；T0 写进 `UnicoreReceiver.hpp`
  的“nmea 按 5 字符地址拒收”注释是错的，已改为校验和交接说明并重算该头的冻结哈希。
- 删除 `GnssModule.cpp` 中 `append_readback_line` 上方残留的旧 `poll_controller` 注释块。
- 复跑：test/gnss 8/8、全驱动 ASan 9/9、main_host_check 44/44、t13_host_check 43/43、doc_check 0 不合格、
  clang-format 通过、frozen 全 OK、`idf.py -B builds/p3c build` 0 warning（rtk_main.bin 0xc3920 = 801 056 B，TRACE=y）。
- 待 HIL：UM980 `gnss config`（行数、无 `unknown bytes dropped`、`$CONFIG` 校验和变体）。
