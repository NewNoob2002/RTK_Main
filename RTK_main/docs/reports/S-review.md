# S 系列独立复审

复审范围为 `2ac54de..3aadc68` 的 S1–S5-B3，共 28 个提交。行号以 `3aadc68` 为准，第 4 条使用对应历史提交的行号。工作区 S5-B4 由另一位评审者验收。构建和主机复现均在 `builds/srev-*` 的 Git 快照中执行。本次交付文件为 `docs/reports/S-review.md`。

**结论**　目标提交有三项高严重度回归，分别影响配对故障追踪、`status overview` 快照和 H1/H2 自测。另有一处在后续提交修复的中间提交编译断点。两种 ESP32 编译配置均通过。

## 高严重度

1. **PIN 和 SSP 确认投递失败后漏报追踪**（高，`898d782`，`components/spp_server/SppServerSession.c:417`）

   旧版在所有控制请求投递之后检查 `err`，仅 `REJECT` 需要持锁结算。拆分把非 `REJECT` 提前 `continue`，使 `esp_bt_gap_pin_reply()` 和 `esp_bt_gap_ssp_confirm_reply()` 的错误均跳过 `SPP_SERVER_TRACE_CTRL_ERR`。启用 TRACE 的主机对照中，令两个 SDK 调用均返回 `ESP_FAIL`，两版都各调用一次。父提交 `6d3f046` 各上报一次，`3aadc68` 均上报零次。建议将锁内结算放到 `if (req.type == SPP_SERVER_CTRL_REJECT)` 内，随后统一检查 `err`，并补这两个失败分支的断言。依据为 S4 §2 的错误输出保持要求。

2. **一次状态概览可混用两代系统快照**（高，`327f5cd`，`main/modules/system/SystemStatusCmds.cpp:155`、`main/modules/system/SystemStatusCmds.cpp:199`、`main/modules/system/SystemStatusCmds.cpp:243`）

   父提交 `a9bac39` 的 `main/modules/system/SystemCmds.cpp:182` 在首行输出前只读一次 `systemStatus`，同时用于 `[link]` 的运行状态与 `[sys]` 的堆统计。拆分后分别读取，中间已打印 `[link]` 和 `[rtcm]`。当串口发送缓冲已满而控制台等待输出时，`SystemModule::poll()` 可以更新 `systemStatus`（`main/modules/system/SystemModule.cpp:105`），单次输出便可同时展示旧运行状态与新堆统计。RTCM 与执行器快照也从首行输出前移到了对应段落开始处。建议在 `cmdStatusOverview()` 的原有位置一次采样，把每段需要的值传给打印函数，保留原先位于 `[sys]` 中间的 GNSS 专属采样时点。该回归由调用顺序和并发写路径确认。

3. **改名使 H1/H2 独立自测无法编译**（高，`2c2670a`，`test/stream_demux_dev/h1/Selftest.cpp:116`、`test/stream_demux_dev/h1/Selftest.cpp:324`、`test/stream_demux_dev/h2/H2Check.cpp:57`、`test/stream_demux_dev/h2/H2Check.cpp:141`）

   H1 更新了部分 `onFrame` 覆盖，却仍用 `on_unknown`、`index_of`、`reset_stats` 等旧名。H2 的独立 CMake 自测仍引用 `first_byte`、`max_frame`、`crc32_novatel`、`msg_id` 等旧名。用同一主机编译器对照父提交 `8145016`，H1 语法检查通过，H2 构建并运行得到 12019 通过、0 失败。目标提交两者均因旧名编译失败。建议按实际编译诊断同步两个调用方，再运行自测。H2 原验收报告第 8 项的独立自测入口提供复验命令，S2 卡要求改名同步调用方。

4. **S1 的 T0b 单独检出时无法编译**（高，历史提交 `3d7c9f5`，`components/gnss/include/gnss/DriverList.inc:8`、`components/gnss/src/Registry.cpp:21`）

   注册表宏已经拼出 `kUm980Receiver`、`kZedF9pReceiver`、`kK803Receiver`，当时 `components/gnss/src/DriverInstances.hpp:43` 等处仍声明家族实例。用三家驱动宏均为 1 直接对该提交的 `Registry.cpp` 做语法检查，三个叶子实例均报未声明，默认启用的 Unicore 和 u-blox 也会遇到同一问题。`a25b636` 补齐了定义，目标提交的全驱动构建通过。若这组改动需要重新组织成可独立构建的提交，建议同一提交切换登记项、声明和定义。依据为 ADR-0012 §2.8 的叶子登记契约。

