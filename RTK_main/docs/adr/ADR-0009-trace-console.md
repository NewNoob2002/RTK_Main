# ADR-0009 运行时调试追踪与控制台整理

状态：Accepted（用户 2026-11-12 确认：按建议执行，并提高调试串口波特率）
日期：2026-11-12

## 1. 背景

- 固件 85 处 `ESP_LOG` 全在 `main` 与 `spp_server`；`link_core`、`stream_demux`、`device_proto` 无日志。角色判定、报警变化、解析器成帧/拒绝只能事后从 `status` 计数推断。
- 编译期 `CONFIG_LOG_MAXIMUM_LEVEL=3`（INFO），运行时开不出 DEBUG；IDF 日志按 tag 分，粒度不够。
- 控制台 115200 bps ≈ 11.5 KB/s；IDF REPL 装 UART 驱动时 TX 缓冲为 0（`uart_vfs.c:1209 uart_driver_install(ch, 256, 0, …)`），每次 printf/ESP_LOG 都在 128 B FIFO 上按波特率阻塞调用者。40 KB/s RTCM ≈ 230 帧/s，逐帧一行就超过串口能力；若在 link 任务里直接打日志，会重新引入 J5 修掉的 RX 溢出。
- 控制台 8 条平铺命令（`pair`/`wl`/`policy` 互不相关），`status` 十几行一次性输出，成功无回显、失败只有 IDF 的 `Command returned non-zero error code`。

## 2. 决策

### 2.1 实时路径只记事件，不格式化、不碰串口

- `app_fw` 新增 `fw/Trace.hpp`：类别位掩码（原子读，关闭时一次比较即返回）、16 字节定长事件 `fw::TraceEvent`、多写者单读者的定长环 `fw::TraceRing`（注入 `LockOps`，临界区只拷 16 B；满了丢**新**事件并计数，不阻塞）。
- 全局唯一入口 `fw::trace(cat, code, session, a, b)`：检查掩码 → 取时间 → 调用安装的 sink（main 安装为“压入 TraceRing”）。未安装 sink 时什么也不做（host 测试与报警器复用不受影响）。
- 允许在 link 任务、spp_worker 任务、BT 回调任务（spp_server C 驱动经 C 钩子）中调用；**禁止在 ISR 中调用**。K2 删除 async_tx 任务。
- 格式化与输出由 main 的 `DebugModule` 的低优先级任务完成（prio 1，core 0）：取事件 → 查 (cat, code) 文本表 → printf。环有丢弃时先补一行 `[trace] dropped N`。
- 打开全部 trace 只会让 trace 自己丢，不能影响蓝牙收发：门槛见 §4。

### 2.2 类别与事件（code 由各来源定义，main 负责文本表）

| 类别 | 事件（code） | 来源 |
| --- | --- | --- |
| `link` | open（角色及原因）、reject、close、alarm rx timeout、pending timeout、pair window open/close、whitelist add | `link_core`（`core/TraceCodes.hpp`，经 `fw::trace`） |
| `spp` | cong on/off、write done/err、rx drop（字节）、sdp/srv start、gap auth | `spp_server` C 驱动，`spp_server_set_trace_hook()` 钩子；main 安装为转发到 `fw::trace` |
| `rtcm` | 每帧：消息号、长度；reject（原因）、resync、unknown bytes | main `rtcm` 模块（demux 回调里，`stream_demux` 不依赖 app_fw，不改） |
| `proto` | 产品帧 msg_id/type/len、应答结果码、解析错误（CRC/长度） | `link_core` link_service / command_router |
| `alarm` | 状态/级别/override 变化、推送 | `link_core` AlarmService |
| `store` | load/save 结果、去抖、重试 | main storage 模块 |
| `exec` | 轮次超时（ms）、命令投递失败 | main/rtos 执行器适配 |

`link_core` 依赖 app_fw 已允许（AGENTS.md §3），不新增依赖。`spp_server` C 文件不得包含 C++ 头，故用 C 函数指针钩子。

### 2.3 周期报告

`debug report <秒|off>`：每 N 秒一行速率摘要（RTCM B/s 与帧/s、rx_dropped 增量、产品 RTT p50 近似、heap free/min、link 栈余量、SPP accepted/completed 字节、trace 丢弃数）。高负载时用它，逐帧 trace 只在低速排查时开。

