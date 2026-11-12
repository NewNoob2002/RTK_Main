# T1b 追踪事件点接入 + DebugModule（ADR-0009）

执行方：pi + claude-opus-4.5。前置：T1a（`fw::trace`、`TraceRing`、`cli::fail/Verb/dispatch/add_verbs`）已完成。
本轮只做事件点接入与 DebugModule，不改协议、不碰冻结文件、不烧录。

结论：**功能与全部门禁通过，唯一偏离是 DRAM 增量 8 488 B（任务卡 6 KB，ADR-0009 上限 12 KB）**，
原因见 §7.3——4 KB 追踪环 + 4 KB 输出任务栈都是同一张任务卡强制要求的，两者相加已超 6 KB。

## 1. 改动文件

新增：

| 文件 | 作用 |
| --- | --- |
| `components/rtos/include/rtos/exec_trace.hpp` | `rtos::ExecTrace`（kRoundOverrun=0、kCallDrop=1）+ `rtos::trace_exec()`；rtos 不能 include main 头，码值以 rtos 为准 |
| `main/def/debug_def.hpp` | `DebugVerb`（list/on/off/report/save） |
| `main/def/…`（见下） | `RtcmTrace`/`RtcmReject`、`StoreTrace`、`using ExecTrace = rtos::ExecTrace` + static_assert 锁定码值 |
| `main/modules/debug/debug_module.hpp/.cpp` | 环、静态存储、portMUX、输出任务、StoreBinding、命令处理 |
| `main/modules/debug/debug_keys.hpp` | `fw::StoreKey kKeys[2]`（ns `dbg`：`mask` kU32、`report` kU16 0..3600）+ 三条 static_assert |
| `main/modules/debug/trace_text.hpp/.cpp` | 37 项 (cat,code)→文本表、类别名、类别说明、`format_event()`（唯一格式化入口） |
| `main/modules/debug/debug_cmds.cpp` | `debug` 命令组（`cli::Verb` + `dispatch` + `add_verbs`） |
| `test/services/test_link_trace.cpp` | 记录型 sink + open/HELLO/close 事件序列用例 |
| `tools/main_host_check/shim/nvs.h`、`shim/nvs_shim.c` | host 的 NVS 整数键内存替身（storage 通用键表用） |

修改（仅加追踪调用、必要的接线与 host 支撑）：

- `components/link_core/link_manager.cpp`、`link_service.cpp`、`command_router.cpp`、`services/alarm_service.cpp`
- `components/rtos/executor.cpp`
- `components/spp_server/{private_include/spp_server_priv.h,spp_server_gap.c,spp_server_init.c,spp_server_rx.c,spp_server_session.c,spp_server_tx.c}`
- `main/{app_config.hpp,app_main.cpp,module_list.inc}`、`main/def/{rtcm_def.hpp,storage_def.hpp,system_def.hpp}`
- `main/modules/bt_link/bt_link_module.{hpp,cpp}`、`main/modules/storage/storage_module.{hpp,cpp}`
- `test/services/CMakeLists.txt`、`tools/main_host_check/CMakeLists.txt` 与 host 桩（`main_host_check.cpp`、`template_check.cpp`、`shim/freertos/task.h`）
- `tools/t13_host_check/shim/{fake_rtos.c,inc/esp_err.h,inc/freertos/FreeRTOS.h}`

`git diff --stat`：28 个已跟踪文件 +462/−13；另新增 6 项（含 `main/modules/debug/` 目录）。

## 2. 门槛逐项结果

### 2.1 IDF 构建与体积

```sh
. ~/esp/esp-idf/export.sh
IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1b build          # PASS，0 warning
IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1b size
IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1b size-components
```

- 编译告警：`grep -c 'warning:'` = **0**（复检方式：`touch` 全部改动源文件后整轮重建，日志 `/tmp/t1b_build2.log`）。**PASS**
- 体积（T1a 报告基线 → 本轮）：

| 项 | T1a | T1b | 差 |
| --- | --- | --- | --- |
| DRAM | 84 381 | 92 869 (74.55%) | **+8 488** |
| IRAM | 82 791 | 82 791 | 0 |
| Flash（total image） | 663 792 | 681 540 | +17 748 |
| `rtk_main.bin` | — | 0xa66c0（681 664 B） | — |

DRAM 增量逐项解释（`libmain.a` 的 `.bss` 为 33 052）：