## 中严重度

5. **编译期诊断仍显示已改名的 C++ 符号**（中，`2c2670a`，`components/stream_demux/include/demux/StaticDemux.hpp:26`、`components/stream_demux/include/demux/StaticDemux.hpp:102`）

   误用未登记的解码器时，`indexOf()` 的断言仍显示 `StaticDemux::index_of`。帧容量断言仍显示 `max_frame`，条件已读取 `maxFrame`。两条文本用于定位 C++ 符号。建议经任务卡授权单独更新这两条诊断，S2 卡原先要求保留字符串字面量。检查到的 CLI 输出字段、场景名、trace 模板键和匹配值应继续使用各自现有拼写。

6. **GNSS 注册入口仍指向已拆除的清单和访问器**（中，`05bbc97`，`docs/adr/ADR-0012-gnss-receiver-drivers.md:132`、`docs/adr/ADR-0012-gnss-receiver-drivers.md:197`、`docs/adr/ADR-0012-gnss-receiver-drivers.md:202`、`components/gnss/include/gnss/Receiver.hpp:123`）

   S3 删除了 `DriverList.inc` 和驱动访问器，当前表位于 `components/gnss/src/Registry.cpp:20`、实例声明位于 `DriverInstances.hpp`。跟随旧说明登记新驱动或调用 `um982Receiver()` 会找不到对应入口。建议在 ADR-0012 列出 S3 的登记路径，并把 API 注释改为基准驱动用 `receiverFor()`，型号差异通过 `forModel()` 选择。依据为 ADR-0013 §2.3。

7. **FanoutRing 初始化锁与未初始化返回码说明不完整**（中，`4af5a44`，`components/app_fw/include/fw/FanoutRing.hpp:90`、`components/app_fw/include/fw/FanoutRing.hpp:107`、`components/app_fw/include/fw/FanoutRing.hpp:150`、`components/app_fw/include/fw/FanoutRing.hpp:169`）

   `init()` 与 `initTyped()` 在单线程启动期保存锁表，不调用其 lock/unlock（`components/app_fw/src/FanoutRing.cpp:112`、`components/app_fw/src/FanoutRing.cpp:250`）。`detach()` 和 `setMask()` 在默认构造实例上均先返回 `kInvalidState`，公开返回值清单遗漏了这一分支（同文件第 163、314 行）。建议将初始化的 `@note` 改为绑定锁表，为两个方法补 `kInvalidState`。依据为 CODING-STYLE §6.2。

8. **SPP 事件结算的锁所有权和丢弃计数注释写反**（中，`898d782` 引入于拆分，`ed65699` 整理后仍存，`components/spp_server/SppServerRx.c:144`、`components/spp_server/SppServerRx.c:275`）

   `spp_server_settle_event()` 在第 279、299 行自行进出 `s_spp_mux`，`spp_server_poll_event()` 在第 328 行未持锁调用。按“需持锁”理解会把调用方持锁和锁内工作的约束混为一谈。队列满时，`spp_server_clear_pending_locked()` 返回的是全局丢失事件计数 `s_spp.evt_dropped`，第 204 行把它写入 trace。会话丢失字节数独自在第 291 行写入 RX_DROP。建议分别注明“函数内部持锁”和“返回驱动累计丢失的队列事件数，用于 trace”。依据为 CODING-STYLE §5.3、§6.2。

9. **NMEA 的 `kMalformed` 会增加统计**（中，`3aadc68`，`components/gnss/include/gnss/NmeaParser.hpp:51`）

   截断帧和字段错误均会增加 `stats().malformed`（`components/gnss/src/NmeaParser.cpp:113`、`components/gnss/src/NmeaParser.cpp:138`）。当前“未改变任何状态”的保证与公开可见的统计副作用冲突。另建议写明 `kSatsReady` 由 GGA 发布卫星表时返回，因此当次也更新 `fix()`（`components/gnss/src/NmeaParserPosition.cpp:204` 至第 207 行）。依据为 CODING-STYLE §6.2。