K2（ADR-0011）删除 async_tx 队列/任务/栈统计，控制台发送统计改为驱动的 accepted/completed/cancelled/errors、CONG/high-water/paused/inflight、匿名失败/stray。
accepted及`LinkService::replies_tx`只表示接纳，completed才表示完整WRITE完成，均不代表应用ACK。新增匿名失败事件只携status与候选会话位图；实时回调/worker仍只记事件。

### 2.4 持久化：调试掩码是 `StoreKey` 的第一个使用者

- `debug on/off` 只改内存；`debug save` 写 NVS（命名空间 `"dbg"`，键 `"mask"` u32，`report` 周期键 `"report"` u16）。启动时 storage 加载并应用。
- 实现走 `fw::StoreKey` 表 + `StoreBinding`（ADR-0007 §3.6-2 定义、至今未用），storage 模块新增“按表 load/save”通路；现有 `sn`/`alarm_wl` 本轮不迁移（降低风险，后续单独迁）。

### 2.5 控制台：组 + 动词，统一回执

```
help [组]
status [link|rtcm|sys|all]   默认一屏概览
bt     list | pair [秒] | policy
wl     list | add <MAC> | del <MAC> | clear
alarm  show | set <state> <level> | clear
rtcm   stats | reset
store  show | sn <值>
debug  list | on <类别…|all> | off <类别…|all> | report <秒|off> | save
log    level <tag|*> <E|W|I|N>
sys    info | heap | tasks | reboot
```

- `cli` 在注册时为每条命令包一层 trampoline：handler 返回 0 → 打印 `OK`；返回非 0 且已调用 `cli::fail()` → 已打印 `ERR <原因>`；返回非 0 未给原因 → 打印 `ERR <code>`。trampoline 一律向 esp_console 返回 0，IDF 的 non-zero 提示不再出现。最后一行固定 `OK` 或 `ERR …`（借鉴 SparkFun `$SPxxx,…,OK/ERROR`；本轮不做带校验和的机器协议）。
- `cli::Verb` 子命令表 + `cli::dispatch()`：未知/缺少动词时打印本组用法并 `ERR usage`。
- 自定义 `help`：无参列出每组一行摘要；`help <组>` 列出该组全部动词与参数。
- `status` 分节（标题 + 对齐键值 + 单位）。**`rtcm:` 与 `stats:` 两行格式保持 ADR-0006 §2 原样**（HIL 脚本 `tools/hil/spp_rtcm_mix.py` 依赖），放在 `status rtcm` 与 `status all` 中；`spp_rtcm_mix.py` 改为发送 `status rtcm`。
- `sys tasks` 用 `uxTaskGetStackHighWaterMark` 遍历已知任务句柄（不开 `CONFIG_FREERTOS_USE_TRACE_FACILITY` 也可用）；`log level` 调用 `esp_log_level_set`（只能 ≤ INFO，`N`=NONE）。
- 只读命令直接读 DataCenter；改状态命令仍经 `Executor::call` 投递（ADR-0007 §4.3 不变）。

### 2.6 调试串口：提高波特率 + 大 TX 环形缓冲

- **经典 ESP32 没有 UART DMA**：IDF 只在有 UHCI 的芯片（`SOC_UHCI_SUPPORTED`：C3/C5/C6/H21…）上提供 UART DMA，ESP32 无此能力。替代方案效果等价于“异步发送”：
- `cli::start()` 不再走 `esp_console_new_repl_uart()`，改为自行 `uart_param_config` + `uart_driver_install(ch, rx 256, tx CONFIG_RTK_CONSOLE_UART_TX_BUF, …)` + `uart_vfs_dev_use_driver()` + 行尾设置，再 `esp_console_new_repl_stdio()`。TX 环（默认 8 KB）由 UART 中断搬运，printf/ESP_LOG 只在环满时阻塞。
- 波特率默认 **2 000 000**：`CONFIG_ESP_CONSOLE_UART_BAUDRATE=2000000`（bootloader 与 app 日志、`idf.py monitor` 默认值都随之变化），`CONFIG_RTK_CONSOLE_UART_BAUD` 默认跟随它。80 MHz APB / 2 M = 40 整除，无分频误差。ROM 一级引导的几行仍是 115200（硬件固定，monitor 中会显示乱码，属预期）。
- 实测依据：ACM0 的 CH343（1a86 USB Single Serial）用 esptool 在 921600/1.5M/2M/3M 读 512 KB 结果全部一致（2026-11-12）。USB0 板（报警器测试固件）不改。

