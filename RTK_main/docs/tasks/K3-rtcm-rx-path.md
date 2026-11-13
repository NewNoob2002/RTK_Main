# K3：RTCM 直通与链路 RX 唤醒/公平调度

状态：**ACCEPTED（待实施）**。设计决定已确认；实施与冻结契约定稿另行下达。依据：[ADR-0011](../adr/ADR-0011-data-path-memory.md) §3、§4.2/4.5、§6.1、§8～12及[修订指令](ADR-0011-revise.md)。

## 1. 目标与前置

从会话Demux直接将PHONE RTCM整帧送入RtcmPipe，移除大批次与SessionSink负载队列；RtcmModule仅统计/控制。SPP DATA_IND立即唤醒kLink，单次read256～512B，保留K2回复反压和双会话公平。

前置：K2通过非阻塞接纳/反压/生命周期验收，新的RX通知和小统计快照契约定稿。Q4字节窗口已接受；本卡新增上行字节窗口压测模式，验证up256窗口内零丢。此卡不引入App输出或boot堆。

## 2. 范围与允许修改文件

| 文件 | 范围 |
| --- | --- |
| `components/spp_server/spp_server_rx.c`、`spp_server_session.c`、`spp_server_init.c`、`private_include/spp_server_priv.h` | DATA/RX_DROP level状态、立即通知、关闭/等待竞态；保持回调不阻塞 |
| `main/modules/bt_link/bt_link_module.hpp`、`.cpp` | RX512B、预算/轮转/尾部保留、Demux直通、pending产品帧；移除旧SessionSink队列/批次 |
| `main/def/rtcm_def.hpp`、`main/data/app_data.hpp`、`.cpp` | 删除RtcmBatch类型/槽，新增小计数epoch；保持原status字段 |
| `main/data/rtcm_path.hpp`、`.cpp`（必要时新增） | kLink独占通路helper/快照，禁止借此跨调用模块 |
| `main/modules/rtcm/rtcm_module.hpp`、`.cpp`、`rtcm_forwarder.hpp`、`.cpp`、`rtcm_cmds.cpp` | 只统计、reset、速率；源码原文件不改名，维持冻结host可链接 |
| `main/modules/gnss/rtcm_pipe.hpp`、`gnss_module.hpp`、`.cpp` | 整帧push、pipe统计、UART pending后缀与故障处理；UART RX/TX预算公平 |
| `main/modules/debug/trace_text.cpp`、`main/modules/system/system_cmds.cpp` | 追加诊断，旧两行逐字段保留 |
| `main/CMakeLists.txt`、`components/spp_server/CMakeLists.txt` | 必要源文件接入/strict warnings |
| `tools/main_host_check/**`、`tools/t13_host_check/**`、`test/gnss/**` | 通路/RX/生命周期/UART替身回归 |
| `tools/hil/spp_stress.py` | 新增 `--mode upwin --window-bytes 3072` 上行模式，保持既有all/rtt/win/up/bad及duration soak行为兼容；up继续为无窗口洪泛负例 |
| `tools/hil/spp_rtcm_mix.py` | 如有必要仅扩展新统计解析；原rtcm:/stats:解析保持兼容，不运行串口 |
| `docs/reports/K3.md` | 实施证据与复制账 |

## 3. 冻结修订提案

`components/spp_server/include/spp_server.h`：在K2已批准接口上定稿RX level/wake hook、统计快照/线程契约；hook只能通知，不能在BTC执行模块逻辑。ADR-0007/ADR-0010的批次/RTCM通路说明需要架构方同步定稿；`docs/frozen.sha256` 只更新获批条目。本卡不修改任何既有rtos公共头、demux公共头和 `test/host/**`。

## 4. 实施契约