| 组成 | 字节 | 来源 |
| --- | --- | --- |
| 追踪环静态存储 | 4 096 | `DebugModule::ring_storage_` = 256 × `sizeof(fw::TraceEvent)`(16) |
| 输出任务静态栈 | 4 096 | `rtos::StaticTask<kTaskStackBytes>`，`kTaskStackBytes = 4096` |
| 模块对象/TCB/环形结构 | ≈ 296 | `TraceRing`、`StaticTask` 的 TCB 指针、`portMUX_TYPE`、verb 表以外的标量成员 |
| 合计 | ≈ 8 488 | 与实测增量一致 |

**判定：FAIL（相对任务卡的 6 KB）/ PASS（相对 ADR-0009 §4 的 12 KB）**，理由与可选改法见 §7.3。

### 2.2 host 测试（普通 + ASan/UBSan）

```sh
cmake -S <dir> -B builds/t1b-<name> && cmake --build builds/t1b-<name> && ctest --test-dir builds/t1b-<name> --output-on-failure
cmake -S <dir> -B builds/t1b-asan-<name> \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && ... ctest ...
```

| 工程 | T1a 用例数 | 普通 | ASan/UBSan |
| --- | --- | --- | --- |
| `test/app_fw` | 3 | PASS 3/3 | PASS 3/3 |
| `test/cli` | 2 | PASS 2/2 | PASS 2/2 |
| `test/host` | 7 | PASS 7/7 | PASS 7/7 |
| `test/stream_demux` | 4 | PASS 4/4 | PASS 4/4 |
| `test/bt_reconnect` | 1 | PASS 1/1 | PASS 1/1 |
| `test/services` | 2 | **PASS 3/3**（新增 `test_link_trace`） | **PASS 3/3** |
| `tools/main_host_check` | 30 | PASS 30/30 | PASS 30/30 |
| `tools/t13_host_check` | 29 | PASS 29/29 | 见下（既有偶发） |

用例数只增不减。构建目录一律 `builds/t1b-*`。

`tools/t13_host_check` 的 ASan 构建存在**既有偶发失败**：`t13_write_unmatched_no_resource` 约 3/20 失败
（`t13_host_check.c:606: fake_write_call_handle(0) == 0x11U`）。已用 `git archive HEAD`（不含本轮改动的
`/tmp/t1b-base`）对照复现：基线同样 4/20 失败 → 与本轮改动无关，属该用例在 ASan 下的调度竞态。

### 2.3 静态门禁

```sh
python3 -I tools/g7b_check/g7b_check.py --build builds/t1b   # G7b PASS（异常/RTTI 6/6 白名单，heap 无新增）
clang-format --style=file --dry-run --Werror <改动的 .cpp/.hpp>   # 无输出 = PASS
sha256sum -c docs/frozen.sha256                              # 165 项全部 OK
git diff --check                                             # 无输出
```

**PASS**。C 文件按工程规则以 astyle 为权威，本轮未对既有 C 文件做整篇重排；新增行沿用同文件的注释对齐风格。

## 3. 事件点清单（文件:行）

### 3.1 link_core（`core::trace_*`）

`components/link_core/link_manager.cpp`：kOpen 109/113/128（角色+LinkReason：whitelisted/first_phone/pending_hello）、
kKick 120（手机优先抢占，理由 policy_preempt）、kClose 148/181/190（普通关闭 / 配对窗口外 HELLO / 白名单满）、
kWhitelistAdd 195、kRoleChange 200、kAlarmRxTimeout 224、kPendingTimeout 231、kPairClose 243、kPairOpen 283、
kWhitelistClear 294。

`components/link_core/link_service.cpp`：kBadCrc 113、kBadLength 118、kFrame 127、kReply 145（成功）、
kReplyError 149（写失败）、kPushFail 190/198。

`components/link_core/command_router.cpp`：kNoRoute 86（角色不匹配的 kDenied）、92（无路由）。

`components/link_core/services/alarm_service.cpp`：kState 94、kOverride 130/136、kPushFail 160/168、kPush 173。

### 3.2 spp_server（C 驱动 → 钩子）

| 事件 | 位置 |
| --- | --- |
| SRV_START | `spp_server_gap.c:40` |
| AUTH | `spp_server_gap.c:145` |
| CONG（仅变化时） | `spp_server_session.c:215` |
| WRITE_ERR（提交失败） | `spp_server_tx.c:108` |
| WRITE_DONE（字节+耗时 ms） | `spp_server_tx.c:274` |
| WRITE_ERR（写完成失败） | `spp_server_tx.c:276` |
| RX_DROP（本次丢弃+缓冲已用） | `spp_server_rx.c:49` |
| EVT_DROP（事件类型+累计丢弃） | `spp_server_rx.c:101` |