### 2.7 编译期开关 `CONFIG_RTK_TRACE_ENABLE`（默认 y）

为 n 时：`app_fw` 对所有使用者 PUBLIC 定义 `FW_TRACE_ENABLED=0`，`fw/Trace.hpp` 中 `trace()`/`trace_on()` 变为空内联（调用点不加 `#if`，整体编译消除）；
`main/CMakeLists.txt` 不收 `modules/debug/`，`module_list.inc`、`AppMain.cpp`（debug 命令/动词/早期 init）、storage 的 `kKeyTables` 同步去掉；
`SppServerPriv.h` 的钩子调用编译为空。实测（IDF v6.1，esp32）：

| 构建 | DRAM | Flash Code | Flash Data | 镜像 |
|---|---|---|---|---|
| on | 92,869 | 422,944 | 122,640 | 681,540 |
| off | 84,405 | 417,060 | 119,344 | 663,912 |
| 差值 | −8,464 | −5,884 | −3,296 | −17,628 |

off 构建 ELF 中无 TraceRing/DebugModule/trace_out/fw::trace 符号。

运行时默认掩码为 0（全关；NVS `dbg/mask` 有保存值时以保存值为准）。T3 实测：7 类全开 + 29 KB/s RTCM 负载，
trace_out 整行 `uart_write_bytes` 输出约 3.4 KB/s，环 max_depth 45/256、0 丢；数据通路 0 丢。

### 2.8 控制台统一输出出口 `cli/Out.hpp`（2026-11-12 补充）

IDF v6.1 的 UART VFS（`esp_driver_uart/src/uart_vfs.c:227`）对每个字符调用一次 `uart_write_bytes(…, 1)`，
`printf` 在 2 Mbaud / 240 MHz 下只有约 24.6 KB/s。决策：

- 命令回复（`cli::fail/print_*`、trampoline 的 `OK`/`ERR`、help）、各模块命令里的输出、trace 事件行与周期报告，
  一律经 `cli::out_printf/out_write`：在 256 B 静态缓冲里整行格式化，再一次交给输出端。业务代码不再直接 `printf`。
- 输出端由 `console start()` 在装好 UART TX 环形驱动后安装（`uart_write_bytes` 直写）；未安装（host 测试、USB 控制台、
  TX 环形=0）时写 stdout。绕过 VFS 后 `\n → \r\n` 由 out 模块负责（跨调用记住上一个字符，不重复补 `\r`）。
- `ESP_LOG` 经 `esp_log_set_vprintf` 接到同一出口、同一把锁，日志、回复、trace 互不穿插；ISR、调度器未运行、
  同任务重入（输出端里又打日志）时回落原 vprintf。超过 256 B 的单条输出整条走 stdout（慢但不截断）。
- 锁：目标板用静态 FreeRTOS 递归互斥（`std::recursive_mutex` 会拉入 `system_error`/`format.o`/`__cxa_atexit`，违反 g7b）；
  host 用 `std::recursive_mutex`。持锁期间可能阻塞在 TX 环形上，禁止在 ISR 调用。
- linenoise 的提示符与回显仍走 stdout；出口每次写前 `fflush(stdout)` 保证顺序。
- 未知命令：IDF REPL 循环（`console/esp_console_common.c:93-94`）对 `ESP_ERR_NOT_FOUND` 硬编码打印 `Unrecognized command`，
  无回调。`cli` 以 `-Wl,--wrap=esp_console_run` 包装：未找到时输出 `ERR unknown command` 并返回 `ESP_ERR_INVALID_ARG`
  （REPL 视作空行不再输出），保证“每条命令最后一行是 OK/ERR”无例外。升级 IDF 时需复核该循环的分支是否变化。

实测（`help`/`debug list`/`sys tasks`/`status all` 各 100 次连发）：控制台回复 24.6 KB/s → 87–145 KB/s，
`status all` 每条 ~51 ms → 8.6 ms。trace 全开 + 蓝牙 28 KB/s 负载：环 0 丢、无穿插行，数据路径无损。

