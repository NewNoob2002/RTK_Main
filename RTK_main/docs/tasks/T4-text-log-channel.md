# T4 文本日志通道 + 分流器拒绝钩子 + 句子格式 + 日志审计

依据：`docs/adr/ADR-0009-trace-console.md` §2.11（必读）、§2.1–§2.8；`AGENTS.md`；`docs/adr/ADR-0005*`（分流器引擎契约）。
分支：`feat/gnss`（当前 HEAD 之上直接改，不要新建分支，不要提交——由 Claude 审核后提交）。
构建目录只用 `builds/t4*`。

## 冻结头（只能实现，不能改；改动须先停下报告）
- `components/app_fw/include/fw/tlog.hpp`（新）
- `components/app_fw/include/fw/trace.hpp`（追加 `TraceRing::peek_time`）
- `components/stream_demux/include/demux/demux.hpp`（追加 `RejectCause`、`Sink::on_reject`）
- `components/stream_demux/include/demux/decoder.hpp`（追加 `Decoder::explain`）
- 其余 `docs/frozen.sha256` 中列出的文件。完成后**不要**改 frozen.sha256（Claude 重算）。

## 工作项

### A. app_fw：LogRing + tlog
1. 新文件 `components/app_fw/src/tlog.cpp`，加入 `components/app_fw/CMakeLists.txt` 的 `_app_fw_srcs`。
   - `log_install`：复用 trace.cpp 中已安装的时钟（在 trace.cpp 内部增加一个仅组件内可见的取时钟函数，或把两者放在同一翻译单元的
     内部命名空间；不得改冻结头）。
   - 十六进制掩码用 `std::atomic<uint32_t>`（relaxed），与 trace 掩码同法。
   - `tlog`：先 `trace_on(cat)` 判断，再 `vsnprintf` 到 `kLogTextMax + 1` 的栈缓冲；被截断时末尾替换为 `...`；去掉结尾的 `\n`。
   - `tlog_hex`：`log_hex_on(cat)` 为假立即返回；`len = min(size, kLogHexMax)`，`total = size`。
   - `LogRing`：字节环，记录 = 12 B 头 + payload，可跨末尾回绕（分两段 memcpy）；整条放不下 → 丢弃计数；`peek_time` 读最旧头的 t_ms；
     `pop` 复制 payload 到 out（不足截断）、整条移除。锁用 `LockOps`（同 TraceRing）。
2. `TraceRing::peek_time` 实现于 `components/app_fw/src/trace.cpp`。
3. 测试 `test/app_fw`：LogRing 回绕、满丢弃计数、截断、peek/pop 次序、take_dropped；tlog 关闭类别不写、截断加 `...`、hex 掩码两级；
   `FW_TRACE_ENABLED=0` 编译路径（若 test 已有 off 变体则扩展，否则至少保证编译）。

### B. stream_demux：on_reject + explain
1. `components/stream_demux/src/demux.cpp`：在 step 返回 kReject（cause kStep）、超长强制拒绝（kTooLong，现 `++overflows` 处）、
   契约违规（kContract）、`abort()` 丢半帧（kAbort）时调用 `sink_->on_reject(...)`，`prefix` 为该起点被拒绝的已累积前缀，
   `last` = 该起点没有其他候选。回调前后与 on_frame 相同地置 `in_callback_`。统计语义不变。
2. 每个解码器实现 `explain` 并在其 `kDecoder` 定义中填上：NMEA、RTCM3（长度/CRC24 received/computed）、UBX（长度/校验 A/B）、
   UnicoreBin（头长/CRC32）、UnicoreHash（XOR 或 CRC32 received/computed、非法字节、缺 CR）、`components/device_proto` 的 RtkFrame、
   `components/gnss` 的 UnicoreResp 与 ComnavPrompt。原因用简短英文，形如 `bad checksum, received 0x23, computed 0x07`、
   `illegal byte 0x01 at offset 17`、`length 1200 exceeds 1029`、`no CR before LF`、`sync byte 2 is 0x41, expected 0x44`。
   explain 只依赖入参（前缀可能是半帧：给出“最早能确定的原因”）。
3. 测试 `test/stream_demux`：每个 cause 的 on_reject 调用次数/参数、last 标志、不影响已有统计；每个解码器的 explain 至少 2 个用例
   （含 UM980 `$CONFIG` 行：标准 XOR 下报 bad checksum）。已有用例全部保持通过。