钩子实现：`spp_server_init.c` 定义 `spp_server_trace_fn s_spp_trace;`，`spp_server_set_trace_hook()` 在
`taskENTER_CRITICAL(&s_spp_mux)` 内赋值；私有头 `spp_server_priv.h:191` 的 `spp_server_trace()` 先在临界区内
拷贝函数指针、出临界区后再调用（**临界区内不调用 FreeRTOS API，也不回调用户代码**）。
耗时来自 `spp_server_priv.h:111` 新增的 `tx_submit_tick`（提交前在临界区外读 `xTaskGetTickCount()`）。

### 3.3 main

- `rtcm`（kRtcm，`main/def/rtcm_def.hpp`）：demux 回调 `bt_link_module.cpp:65/76/80`（拒绝-角色、每帧 msg+len、
  拒绝-容量），`bt_link_module.cpp:312/316`（重同步、unknown bytes，按分流器累计值与会话基线取差）。
- `storage`（kStore，`main/def/storage_def.hpp`）：`storage_module.cpp:198/253`（通用键表 load/save 结果）、
  304（去抖触发+白名单条数）、337/344（白名单保存结果）、345（重试间隔+失败次数）。
- 执行器（kExec，`main/def/system_def.hpp`）：`components/rtos/executor.cpp:181`（`Executor::call` 投递失败）、
  281（轮次超时，a=实际 ms、b=预算 ms）。

## 4. 代码检查清单（门槛 4）

| 检查项 | 证据 |
| --- | --- |
| 链路任务无新增 `ESP_LOG`/`printf` | `git diff -U0` 中新增 `ESP_LOG*` 仅 3 处：`spp_server_rx.c`（既有日志行改参数）、`app_main.cpp:177`（启动期）、`app_main.cpp:145` 附近（控制台注册）。`main/modules/bt_link/**`、`link_core/**`、`main/modules/storage/*.cpp` 的非命令行路径无新增日志 |
| async_tx 任务无新增日志 | `main/modules/bt_link/async_tx.cpp` 本轮未改动（`git status` 无该文件） |
| SPP 回调路径无新增日志 | 新增的 `spp_server_trace(...)` 与既有 `ESP_LOGW/LOGE` 并列，未加新日志；`spp_server_gap.c/rx.c/session.c/tx.c` 的新增行只有 trace 调用与 `tx_submit_tick` |
| 所有 `fw::trace` 不在 ISR | 调用点全部位于：kLink 执行器任务（link_core、bt_link、storage、executor）、BTC/SPP 回调（gap/session/tx/rx，任务上下文）、启动期单线程（`debug_module.cpp:83/89`）。无 ISR 或 `esp_timer` 回调调用 `fw::trace` |
| 输出任务是唯一格式化者 | 全工程 `printf` 只出现在命令回调（console 任务，既有风格）与 `debug_module.cpp:220/228/271`（`trace_out` 任务）。事件文本只在 `debug_module.cpp:228` 由 `debug::format_event()` + `printf` 生成 |
| 格式化逻辑集中 | `main/modules/debug/trace_text.cpp`（`format_event`、`trace_cat_name`、`trace_cat_summary`、`parse_cat`），未知码输出 `cat=… code=… a=… b=…` |
| 输出任务优先级/核 | `debug_module.cpp:100` 附近 `task_.start("trace_out", 1U, 0U, …)`：prio 1、core 0、栈 4 096 B（按 printf+文本表估计 ≈2 KB 占用，余量 ≥1 KB；**上板后需用 `uxTaskGetStackHighWaterMark` 复核**） |
| 掩码在环外 | `fw::trace_set_mask()` 直接改全局原子量；`debug on/off` 在 console 任务；`report`/`save` 经 `app::submit()` 投递到 kLink 执行器 |

## 5. 契约与数据流要点

- 环：`fw::TraceRing` + `std::array<fw::TraceEvent,256>` + `portMUX_TYPE`（`rtos::spin_lock_ops`），全静态、无堆。
- 安装顺序：`app_main.cpp:175` 在 `data.init()` 之后、`storage.init()` 之前显式 `s_app.debug.init()`，
  以便捕获启动期的 `kStore` 事件；`Executor::init()` 之后的二次调用因 `installed_` 幂等返回 `kOk`
  （`debug_module.cpp:74-77`），不再触碰任何全局状态。
