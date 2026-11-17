# ADR-0004 通用多协议字节流分流器与 RTCM 经蓝牙转发（方案 B）

- 状态：**Proposed 2026-11-11**。§1–§4、§7 已由用户确认方向（“RTCM 用方案 B”“自己完成一个通用的分流解析器，可在不同 MCU 平台通用”“基准测试跳过”），进入实现；§5、§6 为后续阶段的方向性决定，实施前另行细化。
- Profile：`esp-idf`（固件集成部分）；分流器本体为 **平台无关纯 C11**。评审深度：Full（新组件 + 冻结接口）。
- 修订：AGENTS.md §3 组件依赖表新增 `rtk_demux`，`rtk_proto` 允许 REQUIRES `rtk_demux`（两者都是纯组件）。AGENTS.md 本身由架构方在本 ADR 批准后同步修改。

## 1. 背景

1. 手机 App 作为 NTRIP 客户端，把差分数据（RTCM3）经 SPP 发给设备，再由设备写入 GNSS 板卡 UART。SPP 上同时跑着产品协议（`AA 44 18` 帧：命令、应答、报警推送）。
2. 用户选 **方案 B**：手机发裸 RTCM 字节，与产品帧在同一条 SPP 字节流里交错；设备按同步头分流，不改 App 端的 NTRIP 透传逻辑。
3. GNSS 板卡侧会有 NMEA、RTCM、厂家二进制（Unicore `AA 44 B5`、u-blox UBX 等）、Unicore Hash 语句，换板卡协议集合不同。需要一个能挂载任意协议子集、可在不同 MCU（ESP32、HC32、CH32、STM32）复用的分流解析器。
4. 参考：SparkFun SEMP（`E1-5.4/components/Parser/`，`SparkFun_RTK_Everywhere_Firmware`）。其机制是首字节轮询解析器表 + 每字节函数指针状态机 + 整帧回调，性能足够；短板是：假同步失败后**不回扫**已缓冲字节（假前导会吞掉紧随其后的真消息）、只能逐字节喂入、解析器带可变状态（scratch pad）。

## 2. 决定：`rtk_demux` 组件

| 项 | 决定 | 理由 |
| --- | --- | --- |
| 语言 | C11 子集，BARR-C 安全核心（AGENTS.md §2 C 文件规则） | 跨 MCU 复用；ADR-0003“字节级编解码用 C” |
| 依赖 | 只有 `<stdbool.h> <stddef.h> <stdint.h> <string.h>`；无 OS、无 SDK、无 malloc、无浮点、无 printf | 任意裸机/RTOS 可用；ISR 可调用 |
| 实例 | 每条字节流一个 `rtk_demux_t`，调用方提供存储与帧缓冲区 | 静态分配；多流互不干扰 |
| 解码器 | 无状态 `const` 描述符：`first_byte()` + 纯函数 `step(frame, len, &need)` | 纯函数使“回放重扫”和“分块无关”可证明；描述符可被多个实例共享；增加协议无需改引擎 |
| 首字节识别 | init 时对 256 个值调用 `first_byte`，建 256 B 位图（≤8 个解码器） | 空闲态每字节 O(1)，不像 SEMP 每字节轮询 N 个解析器 |
| 批量累积 | `step` 返回 `NEED_MORE` 时给出 `need`（下次调用前至少还要的字节数）；长度已知后一次要整段，引擎 `memcpy` 后只调用一次 `step`，CRC 在帧尾整段计算 | 载荷阶段不再逐字节函数调用 |
| 同首字节冲突 | 按 decoders[] 优先级依次尝试；一个 REJECT 后在**同一起点**回放给下一个 | 例：NMEA 与 SBF 都以 `$` 开头；产品帧 `AA 44 18` 与 Unicore `AA 44 B5` |
| 失败恢复 | 所有候选都拒绝后，起点前移 1 字节，在已缓冲字节中找下一个可能首字节并回放；之前的字节作为未知字节输出 | 修复 SEMP 的“不回扫”缺陷；最坏复杂度 O(n·max_frame)，正常流 O(n) |
| 半帧超时 | 引擎不计时；调用方在流空闲/断链时调用 `rtk_demux_abort()` | 保持平台无关 |
| 未知字节 | 按流顺序回调 `on_unknown`（可为 NULL） | 透传或诊断；不丢顺序 |
| 回调 | `on_frame` 在 `feed` 内同步调用，帧指针只在回调内有效；回调中重入同一实例返回 `RTK_DEMUX_ERR_REENTRY` | 零拷贝；调用方决定是否复制或入队 |
| 统计 | 每解码器 frames/rejects/bytes；unknown_bytes、resyncs、overflows、aborts | 现场诊断（替代 SEMP 的打印） |