### 2.10 面向客户的控制台：只读状态 + 隐藏 factory 命令集（2026-11-12 补充）

控制台交付给客户后，误改生产配置（SN、白名单）或强制报警状态的风险大于调试便利。决策：

- `cli::Cmd` 增加 `bool hidden = false`（冻结头修订）：隐藏命令照常注册、可执行，`help` 列表不显示；`help <name>` 仍可查。
- 可见命令只读：`bt`（协议栈/链路状态、设备名、当前会话的角色/MAC/句柄，配对窗口打开时提示）、`alarm`（状态、级别、覆盖、
  最近心跳）、`config [identity|link|gnss|debug|build]`（取代 `store`：按分组只读显示 SN、设备名、链路策略、白名单、
  NVS 存储目录中的各键与编译期配置）。移除 `bt pair/policy`、`wl`、`alarm set/clear`、`store sn`。
- 改状态的动作集中到隐藏的 `factory`：`status`（默认）、`unlock <key>`、`lock`、`sn <value>`、`wl-add/wl-del <MAC>`、
  `wl-clear`、`pair`、`alarm-set <state> <level>`、`alarm-clear`。除 status/unlock/lock 外需先 `factory unlock <key>`
  （Kconfig `CONFIG_RTK_FACTORY_KEY`，默认 `rtk-factory`）；解锁状态只在内存，重启或 `factory lock` 恢复锁定。
  这只防误操作，不是安全边界（固件里能读到密钥）。
- 策略 `CONCURRENT/PHONE_PREEMPT` 是编译期配置，只在 `config link` 中显示。

### 2.11 文本日志通道、分流器拒绝钩子与句子格式（2026-11-13 补充）

背景：GNSS 模块、分流器（stream_demux）与各解码器没有任何可读日志；trace 事件只有 16 B 数值，输出是
`key=value`，说不清“解析了哪条语句、哪条为什么失败”。用户要求：打开日志后能看到成功解析的语句、失败原因，
可附原始十六进制；格式参考 SparkFun RTK 固件的句子式日志，不要全是 `key=value`。决策：

- **两条通道，一个读者。** 实时路径（BT 回调、spp 发送、link 执行器逐帧路径）继续只用 `fw::trace()`（§2.1 不变）。
  非实时任务（io 执行器上的 GNSS、控制台任务，以及 link 执行器上的**错误/拒绝分支**——不是逐帧成功分支）可用新的
  `fw::tlog(cat, fmt, ...)` 在自己栈上格式化一行句子，`fw::tlog_hex(cat, bytes)` 附原始字节（≤ 256 B）。两者写入变长记录环
  `fw::LogRing`（新冻结头 `fw/Tlog.hpp`，存储大小 Kconfig `CONFIG_RTK_LOG_RING_BYTES`，默认 4096）。`trace_out` 任务同时读
  TraceRing 与 LogRing，按时间戳合并输出（新增 `TraceRing::peek_time`），仍经 `cli::out_write` 整行写出，不穿插。
- **两级开关。** 类别掩码（`debug on/off`，与 trace 共用）控制事件与文本行；新增十六进制掩码 `log_set_hex_mask`
  控制 `tlog_hex`。控制台：`debug on <cat...|all> [hex]` 打开类别，带 `hex` 同时打开这些类别的十六进制转储、不带则关闭；
  `debug off <cat...|all>` 两者都关；`debug list` 显示每类 `off / on / on+hex`；`debug save` 同时保存十六进制掩码
  （NVS 命名空间 `dbg` 新键 `hex`，u32）。`CONFIG_RTK_TRACE_ENABLE=n` 时 `tlog/tlog_hex/log_hex_on` 为空内联。
- **分流器拒绝钩子。** `demux::Sink` 新增虚函数 `on_reject(decoder, index, prefix, cause, last)`（默认空操作，已有 Sink 不受影响），
  `RejectCause{kStep, kTooLong, kContract, kAbort}`；`demux::Decoder` 新增可选成员 `explain(prefix, out)`：对被拒绝的前缀给出一句英文原因
  （纯函数，引擎从不调用）。各解码器（NMEA、RTCM3、UBX、UnicoreBin、UnicoreHash、RtkFrame、UnicoreResp、ComnavPrompt）都要实现 explain。