- SPP 转发：`spp_server_set_trace_hook(&spp_trace_forward)`（`debug_module.cpp:89`），C 回调把
  `spp_server_trace_code_t` 原样作为 `fw::TraceCat::kSpp` 的 code，`a`/`b` 直传、`session=handle&0xFFFF`。
- 持久化：`debug_keys.hpp` 两张键经 `fw::StoreKey` 表登记；storage 新增 `load_keys()`/`save_keys()`/`key_tables()`，
  `init()` 在 `load_sn()` 之后 `load_key_tables()`；`debug save` 在 kLink 执行器里同步调用 `save_keys(keys())`。
  `nvs_store.h` 冻结且无整数 API，故 storage 直接用 `nvs.h`（ns `dbg`），`rtk` 命名空间与其键未变。
- 命令组：`debug list|on|off|report <1..3600|off>|save`，最后一行由 cli 统一补 `OK`/`ERR …`；
  参数错误走 `cli::fail`。命令表总数 5 ≤ `cli::kMaxCmds(24)`。

## 6. 预期控制台样例（供 T3 上板对照）

```text
debug list
ring events=256 depth=0 pushed=142 dropped=0 max_depth=9
report period=0s (off)
link  on   会话打开/关闭、角色判定与白名单
spp   on   驱动层：启动、拥塞、写完成、溢出
rtcm  on   RTCM 帧转发、拒绝与重同步
proto on   协议帧、应答与路由失败
alarm on   报警状态、override 与状态推送
store on   NVS 读写、去抖与重试
exec  on   执行器轮次超时与投递失败
OK

debug on link rtcm
OK

[   0.412] store load   a=0 count=2
[   1.008] link  open   h=0081 role=PHONE why=first_phone
[   1.052] spp   srv_start a=1
[   1.310] link  pair_open a=60000
[   2.744] link  open   h=0082 role=PENDING why=pending_hello
[   2.901] link  whitelist_add h=0082 n=1
[   2.901] link  role_change h=0082 old=PENDING new=ALARM
[   3.020] rtcm  frame  msg=1005 len=1029
[   3.041] rtcm  frame  msg=1077 len=245
[   3.062] link  close  h=0082 role=ALARM
[   6.004] link  alarm_rx_timeout h=0082 quiet=3001

debug report 5
report period=5s
OK
[report] rtcm 39.8KB/s 221f/s drop+0 | prod 12/s | heap 142K min 136K | stk link 4.7K tx 2.3K | trace drop 0
```

（`debug list` 的机读格式为 `ring …`、`report …`、每类别 `%-5s %-3s %s`；trace 行时间列是
`[%4lu.%03lu]`，类别名左对齐 5 列，字段为 `键=值`。）

## 7. 与契约的偏离（逐条说明）

1. **`kBadCrc`/`kBadLength` 的 b 参数**：`trace_codes.hpp` 注释要求 `kBadCrc a=丢弃字节数`、
   `kBadLength a=声明长度`，但 `proto::FrameParser::feed()` 只返回 `ParseResult`，不暴露这两个量。
   现改为 `a=该会话累计 CRC/长度错误数`（`link_service.cpp:113/118`），未改协议解析器接口（冻结头）。
2. **`kKeys` 声明形式**：任务卡写 `fw::StoreKey kKeys[]`，工程 lint 禁止 C 数组，改用
   `std::array<fw::StoreKey,2U>`（`debug_keys.hpp:13`），语义与用法相同（`keys()` 返回 `std::span`）。
3. **DRAM 增量 8 488 B > 任务卡 6 KB**（§2.1）。拆解：追踪环 4 KB（卡要求 256×16 B）+ 输出任务栈 4 KB
   （卡要求“实测 + ≥1 KB 余量”，printf 与文本表路径按 2 KB 估，4 KB 是留够余量的保守值）。两者都是同一张
   卡的硬性要求，相加已超 6 KB 预算；ADR-0009 §4 的上限是 12 KB，本轮满足。若要压到 6 KB 以内，可选
   （a）环降到 128 条（−2 KB）或（b）栈降到 2 048 B（−2 KB），任一都能达标，但都与卡的明文要求冲突，
   因此保持现状并在此报告，等验收方决定。
4. **storage 直接用 `nvs.h`**：`nvs_store.h` 冻结且只有 SN/白名单 API，按任务卡“在 storage 模块内直接用
   `nvs.h`”实现整数键读写（`storage_module.cpp` 的 `extern "C" { #include "nvs.h" }`）。host 侧为此新增
   `tools/main_host_check/shim/nvs.h` 与 `nvs_shim.c`（内存替身），并在 `tools/t13_host_check` 的 `esp_err.h`
   替身补 `ESP_ERR_NVS_NOT_FOUND`(0x1102)。