接口契约：`components/rtk_demux/include/rtk_demux.h`（冻结）。引擎的参考语义见 §3。

### 2.1 不做的事

- 不解析消息内容（坐标、基站 ID 等），只判定边界与校验。内容解析由各协议的消费者完成。
- 不做自动波特率、板卡识别（见 §5）。
- 不内置日志；诊断走统计结构。

## 3. 引擎参考语义（实现与测试的共同依据）

记 `buf[0..len)` 为当前候选的已累积字节，`cand` 为当前候选解码器。

1. **空闲**（无候选）：读入字节 `b`。若 `first_mask[b] == 0`，`b` 归入未知字节；否则 `buf = [b]`，`tried = 0`，`cand` = 位图中下标最小者，调用 `step(buf, 1, &need)`。
2. **累积**：从输入复制 `min(need_remaining, 输入剩余)` 字节；凑满 `need` 后调用 `step(buf, len, &need)`。
3. **结论处理**：
   - `FRAME`：先输出此前积压的未知字节，再回调 `on_frame(buf, len)`；`len = 0`，回到空闲。
   - `NEED_MORE`：若 `need == 0` 或 `len + need > cand.max_frame` → 视为 `REJECT`（契约违规，计入该解码器 rejects）；若 `len + need > buf_size` → 视为 `REJECT` 并 `overflows++`。
   - `REJECT`：`tried |= bit(cand)`。若 `first_mask[buf[0]] & ~tried` 非空，取其中下标最小者为新 `cand`，**在同一起点回放** `buf[0..len)`（从 `step(buf,1)` 起，按 `need` 递增前缀；前缀达到 `len` 仍 `NEED_MORE` 则继续从输入累积；回放中途得到 `FRAME` 于 `p < len`，则输出该帧并把 `buf[p..len)` 当作新输入重新处理）。若没有剩余候选：`resyncs++`，`buf[0]` 归入未知字节，`buf[1..len)` 当作新输入重新处理（重新从步骤 1 开始，`tried` 清零）。
4. **重新处理已缓冲字节**等价于“把这些字节插到尚未处理的输入之前再喂一次”。实现可原地 `memmove`，不得使用额外的 O(buf_size) 栈数组。
5. `abort()`：反复把当前候选按 `REJECT` 处理（同第 3 条），直到不再有候选在累积；回放过程中完成的帧照常回调；最后所有剩余字节作为未知字节输出，`len = 0`。
6. **不变量**：
   - I1 分块无关：同一字节序列任意切分喂入，帧回调序列相同，未知字节拼接后相同。
   - I2 字节守恒：每个输入字节最终恰好属于一帧或未知字节输出一次（`abort` 或后续字节到来之后）。
   - I3 有界：每次 `feed` 只使用 O(1) 栈；`buf` 永不越界；每次 REJECT 后起点单调前移或候选集合单调缩小，因此必然终止。
   - I4 首字节位图在 init 后不变；解码器描述符不被写。

## 4. 内置解码器（`rtk_demux_decoders.h`、`rtk_proto_demux.h`）