10. **Controller 注释误导候选生命周期和输入帧边界**（中，`3aadc68`，`components/gnss/include/gnss/Controller.hpp:190`、`components/gnss/include/gnss/Controller.hpp:221`）

    `probeReceiver()` 只检查候选指针，成功转入配置或探测失败时仍可返回最近发送查询用的驱动（`components/gnss/src/Controller.cpp:210`），与“未在探测中时为 nullReceiver()”冲突。`onFrame()` 收到的 span 直接来自 demux（`components/stream_demux/src/Demux.cpp:104`、`main/modules/gnss/GnssModuleFrames.cpp:114`），UBX 的同步字等协议前缀也在 span 内。按注释剥掉前缀后，驱动不能按原协议偏移识别响应。建议按实现描述“候选指针为空时才回落”，并将 `frame` 写为分流器交付的完整帧（含同步/起始字节）。依据为 CODING-STYLE §6.2。

11. **OPEN/CLOSE 入队前的临界区范围改变**（中，待确认，`898d782`，`components/spp_server/SppServerRx.c:181`）

    原实现对每个事件类型先进入 `s_spp_mux`，现实现仅在 DATA/RX_DROP 时进入第一段临界区。OPEN/CLOSE 在首段没有共享状态读写，生产者仍在 `SppServerGap.c:73`、第 98 行调用前发布会话状态，现有 T13 生命周期测试通过。未确认实际时序回归。S4 §2 明确要求锁范围保持一致，建议恢复原位置，或由维护者确认移除这次同步是有意的。

## 逐提交核对

| 批次 | 提交与核对结果 |
| --- | --- |
| S1 | `75e1445`、`3d7c9f5`、`a25b636`。检查三家族的 protected 非虚析构、四个 final 叶子的平凡析构断言、constinit 实例、型号选择与探测顺序。中间提交断点见第 4 条。 |
| S2 | `5170aa0`、`031f958`、`d613ef7`、`2bad7da`、`8145016`、`2c2670a`、`1b957f0`、`3befb87`、`d01d76a`、`518fecc`、`9ad8acc`、`9042318`。核对改名表、非注释 token、字面量、命令表和拼接宏。归档 `GoldenGen.cpp:261` 的诊断文本随符号更新，现行 golden 向量未改。遗漏见第 3、5 条。 |
| S3 | `7add8a9`、`05bbc97`。检查注册表常量初始化、三个驱动开关、叶子实例地址和顺序、`takeField` 内联替换及其短路语义。文档遗漏见第 6 条。 |
| S4 | `0132eec`、`7d3c1cf`、`6d3f046`、`898d782`、`a9bac39`、`327f5cd`、`d97e9b2`。按函数体 token 匹配搬移项，对变化的函数回读调用关系，检查条件取反、早退、数据寿命、常量初始化、锁和输出次序。`d97e9b2` 已修掉重复的 `[gnss]`，目标提交的 `printGnssCore()` 只输出一次（`main/modules/gnss/GnssCmdsStatus.cpp:56`）。`6d3f046` 是同区间的协议提案，无固件实现改动。 |
| S5 | `319bba3`、`4af5a44`、`ed65699`、`3aadc68`。B1、B2 的 C/C++ 非注释 token 无变化，B3 的两份 u-blox 源文件只调整常量限定。抽查返回值、输出参数、持锁和任务/ISR 文档，发现见第 7–10 条。 |

## 验证记录