- **句子格式（参考 SparkFun）。** 每行：`[  秒.毫秒] 类别 句子`，类别用大写短名（`LINK SPP RTCM PROTO ALARM STORE EXEC GNSS`），
  句子以对象开头、说清发生了什么，数字带单位，必要时括号补充；只有确实是“名字=值”清单时才用 `k=v`。十六进制转储每行 16 字节，
  带偏移和 ASCII 列，缩进 4 格。示例：
  ```
  [  12.345] [GNSS] NMEA GNGGA, 72 bytes
  [  12.346] [GNSS] UnicoreHash rejected 41 bytes: bad checksum, received 0x23, computed 0x07
  [  12.346]     0000: 24 43 4f 4e 46 49 47 2c 43 4f 4d 32 2c 43 4f 4e  $CONFIG,COM2,CON
  [  12.400] [GNSS] 37 unknown bytes dropped (resync 18)
  [  13.001] [GNSS] UM980 <- "GPGGA COM2 1"
  [  13.013] [GNSS] UM980 -> OK (12 ms)
  [  20.100] [LINK] session 0x0081 opened as ALARM (whitelisted)
  [  20.200] [PROTO] session 0x0081 frame 0x04 rejected: bad CRC
  ```
  既有 trace 事件的文本表（`main/modules/debug/TraceText.cpp`）同步改成句子格式；事件码与字段含义不变。
- **覆盖面。** 每个组件/应用模块都要能在打开对应类别后说清成功与失败；审计结果与补齐清单写入 `docs/reports/T4-log-audit.md`。

### 2.12 组件分级日志 `fw/Log.hpp`（2026-11-16 补充，L0）

- **问题。** `fw::tlog()` 只有一个级别且受类别掩码控制，类别默认全关，错误也会被静默；追踪关闭的固件（`RTK_TRACE_ENABLE=n`）没有任何文本诊断；纯组件（gnss、link_core 等）没有日志出口。
- **接口。** C++ 用 `FW_LOGE/W/I/D(cat, fmt, ...)`（`fw/Log.hpp`），C 组件用 `FW_CLOGE/W/I/D(FW_LOG_CAT_*, fmt, ...)`（`fw/LogC.h`，同一实现）。各组件可包一层自己的宏（如 `#define GNSS_LOGW(...) FW_LOGW(fw::TraceCat::kGnss, __VA_ARGS__)`），放在组件私有头。`fw::tlog()` 保留，等价 `FW_LOGI`。
- **全面替代 `ESP_LOG*`（L0b，2026-11-16）。** 固件自有代码（main 与 components）不再调用 `ESP_LOGx`，统一走本通道，每行带时间戳与类别 tag。只保留对 IDF 自身日志的控制调用：`esp_log_level_set`（压低 BTDM/phy 横幅、`log` 命令）与 `esp_log_set_vprintf`（把 IDF 日志转到控制台出口）。
- **级别规则（L0b 修订：追踪与日志分开设置）。** 每个类别一个日志级别（2 bit），默认全部 I，与 trace 类别掩码无关：`debug log <cat...|all> <e|w|i|d> [hex]` 设置，`debug save` 写 NVS `dbg/loglv`。记录条件只有 `level ≤ 该类别级别`；E 总是记录。trace 事件由 `debug trace on|off <cat...|all>` 单独控制（NVS `dbg/mask`，默认全关）；`debug on|off` 保留为 `debug trace on|off` 的简写。hex 转储需该类别 hex 位且日志级别为 D。编译期上限 `CONFIG_RTK_LOG_LEVEL`（0..3，默认 3）把更高级别的调用连同参数一起编译掉；追踪关闭的固件同样按此上限（不再强制 W），日志走直写通道。
- **通路。** 启动第一步（`step_power`）即安装直写 sink，启动横幅、复位原因、各模块初始化结果与启动摘要都以 `[t] [SYS] …` 行按序出现在提示符之前；启动结束（摘要之后、REPL 之前）DebugModule 才把文本环挂上（`attach_log`），此后写环，由 trace_out 按时间与 trace 事件合并输出；环未安装（启动早期、追踪关闭的固件）时写 `log_install_direct` 安装的直写 sink（`main/app/ConsoleSetup.cpp`，同步写 `cli::out_write`）；两者都没有时丢弃并计入 `log_stats().lost`。两条通路共用 `fw::log_format_line`，行格式一致：`[   8.712] [GNSS] W: text`，I 级不带级别前缀；类别名用方括号隔开（2026-11-16 用户要求），trace 事件行同样是 `[t] [CAT] 句子`。
- **记录头。** `LogHeader` 增加 `level` 与保留字节，`total` 由 uint32 收窄为 uint16，总长仍为 12 B（hex 转储原长上限 65535 足够，帧最长 4096）。
- **新类别 `kSys`。** 启动、控制台、系统服务日志；`kTraceAllMask` 随 `kCount` 自动扩展。
- **用法约束（CODING-STYLE §5.1）。** 日志记状态变化、错误与周期统计；默认 I 级可见，所以逐帧/逐句内容（NMEA 句子、拒收字节、未知字节）用 D；实时每轮路径、ISR、BT 回调任务只用 `trace()`。格式 `事件 key=value`，类别前缀由输出端加，正文不重复组件名。
- **host。** 测试可调 `log_install_direct` 装捕获 sink 断言日志；未装时只计数，不写 stderr，测试输出保持干净。