| 描述符 | 首字节 | 判定 | max_frame | 早拒绝点 |
| --- | --- | --- | --- | --- |
| `rtk_demux_dec_rtcm3` | `D3` | 第 2 字节高 6 位 = 0；长度 10 bit；CRC-24Q（多项式 0x1864CFB，初值 0，覆盖头 3 B + 载荷，校验值 3 B 大端） | 1029 | 第 2 字节保留位 |
| `rtk_demux_dec_nmea` | `$` | 第 2 字节 `[A-Z0-9]`；正文可打印 ASCII 0x20–0x7E 且不含 `$`；`*` 后 2 位十六进制 XOR（`$` 与 `*` 之间）；`\r\n` 结尾 | 256 | 非法字符立即拒绝 |
| `rtk_demux_dec_ubx` | `B5` | 第 2 字节 `62`；len u16 LE（偏移 4）；8 位 Fletcher CK_A/CK_B 覆盖 class..载荷 | 4096 | 第 2 字节；长度超限 |
| `rtk_demux_dec_unicore_bin` | `AA` | `AA 44 B5`；24 B 固定头，载荷长 u16 LE 在偏移 6；CRC-32（0xEDB88320 反射、初值 0、不取反）覆盖头+载荷，4 B LE | 4096 | 第 2、3 字节；长度超限 |
| `rtk_demux_dec_unicore_hash` | `#` | 第 2 字节 `[A-Z]`；正文可打印 ASCII 且不含 `#` `$`；`*` 后 8 位十六进制 CRC-32（同上算法，覆盖 `#` 与 `*` 之间）**或** 2 位十六进制 XOR；`\r\n` 结尾 | 1024 | 非法字符 |
| `rtk_proto_demux_decoder`（在 `rtk_proto`） | `AA` | 与 `rtk_proto_parser_feed` 规则完全一致：`AA 44 18`、头长字节 `0x14`、payload_len（偏移 12，u16 LE）≤ 256、CRC-32（`rtk_proto_crc32`） | 280 | 第 2、3 字节；头长；长度 |

- 我们的产品帧同步头 `AA 44 18` 与 NovAtel OEM 二进制相同，与 Unicore `AA 44 B5` 共享前两字节。二者同挂一个实例时靠第 3 字节早拒绝区分（§2 同首字节冲突规则）。目前它们不在同一条流上；将来若把 GNSS 原始数据转发到蓝牙，需重新评估。
- CRC 实现：各协议在帧尾整段计算。允许用 256 项查找表（放 `const`，进 Flash/ROM），由实现方在报告中给出 Flash 占用；必须用已知向量核对（RTCM 1005 样例、NovAtel/Unicore 样例、`"123456789"` 标准校验值）。
- SBF（`$@`）、SPARTN 暂不实现；描述符接口已支持，按需追加。

## 5. GNSS 板卡配置档（后续阶段，方向性）

- `GnssBoard` 接口（C++）：初始化命令序列、默认波特率、能力表、挂到 GNSS UART 分流器的解码器子集（如 UM980：NMEA + RTCM3 + Unicore 二进制 + Unicore Hash；ZED-F9P：NMEA + RTCM3 + UBX）。
- 识别：NVS 出厂配置优先；未配置时依次发送各家版本查询并用分流器识别应答。
- 上层只拿统一的位置/状态/差分事件，不直接接触厂家协议。实施前另写 ADR 细化。

## 6. 配置管理器与蓝牙接收缓冲区（后续阶段，方向性）

- 配置：各组件声明静态描述表（键、类型、默认值、范围、是否需重启、是否对外），NVS 存储（每组件一个命名空间，键 ≤ 15 字符），`key=value` 只作为控制台/App/导出接口。实施前另写 ADR 细化。
- `RTK_BT_SPP_RX_BUF_SIZE` 改为 Kconfig `CONFIG_RTK_BT_SPP_RX_BUF_SIZE`（默认 2048，接 RTCM 后建议 ≥ 4096）。涉及冻结头 `rtk_bt_spp.h`，与 §7 集成一起修订。

## 7. 固件集成（下一阶段，本批次不做）

- 蓝牙会话的 `drainRx`（`main/app_link.cpp`）改为把 StreamBuffer 读出的块整块 `rtk_demux_feed()`；实例挂 `{rtk_proto_demux_decoder, rtk_demux_dec_rtcm3}`，产品帧优先。
- 产品帧 → 现有 `LinkService`（经 `rtk_proto_frame_view` 解出字段）；RTCM 帧 → GNSS UART 发送管道（StreamBuffer，满则丢整帧并计数），不受“每轮一帧”限制。只接受 PHONE 角色会话的 RTCM。
- 未知字节只计数。会话断开时 `rtk_demux_abort()`。
- 现有 `FrameParser`（ADR-0003 冻结）保留，供报警器 C 固件与现有测试使用。