- RTCM回调span仅当次有效，PHONE角色检查通过后立即push完整帧，其他角色计拒绝。pipe满整帧drop；不把半帧写入/保存回调指针。去掉2314B批次、两个1029B sink及其元数据，产品只保留每会话一个≤280B请求。
- 通路helper/kLink是计数唯一写者，提供小型累计快照；RtcmModule消费累计差量且控制reset epoch。BtLinkModule不调用RtcmModule成员，不引入通用组件的main依赖。
- RX默认每会话512B；已有read chunk未消费后缀不得覆盖。逐字节/逐帧feed允许在产品pending或回复无credit时暂停。不能在一个feed产生多个产品帧后丢掉后续帧。
- DATA_IND后mux外task notify；wait检查level状态并二次复核，队列满不丢唯一唤醒。OPEN/CLOSE和RX_DROP可及时处理，断链清未执行pending，不能先dispatch尾帧。
- 轮次按会话轮转，有界字节/时间预算，命令、超时、报警心跳/0x40不饿死；与K2每会话暂停配合，不把某会话TX满升级为整个链路阻塞。
- RtcmPipe保持kLink单写/kIo单读；统计reset不并发reset字节buffer。GNSS断电/探测未就绪的消费/丢弃策略遵守ADR-0010并可观测。
- kIo将取出的chunk存为pending+offset；uart_write_bytes短写只推进已写字节，零写暂留、不可恢复故障显式discard/重探测。禁止既丢后缀又把accepted计数当UART已发送。UART调用mux外，RTCM TX和GNSS RX分时处理。
- 压测新增upwin：沿用up的256B payload上行，每条按20+256+4=280B线路字节计在途，3072B窗口最多10条；收到对应完整有效回复才归还该请求字节，丢失/超时不能假装释放窗口，更不能自动重发副作用命令。`--flood` 指定帧数，upwin下 `--duration` 只延长/重复该窗口上行阶段以同时达到帧数与持续时间，不误跑既有win8 soak。验证窗口参数、无回复命令不使用此ACK窗口，报告线路字节hwm/应答缺失/内容或CRC错误/最终回读及rx_dropped前后值；已有up保持无窗口，all仍包含该负例并分阶段判读。

## 5. 测试与门禁命令

用例：RTCM与产品随机分片/粘包、坏CRC重同步、非PHONE拒绝、最大1029B帧、pipe满整帧丢；产品pending后仍保存read后缀；回复暂停不饿死另一会话；OPEN/CLOSE/DATA事件合并/满队列/等待前后竞争；持续输入不饿死命令/定时器；累计CRC32/frames/bytes与输入逐字节一致；reset跨轮、断链重开；UART短写/零写/错误/探测暂停/断电，既不重复又不隐性丢字节。两策略都运行。压测工具另用假socket/回复源验证按280B记账且3072B内最多10条、有效完整回复才释放窗口、坏帧/超时不释放且不重发、最终回读一致，以及既有up/all模式兼容。

```sh
. ~/esp/esp-idf/export.sh >/dev/null
cmake -S tools/main_host_check -B builds/k3-main_host_check
cmake --build builds/k3-main_host_check
ctest --test-dir builds/k3-main_host_check --output-on-failure
cmake -S tools/t13_host_check -B builds/k3-t13_host_check
cmake --build builds/k3-t13_host_check
ctest --test-dir builds/k3-t13_host_check --output-on-failure
idf.py build
idf.py size
idf.py size-components
python3 -I tools/g7b_check/g7b_check.py --build build
sha256sum -c docs/frozen.sha256
```

再运行ADR-0011 §10.2九套host普通+ASan、clang-format/G5b/分层/双架构门禁，strict warnings 0告警；不得改test/host、削减原场景或关闭告警。实际静态删除量与新增pending/512B缓存量用map核对，不把DataCenter span描述符当负载节约。

HIL计划（本轮NOT_RUN）：仓库已有脚本，现有all命令如下；其中up是无窗口洪泛负例，只观察显式rx_dropped，不能要求all的TOTAL lost=0：

```sh
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode all
```

本卡新增模式后的验收命令（当前脚本尚不支持upwin/--window-bytes，实施后再执行）：

```sh
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode upwin --window-bytes 3072 --flood 10000 --duration 600
```

up256须在约3KiB字节窗口内至少10000帧、连续10min，rx_dropped=0、应答/序号或内容标记/CRC零缺失；win16也需满足线路字节窗口。另保留 `--mode up` 无窗口负例观察显式rx_dropped，不将有限RX无法吞下洪泛视为窗口验收失败。RTCM混流用已有工具，实际蓝牙/硬件/串口操作由验收方在相应授权后执行，本轮不运行。

## 6. 验收与报告

改后RTCM进pipe前负载复制6→4（不含SDK和下游UART），批次与sink数组真实消失，RtcmModule不转发payload；原rtcm:/stats:输出/HIL解析兼容。RX增加读取粒度且确实及时唤醒、CPU不忙转、alarm时限和轮次预算保持；pipe/UART丢弃原因完整。

`docs/reports/K3.md` 列map/sizeof、通路与共享所有权、窗口条件、实际命令与状态、故障测试日志、硬件未测项。所有失败或契约冲突标FAIL/BLOCKED，不以条件更改后结果冒称无条件up256零丢。

不commit/push、不连串口/烧录、不安装下载。本轮仅修订文档，不改脚本或运行HIL。