### C. main：读者合并输出 + 控制台
1. `main/modules/debug/debug_module.{hpp,cpp}`：新增 LogRing 存储（`CONFIG_RTK_LOG_RING_BYTES`，在 `main/Kconfig.projbuild` 的 trace
   菜单下新增 int，默认 4096，范围 1024–16384，依赖 RTK_TRACE_ENABLE），init 中 `fw::log_install(&LogRing::sink, &log_ring_)`。
   `run()`：两个环都 peek_time，先输出时间较早者；文本记录 `[%4u.%03u] CAT text`；十六进制记录按 §2.11 转储（每行 16 字节、
   4 空格缩进、偏移 4 位十六进制、ASCII 列非可打印用 `.`），`total > len` 时追加 `    (+N bytes not shown)`。丢弃数分别打印
   `[trace] N events dropped` / `[log] N lines dropped`。注意 trace_out 栈（当前 3072 B，HWM 1740 B）：格式化缓冲放静态区或成员，
   完成后在板子上量 HWM（Claude 做 HIL）。
2. `main/modules/debug/debug_cmds.cpp`：`debug on <cat...|all> [hex]`、`debug off <cat...|all>`、`debug list` 显示 `off/on/on+hex`
   与 LogRing 统计（pushed/dropped/max_used/容量）；`debug save` 同时保存 hex。`main/modules/debug/debug_keys.hpp` 新增键 `hex`
   （u32，ns `dbg`），启动时载入并 `log_set_hex_mask`。help 文本英文。
3. `main/modules/debug/trace_text.cpp`：把全部事件文本改成句子格式（ADR-0009 §2.11 示例），类别名大写短名；`kCatSummaries` 改英文。
   事件码与字段含义不变。`test/services/test_link_trace.cpp` 等依赖文本的测试同步更新。

### D. GNSS 与其他模块的日志（审计 + 补齐）
1. `main/modules/gnss/gnss_module.cpp`（io 执行器，可用 tlog）：
   - 帧成功：`GNSS NMEA GNGGA, 72 bytes`；Unicore hash 给消息名（`#VERSIONA`→`VERSIONA`）；RTCM3 给消息号；UBX 给 class/id；
     UnicoreResp 给 `$command,...` 的命令名与 OK/失败；类别打开时逐帧打印，再按 hex 掩码附原始字节。
   - 拒绝：只在 `last == true` 时打一行（避免同一起点多候选重复），包含解码器名、前缀长度、`explain` 原因；hex 附前缀。
   - unknown：`GNSS N unknown bytes dropped`，hex 附字节。
   - 控制器：Action 发送命令时 `GNSS <model|probe> <- "<命令文本>"`（去掉 \r\n）；应答/否定应答/超时（比较 ControllerStats 前后变化
     得出，带耗时 ms）；状态迁移 `GNSS state probing -> configuring`；探测命中型号与固件；波特率切换；RTCM 管道丢弃。
   - 保留现有 trace_gnss 事件（报告与环统计仍用），文本行是新增。
   - 去掉文件头“不用 ESP_LOG”的过时说法若与新行为冲突。`gnss` 组件本身**不**依赖 app_fw（保持纯）：日志都在 main 的 GnssModule 中做。
2. 审计其余：`spp_server`（C，已用 ESP_LOG + trace 钩子）、`link_core`（trace 事件）、`device_proto`、`bt_link` 模块（RTCM/产品帧的
   demux Sink：拒绝分支可在 link 执行器上用 `tlog`（类别 kRtcm/kProto），成功分支只能 trace）、`rtcm_forwarder`、`storage`
   （NVS 读写失败原因）、`system`、`bt_reconnect`、`nvs_store`、`rtos` 执行器。输出 `docs/reports/T4-log-audit.md`：表格列
   “组件/模块 | 已有日志 | 缺口 | 本次补齐 | 备注”，并实现表中标为本次补齐的项（缺口必须给出失败原因，而不只是计数）。
3. `AGENTS.md`：在调试输出规则处补一句——非实时任务的诊断文本走 `fw::tlog`，句子格式见 ADR-0009 §2.11；实时路径只用 `fw::trace`。

## 门槛（全部自己跑过再报告）
- `idf.py -B builds/t4 build` 0 warning（strict warnings 开启）；`CONFIG_RTK_TRACE_ENABLE=n` 变体
  （`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;<scratch>/trace_off.defaults"` 或等效）也 0 warning。
- `python3 -I tools/g7b_check/g7b_check.py --build builds/t4` PASS。
- host：test/host、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/cli、test/gnss、tools/main_host_check、
  tools/t13_host_check 全通过；test/app_fw 与 test/stream_demux 另跑 ASan/UBSan。新增源文件同步到各 host CMakeLists。
- 报告 DRAM/镜像增量（`idf.py -B builds/t4 size`，与 HEAD 对比）。
- 不烧录、不碰串口（Claude 做 HIL）。

## 报告
完成后在终端输出：改动文件清单、每个门槛的实际结果、DRAM/镜像增量、审计表摘要、未完成项与原因。最后一行打印 `T4 DONE` 或 `T4 BLOCKED: <原因>`。