## 8. 本批次分工（BATCH-G）

| 任务 | 内容 | 执行模型 | 选择理由 |
| --- | --- | --- | --- |
| G1 引擎 | `components/rtk_demux/rtk_demux.c`：§3 全部语义 | pi + `codex/gpt-6.1-sol`（thinking high） | 回放重扫、重入、边界条件是本批最难的算法部分，需要强推理 |
| G2 解码器 | `components/rtk_demux/rtk_demux_decoders.c`（5 个）、`components/rtk_proto/rtk_proto_demux.c` | pi + `workbuddy/deepseek-v4.1-flash`（high） | 规格明确、量大、机械性强；该模型已熟悉本工程规范 |
| G3 测试 | `test/demux/`：独立 host CMake 工程，Unity；黑盒测试只依据冻结头文件与本 ADR | pi + `antigravity/gemini-3.8-flash` | 1M 上下文可一次读全规格；与实现方隔离，独立构造测试向量 |
| G4 集成验收 | 交叉审查、host 构建（严格告警 + ASan/UBSan）、`-m32`/交叉编译可移植性检查、`idf.py build` | 架构方（Claude） | 验收独立于实现 |

`test/demux/` 是新的测试工程，不改动冻结的 `test/host/`。G1、G2、G3 并行；G3 先以桩实现编译通过，G1/G2 完成后跑通。

### 验收门禁

1. `test/demux` 普通构建与 ASan/UBSan 构建全部通过；C 严格告警集（AGENTS.md §3 纯 C 一档 + `-Wpedantic -Wvla -Wcast-align`）零告警。
2. 至少覆盖：各解码器正例/每个早拒绝点/校验错；同首字节冲突（产品帧 vs Unicore 二进制、NMEA vs Unicore Hash 之外的 `$` 冲突用自定义测试解码器）；假前导后紧跟真帧可恢复；I1 分块无关（随机切分 ≥1000 轮）；I2 字节守恒；`overflow`、契约违规、重入、`abort`；随机混合流（多协议帧 + 随机垃圾）在 `abort` 后全部帧按序恢复。
3. 可移植性：`gcc -std=c11 -m32` 与（若本机可用）`arm-none-eabi-gcc -mcpu=cortex-m0plus -Os` 编译 `rtk_demux` 零告警；报告给出 xtensa 下 `.text/.rodata` 大小。
4. `idf.py build` 通过（组件加入构建但未被 main 使用时以 `-Wl,--gc-sections` 去除，不影响现有固件）。
5. `sha256sum -c docs/frozen.sha256` 通过（架构方先把本批新冻结头加入清单）。

## 勘误 2026-11-17（分流器改为 C++ 后的当前名称）

本节只订正名称与路径事实，不改变本 ADR 的分流语义。语言分工已由 ADR-0005 改为 C++，实现随之替换：

| 本文旧名 / 旧路径 | 当前名称 / 当前路径 |
| --- | --- |
| `rtk_demux` 组件（C） | `stream_demux` 组件（C++）：`components/stream_demux/include/demux/{Demux,StaticDemux,Decoder,TextWriter,Crc}.hpp`、`include/demux/decoders/*.hpp`、`src/{Demux,Crc}.cpp` |
| `rtk_demux_t` / `rtk_demux_feed()` / `rtk_demux_abort()` | `demux::StaticDemux` 实例与 `feed()` / `abort()` 成员（命名空间 `demux`） |
| `rtk_proto_demux.h`、`rtk_proto_demux_decoder` | `proto/DemuxFrame.hpp`（`device_proto`），产品帧解码器在 `proto::demux_frame` |
| `main/app_link.cpp` 的 `drainRx` | `main/modules/bt_link/BtLinkModule.cpp` |
| `rtk_proto_frame_view` | `proto/Frame.hpp` 的字段视图接口 |