| 命令或检查 | 结果 |
| --- | --- |
| `git log --reverse 2ac54de..3aadc68`、逐提交 `git show`、`git diff --check 2ac54de..3aadc68 -- .` | PASS，已完成提交对照，diff 无空白错误。 |
| `git -C .. archive 3aadc68 RTK_main` 后在 `builds/srev-src` 执行 `sha256sum -c docs/frozen.sha256` | PASS，187 项。 |
| ESP-IDF 6.1，`. ~/esp/esp-idf/export.sh`，`IDF_COMPONENT_MANAGER=0 idf.py -C builds/srev-src -B builds/srev-nognss -DSDKCONFIG=… -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;review-nognss.defaults' build` | PASS。配置确认 GNSS 关闭（`RTK_GNSS_ENABLED=0`），TRACE 开启。日志 `builds/srev-nognss.log`。 |
| 同命令用 `builds/srev-traceoff-all` 和 `review-traceoff-all.defaults` | PASS。配置确认 GNSS 和 Unicore、u-blox、ComNav 开启，TRACE 关闭。日志 `builds/srev-traceoff-all.log`。两份构建均无编译器 `warning:`，配置阶段各有 6 条 CMake 提示（CMP0225 和 IDF 组件私有 include 校验）。 |
| `cmake -S builds/srev-src/test/gnss -B builds/srev-gnss-all -G Ninja`，三家驱动均为 ON，再 build 和 ctest | PASS，9/9。 |
| `cmake -S builds/srev-src/tools/t13_host_check -B builds/srev-t13 -G Ninja`，再 build 和 ctest | PASS，43/43。现有 GAP 场景的 SDK 桩返回成功。 |
| 为 `6d3f046`、`3aadc68` 分别构建 T13 SPP 驱动（`-DCMAKE_C_FLAGS=-DCONFIG_RTK_TRACE_ENABLE=1`），用 `cc` 和链接器 `--wrap` 注入两种 GAP 失败，再执行 `ctrl-check` | 父提交 PASS，目标提交 FAIL。重现源与结果在 `builds/srev-ctrl-check.c`、`builds/srev-ctrl-{s4-parent-src,src}/result.log`。 |
| H1 `g++ -std=c++20 -fsyntax-only`，H2 `cmake -S …/test/stream_demux_dev/h2`、build、运行 `h2_check` | 父提交 `8145016` PASS，`3aadc68` FAIL。日志 `builds/srev-{h1,h2}-{s2-parent-src,src}.log`。 |
| `g++ -std=c++20 -fsyntax-only` 编译 `3d7c9f5` 的 `Registry.cpp`，三家驱动宏均为 1 | FAIL，复现中间提交三个未声明符号。日志 `builds/srev-s1-registry.log`。 |
| `python3 -I builds/srev-src/tools/g7b_check/g7b_check.py --build builds/srev-<配置>`，`xtensa-esp32-elf-nm` 检查两个 `libgnss.a` | PASS，两份 G7b 通过，`D0Ev`、`_Zdl`、`_Zda`、`__cxa_atexit` 和 `_GLOBAL__sub_I` 匹配均为 0。 |
| 全量 ASan/UBSan、G5b、完整格式与 Doxygen 门禁 | NOT_RUN。本次使用针对行为差异的对照、两种固件构建及上述主机套件。S5 契约为人工抽查。 |
| HIL、串口、高负载时序 | NOT_RUN。实机影响未独立验证，OPEN/CLOSE 同步时序见第 11 条。 |

## 修复

实施基线为 `155d81b`。按 Architect 裁定处理复审第 1–11 条。

| 发现 | 修复结果 |
| --- | --- |
| 1 | `components/spp_server/SppServerSession.c` 仅对 REJECT 持锁结算，所有控制请求在锁外检查投递结果并上报 `CTRL_ERR`。 |
| 2 | `main/modules/system/SystemStatusCmds.cpp` 恢复首行输出前的系统、链路、RTCM、分流和链路执行器采样。link 与 sys 使用同一份系统状态。GNSS 与 IO 执行器仍在 sys 的链路执行器统计输出后采样，与 `a9bac39` 一致。 |
| 3、5 | `test/stream_demux_dev/h1/Selftest.cpp` 和 `test/stream_demux_dev/h2/H2Check.cpp` 同步现行符号，`components/stream_demux/include/demux/StaticDemux.hpp` 更新 `indexOf` 和 `maxFrame` 断言文本。 |
| 4 | `3d7c9f5` 的独立编译断点保留为历史记录，已有 `a25b636` 修复。 |
| 6 | `docs/adr/ADR-0012-gnss-receiver-drivers.md` 和 `components/gnss/include/gnss/Receiver.hpp` 说明现行注册表、叶子实例及按家族和型号选择的入口。 |
| 7–10 | `FanoutRing.hpp`、`SppServerRx.c`、`NmeaParser.hpp`、`Controller.hpp` 按实现订正返回值、持锁责任、统计副作用、候选生命周期和帧范围。 |
| 11 | **有意，Architect 裁定**。OPEN/CLOSE 在首段无需操作共享 pending 状态，入队由 `xQueueSend` 自身保护。`SppServerRx.c` 已补充注释。 |

