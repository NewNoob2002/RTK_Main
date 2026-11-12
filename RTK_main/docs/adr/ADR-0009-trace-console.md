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

- `app_fw` 新增 `fw/trace.hpp`：类别位掩码（原子读，关闭时一次比较即返回）、16 字节定长事件 `fw::TraceEvent`、多写者单读者的定长环 `fw::TraceRing`（注入 `LockOps`，临界区只拷 16 B；满了丢**新**事件并计数，不阻塞）。
- 全局唯一入口 `fw::trace(cat, code, session, a, b)`：检查掩码 → 取时间 → 调用安装的 sink（main 安装为“压入 TraceRing”）。未安装 sink 时什么也不做（host 测试与报警器复用不受影响）。
- 允许在 link 任务、async_tx 任务、BT 回调任务（spp_server C 驱动经 C 钩子）中调用；**禁止在 ISR 中调用**。
- 格式化与输出由 main 的 `DebugModule` 的低优先级任务完成（prio 1，core 0）：取事件 → 查 (cat, code) 文本表 → printf。环有丢弃时先补一行 `[trace] dropped N`。
- 打开全部 trace 只会让 trace 自己丢，不能影响蓝牙收发：门槛见 §4。

### 2.2 类别与事件（code 由各来源定义，main 负责文本表）

| 类别 | 事件（code） | 来源 |
| --- | --- | --- |
| `link` | open（角色及原因）、reject、close、alarm rx timeout、pending timeout、pair window open/close、whitelist add | `link_core`（`core/trace_codes.hpp`，经 `fw::trace`） |
| `spp` | cong on/off、write done/err、rx drop（字节）、sdp/srv start、gap auth | `spp_server` C 驱动，`spp_server_set_trace_hook()` 钩子；main 安装为转发到 `fw::trace` |
| `rtcm` | 每帧：消息号、长度；reject（原因）、resync、unknown bytes | main `rtcm` 模块（demux 回调里，`stream_demux` 不依赖 app_fw，不改） |
| `proto` | 产品帧 msg_id/type/len、应答结果码、解析错误（CRC/长度） | `link_core` link_service / command_router |
| `alarm` | 状态/级别/override 变化、推送 | `link_core` AlarmService |
| `store` | load/save 结果、去抖、重试 | main storage 模块 |
| `exec` | 轮次超时（ms）、命令投递失败 | main/rtos 执行器适配 |

`link_core` 依赖 app_fw 已允许（AGENTS.md §3），不新增依赖。`spp_server` C 文件不得包含 C++ 头，故用 C 函数指针钩子。

### 2.3 周期报告

`debug report <秒|off>`：每 N 秒一行速率摘要（RTCM B/s 与帧/s、rx_dropped 增量、产品 RTT p50 近似、heap free/min、link/async_tx 栈余量、trace 丢弃数）。高负载时用它，逐帧 trace 只在低速排查时开。

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

## 3. 冻结接口（T0 由 Claude 写入）

- `components/app_fw/include/fw/trace.hpp`（新）
- `components/link_core/include/core/trace_codes.hpp`（新，link/proto/alarm 事件码）
- `components/cli/include/cli/console.hpp`（追加 `fail`、`Verb`、`dispatch`、`println_kv` 等；原接口不变）
- `components/spp_server/include/spp_server.h`（追加 trace 钩子与事件码；原接口不变）

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

T1a → T1b → T2 串行（都改 main 命令文件）。