## 3. 冻结接口（T0 由 Claude 写入）

- `components/app_fw/include/fw/Trace.hpp`（新）
- `components/link_core/include/core/TraceCodes.hpp`（新，link/proto/alarm 事件码）
- `components/cli/include/cli/Console.hpp`（追加 `fail`、`Verb`、`dispatch`、`println_kv` 等；原接口不变）
- `components/spp_server/include/SppServer.h`（追加 trace 钩子与事件码；原接口不变）
- §2.11：`components/app_fw/include/fw/Tlog.hpp`（新）、`fw/Trace.hpp` 追加 `TraceRing::peek_time`、`components/stream_demux/include/demux/Demux.hpp` 追加 `RejectCause`/`Sink::on_reject`、`demux/Decoder.hpp` 追加 `Decoder::explain`
- §2.12：`components/app_fw/include/fw/Log.hpp`（新）、`fw/Tlog.hpp` 增加 `LogLevel` 与 `LogHeader::level`、`fw/Trace.hpp` 增加 `TraceCat::kSys`；L0b：`fw/LogC.h`（新）、`fw/Log.hpp` 每类别级别接口

## 4. 门槛

- idf build 0 warning；所有 host 测试通过（含新 `test/app_fw` trace 用例、`test/cli` 回执/dispatch 用例），ASan/UBSan 通过。
- DRAM 增量 ≤ 12 KB（TraceRing 256×16 B = 4 KB + UART TX 8 KB 在堆上，计入 heap 而非 .bss，heap min 需 ≥ 120 KB）。
- HIL：双会话 40 KB/s + 产品、**全部 trace 类别打开 + report 1 s**：RTCM 帧/字节/CRC 一致、`rx_dropped=0`、产品 0 丢、exec overruns 0；trace 允许丢且有计数。trace 全关时吞吐与 RTT 不劣于 R7 基线（p99 ≤ 120 ms 量级）。
- 控制台：每条命令最后一行为 `OK` 或 `ERR …`；`spp_rtcm_mix.py --selftest` 与实测都通过。

## 5. 分步

| 步 | 执行 | 内容 |
| --- | --- | --- |
| T0 | Claude | 本 ADR、冻结头、任务卡 |
| T1a | pi codex | `fw::trace` + `TraceRing` 实现与测试；`cli` 回执/dispatch/help、UART 驱动与波特率；`test/cli` |
| T1b | pi deepseek | 事件点接入（link_core、spp_server 钩子、main 各模块）、`DebugModule`（输出任务、report、`debug` 命令、StoreKey 持久化） |
| T2 | pi gemini | 控制台按 §2.5 重排、`status` 分节、`sys`/`log` 命令、HIL 脚本与 `_template` 同步 |
| T3 | Claude | 上板回归、trace 开/关吞吐对比、报告 |

| L0 | Claude | §2.12 `fw/Log.hpp`、直写 sink、`debug level`、已有 tlog 错误点改 E/W |

T1a → T1b → T2 串行（都改 main 命令文件）。