| 验证命令或检查 | 结果 |
| --- | --- |
| `. ~/esp/esp-idf/export.sh` 后运行 `IDF_COMPONENT_MANAGER=0 idf.py -B builds/srev-fix -DSDKCONFIG="$PWD/builds/srev-fix/sdkconfig" build`，使用现有配置副本 | PASS，0 条编译器告警。配置阶段仍有 6 条 CMake 告警，涉及 CMP0225 和 IDF 组件私有 include。日志 `builds/srev-fix-idf.log`。 |
| `python3 -I tools/tidy_check/tidy_check.py --host-build builds/s4v-host --idf-build builds/srev-fix --gnss-build builds/s5v-gnss-all --test-app-build builds/q4-rtk_pure-esp32 --jobs 4` | PASS，110 个文件，0 条发现。日志 `builds/srev-fix-tidy.log`。 |
| `clang-format --style=file --dry-run --Werror` 检查仓库自有 C/C++ 源文件，按仓库规则排除参考代码与 H2 生成向量 | PASS，0 处差异。 |
| `python3 -I tools/doc_check/doc_check.py --report` 和 `--base HEAD` | PASS，1342/1342（100%），7 个改动函数均合格。 |
| `cmake -S <套件目录> -B builds/srev-fix-<套件名> -G Ninja`，再 build 和 ctest | PASS，`main_host_check` 44/44，`t13_host_check` 43/43，`test/services` 6/6，`test/host` 7/7，`test/gnss` 三家驱动全开 9/9。 |
| `g++ -std=c++20 -Icomponents/stream_demux/include -fsyntax-only test/stream_demux_dev/h1/Selftest.cpp` | PASS。 |
| `cmake -S test/stream_demux_dev/h2 -B builds/srev-fix-h2 -G Ninja`，再 build 并运行 `builds/srev-fix-h2/h2_check` | PASS，12019 通过，0 失败。 |
| 使用 `builds/srev-ctrl-check.c` 在主机 TRACE 构建中以链接器 `--wrap` 注入 PIN 与 SSP 确认失败 | PASS，两种 SDK 调用和对应 `CTRL_ERR` 各记录一次。结果 `builds/srev-fix-gap/result.log`。 |
| 与 `a9bac39` 对照 `status overview` 各段的快照读取和输出调用 | PASS，首行前采样次序、GNSS 采样点及控制台输出调用一致。 |
| `sha256sum -c docs/frozen.sha256` 与 `git diff --check` | 冻结哈希 FAIL，仅有 5 个授权差异，分别为 `FanoutRing.hpp`、`Controller.hpp`、`NmeaParser.hpp`、`Receiver.hpp` 的注释及 `StaticDemux.hpp` 的两个断言文本。哈希表保留原值。diff 空白检查 PASS。 |
| HIL | NOT_RUN。 |

## Architect 验收（Claude，2026-11-18）

- 逐项核对修复 diff：#1 `spp_server_ctrl_process` 结算仅限 REJECT、错误追踪对全部类型恢复；#2 `cmdStatusOverview` 输出前一次采样并传入各段；
  #5 `StaticDemux.hpp` 仅改 `static_assert` 文本；其余冻结头（FanoutRing/Controller/NmeaParser/Receiver）去注释 token 比对 0 代码改动。
- #11 裁定为有意：原临界区内无共享状态，`xQueueSend` 自带同步，已加注释。#4 历史提交不改写。
- 门禁：idf `builds/srev-fix` 0 warning，bin 800592 B；G5b 0；format 0；doc_check 1342/1342；主机套件全过；H2 12019 通过；frozen 已重算。