5. **`DebugModule::init()` 幂等**：`app_main` 需在 `storage.init()` 之前装环（否则启动期 `kStore` 事件全丢），
   而 `Executor::init()` 会按模块表再调一次。因此重复调用返回 `kOk` 且不改状态（`debug_module.cpp:74`）。
6. **kExec 码定义在 rtos**：`rtos` 组件不能 include `main/def`，故码值放在
   `components/rtos/include/rtos/exec_trace.hpp`，`main/def/system_def.hpp` 用 `using ExecTrace = rtos::ExecTrace`
   + 两条 `static_assert` 锁定数值，避免两处漂移。
7. **spp 钩子用 portMUX 临界区而非 `_Atomic`**：工程 lint 把 `__atomic_*` 判为保留标识符、clangd 把 C 文件按
   C++ 解析后不识别 `_Atomic`/`atomic_*`。改为“临界区内拷贝函数指针、出临界区调用”，代价是每次上报一次
   极短的临界区，符合 ESP-001/003 的约束（临界区内无 FreeRTOS API、无阻塞、无用户回调）。
8. **`spp_server_priv.h` 的 `_Static_assert` → `static_assert`**（并 `#include <assert.h>`）：同上，clangd 的
   C++ 回退把 `_Static_assert` 判为保留标识符；仍是 C11 标准写法。该文件是组件私有头，不在冻结清单内。
9. **`spp_server_rx.c` 既有日志行参数**：事件队列满时的 `ESP_LOGW` 原读 `s_spp.evt_dropped`（临界区外读共享量），
   现改用临界区内取出的 `total_dropped`。是修改既有日志而非新增日志。
10. **`test/services` 与 `tools/main_host_check` 的 CMake 补 `trace.cpp`**：link_core/executor 现在引用
    `fw::trace`，这两个 host 工程原先未链 `components/app_fw/src/trace.cpp` → 已补（任务卡第 0 条允许）。
11. **`main_host_check`/`template_check` 链接真实 debug 模块**：storage 的 `kKeys` 表引用 `DebugModule` 的四个
    绑定函数，host 侧必须链接 `debug_module.cpp`/`trace_text.cpp`（`template_check` 另加 `debug_cmds.cpp` 与
    `cli/src/reply.cpp`），并补 `spp_server_set_trace_hook`/`vTaskDelete` 空桩。

## 8. 未解决 / 未验证

- **NOT_RUN**：`heap min ≥120 KB`（ADR-0009 门槛）与输出任务栈实测水位需上板；本轮只按静态估计留 4 KB。
- **NOT_RUN**：`idf.py size` 的 IRAM 无变化，但 trace 文本表（约 1.2 KB rodata）只在 Flash，未做 HIL 验证。
- **观察（既有）**：`tools/t13_host_check` 的 `t13_write_unmatched_no_resource` 在 ASan 下约 3/20 偶发失败，
  基线（`git archive HEAD`）同样 4/20 → 与 T1b 无关，建议 T3 一并处理。
- **工具噪声**：pi-lens/clangd 对 `tools/main_host_check/app_test_adapter.hpp` 报 20 条“找不到
  `modules/bt_link/bt_link_module.hpp`/`AppContext` 不完整”的错误，是 clangd 未带 `-I main` 的头文件独立
  分析结果；该目标的实际编译为 0 错误 0 告警、30/30 用例通过。
- 工作区中 `components/cli/src/console.cpp` 的 help 注销修复属他人的改动（已由提交 80698ff 入库），本轮未改。

## 9. 复现命令汇总

```sh
. ~/esp/esp-idf/export.sh
IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1b build && IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1b size
python3 -I tools/g7b_check/g7b_check.py --build builds/t1b
sha256sum -c docs/frozen.sha256
cmake -S test/services -B builds/t1b-services && cmake --build builds/t1b-services && ctest --test-dir builds/t1b-services --output-on-failure
cmake -S tools/main_host_check -B builds/t1b-main && cmake --build builds/t1b-main && ctest --test-dir builds/t1b-main --output-on-failure
cmake -S tools/t13_host_check -B builds/t1b-t13 && cmake --build builds/t1b-t13 && ctest --test-dir builds/t1b-t13 --output-on-failure
# 其余 test/* 工程同样用 builds/t1b-<name> 与 builds/t1b-asan-<name>
```

T1b DONE
