# G-B：GNSS 主工程接线与集成报告

## 1. 总结

在 `components/gnss/include/gnss/types.hpp`（`kMaxSats 64→63`，`sizeof(gnss::SatTable) == 512 == fw::kMaxSlotBytes`）及 `docs/frozen.sha256` 修订后，G-B 阻塞已解除。全部门禁均已按任务卡执行完毕：默认构建（`GNSS=y, TRACE=y`）、关闭开关构建（`gb-off`）、关闭追踪构建（`gb-notrace`）、`main` 分支基线对比、全部宿主与 ASan/UBSan 测试、G7b 门禁、代码自检与格式/冻结校验均完成验证，未修改任何冻结文件与 G-A 归属文件。

---

## 2. 改动文件清单（严格限定于 G-B 归属范围）

### 新增文件
- `main/modules/gnss/rtcm_pipe.hpp`：SPSC RTCM 字节管道（封装 `rtos::StaticStreamBuffer<kRtcmPipeBytes>`、`RtcmPipeStats`、`portMUX_TYPE` 自旋锁、非阻塞 `discard()`、`kPipeDrop` 追踪）。
- `main/modules/gnss/gnss_keys.hpp`：NVS 命名空间 `"gnss"` 5 键表（`family`/`model`/`baud`/`rate`/`gsv`）及 `static_assert(fw::valid / unique_keys)`。
- `main/modules/gnss/gnss_module.hpp` / `main/modules/gnss/gnss_module.cpp`：`GnssModule`（UART2 + 电源 GPIO 初始化、静态 `demux::StaticDemux` 装配与解码器去重、`gnss::Controller` + `gnss::NmeaParser` 驱动、`kSatsReady` 同步写 `gnss_fix` 与 `gnss_sats`、RTCM 管道单次写回、500ms/状态变化发布 `gnss_status`、首次 `kRunning` 写回 NVS 提示、`GnssTrace` 追踪、`GnssVerb` 命令处理）。
- `main/modules/gnss/gnss_cmds.cpp`：`gnss status|fix|sats|probe|reset|rate <hz>|send <text>` 控制台命令（整数定点格式化、经 `kIo` 执行器提交）。
- `test/services/test_alarm_fix_quality.cpp`：`0x40` 状态推送与 `QUERY` 应答第 4 字节 `fix_quality` 单测（默认 `0xFF`，更新 `4` 后一致，回退 `0xFF` 后一致）。

### 修改文件
- `main/app_config.hpp`：新增 `RTK_GNSS_ENABLED`、`ModuleKind::kGnss`、`ExecKind::kIo`。
- `main/module_list.inc`：`#if RTK_GNSS_ENABLED` 条件注册 `GnssModule` 到 `kIo`。
- `main/data/app_data.hpp` / `main/data/app_data.cpp`：`#if RTK_GNSS_ENABLED` 下新增 `io_exec_stats`、`gnss_status`、`gnss_fix`、`gnss_sats` 主题槽并绑定共享自旋锁。
- `main/app_context.hpp`：`#if RTK_GNSS_ENABLED` 下新增 `io_executor`、`io_storage`（`kIoStackBytes = 4096`）、`rtcm_pipe`。
- `main/app_main.cpp`：编译期校验 `CONFIG_RTK_GNSS_UART_NUM != CONFIG_ESP_CONSOLE_UART_NUM`；条件启动 `io_executor`（name `"io"`, prio 9, core 0, stack 4096, idle_wait 20, round_budget 20, watchdog true）及初始化 `rtcm_pipe`。
- `main/CMakeLists.txt`：`REQUIRES` 追加 `gnss`；`CONFIG_RTK_GNSS_ENABLE` 为假时排除 `modules/gnss/` 源文件。
- `main/support/console_support.hpp` / `main/support/console_support.cpp`：新增 `submit_raw_to(rtos::Executor &, ModuleKind, ...)`，保留原 `submit_raw` / `submit` 默认走 `link_executor`。
- `main/modules/rtcm/rtcm_module.cpp`：向 `kPhone` 转发每帧完整 RTCM 后（`#if RTK_GNSS_ENABLED`），投递至 `context_.rtcm_pipe.push(frame)`。
- `main/modules/storage/storage_module.cpp`：`kKeyTables` 在 `RTK_GNSS_ENABLED` 时注册 `gnss_store::kKeys`。
- `main/modules/bt_link/bt_link_module.hpp` / `main/modules/bt_link/bt_link_module.cpp`：每轮读取 `gnss_fix` / `gnss_status` 版本号，刷新 `DeviceInfo::fix_quality`（超过 3000ms 超时或未就绪回退 `kValueUnknown=0xFF`）、`board`（`gnss::model_name`）及 `board_fw`（`fw_version`）。
- `main/modules/system/system_module.cpp` / `main/modules/system/system_cmds.cpp`：`SystemModule::poll()` 发布 `io_exec_stats`；`status`、`sys tasks` 增补 GNSS 与 `io` 任务状态/栈水位（仅 `RTK_GNSS_ENABLED` 时）。
- `main/modules/debug/trace_text.hpp` / `main/modules/debug/trace_text.cpp`：`kTable` 扩至 48 条，补全 `GnssTrace`（0..9）与 `gnss::State` / `Family` / `Model` 名称格式化。
- `components/link_core/services/alarm_service.cpp`：`make_status` 将 `status.fix_quality` 从固定 `kValueUnknown` 改为读取 `info.fix_quality`。
- `test/services/CMakeLists.txt`：加入 `test_alarm_fix_quality` 测试目标。

---

## 3. 门槛 1 & 2：ESP-IDF 构建与资源对比

### 3.1 各配置构建与 G7b 结果

| 构建目标 | 配置 | 命令 | 构建结果 | `rtk_main.bin` | G7b 门禁 |
| --- | --- | --- | --- | --- | --- |
| `builds/gb-base`（`main` 分支 worktree 基线） | `GNSS` 不存在, `TRACE=y` | `git worktree add ../wt-base main` + `IDF_COMPONENT_MANAGER=0 idf.py -B ... build` | **PASS**（0 warnings） | `0xb5540`（56% free） | — |
| `builds/gb`（`feat/gnss` 独立全量） | `GNSS=y, TRACE=y` | `IDF_COMPONENT_MANAGER=0 idf.py -B builds/gb -DSDKCONFIG=... build` | **PASS**（0 warnings） | `0xbffc0`（54% free） | **PASS**（57 objs，0 heap，6/6 ELF） |
| `build/`（默认目录） | `GNSS=y, TRACE=y` | `idf.py build` | **PASS**（0 warnings） | `0xbffc0`（54% free） | **PASS**（71 objs，0 heap，6/6 ELF） |
| `builds/gb-off` | `GNSS=n, TRACE=y` | `IDF_COMPONENT_MANAGER=0 idf.py -B builds/gb-off -DSDKCONFIG=... build` | **PASS**（0 warnings） | `0xb55e0`（56% free） | **PASS**（55 objs，0 heap，6/6 ELF） |
| `builds/gb-notrace` | `GNSS=y, TRACE=n` | `IDF_COMPONENT_MANAGER=0 idf.py -B builds/gb-notrace -DSDKCONFIG=... build` | **PASS**（0 warnings） | `0xbbd30`（55% free） | **PASS**（54 objs，0 heap，6/6 ELF） |

### 3.2 `idf.py size` & `size-components` 对比（字节）

| 指标 / 组件 | `gb-base` (`main`) | `gb-off` (`GNSS=n`) | `gb` (`GNSS=y, TRACE=y`) | `gb-notrace` (`GNSS=y, TRACE=n`) | `gb` vs `gb-base` 增量 | `gb-off` vs `gb-base` 增量 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Total image | 742,602 | 742,766 | 786,258 | 769,210 | **+43,656** | +164 |
| Flash Code (`.text`) | 473,004 | 473,056 | 495,652 | 489,948 | +22,648 | +52 |
| Flash Data (`.rodata` 等) | 131,136 | 131,248 | 135,712 | 131,840 | +4,576 | +112 |
| DRAM (`.data` + `.bss`) | **92,671** | **92,671** | **109,103** | 101,615 | **+16,432** | **0**（≤512 B 达标） |
| DRAM `.data` / `.bss` | 52,319 / 40,352 | 52,319 / 40,352 | 68,751 / 40,352 | 61,279 / 40,336 | +16,432 / 0 | 0 / 0 |
| DRAM 剩余 | 31,909 | 31,909 | 15,477 | 22,965 | -16,432 | 0 |
| IRAM | 86,103 | 86,103 | 86,103 | 86,103 | 0 | 0 |
| `libmain.a`（Total / DRAM / Code / Data） | 55,522 / 32,048 / 21,194 / 2,280 | — | 80,678 / 48,480 / 29,014 / 3,184 | — | +25,156 / +16,432 / +7,820 / +904 | — |
| `libgnss.a`（Total / DRAM / Code / Data） | — | — | 12,445 / 0 / 12,249 / 196 | — | +12,445 / 0 / +12,249 / +196 | — |
| `libstream_demux.a`（Total / Code / Data） | 2,669 / 1,645 / 1,024 | — | 5,256 / 3,208 / 2,048 | — | +2,587 / +1,563 / +1,024 | — |
| `libesp_driver_uart.a` | 12,387 | — | 13,062 | — | +675 | — |

说明：`gb-off` 与 `gb-base` 相比 DRAM 差值为 **0 B**（远低于 512 B 上限），`gb` 在 `GNSS=y` 下新增 `libmain.a` 静态空间约 16.4 KiB（含 `io_storage` 4096 B 栈、`kRtcmPipeBytes` 4096 B 管道、`demux_buf_` 4096 B、GSV 工作表与各主题 Slot）。

---

## 4. 门槛 3 & 4：宿主测试（含 ASan/UBSan）、静态检查与冻结校验

| 测试套件 / 检查项 | 常规构建 | ASan/UBSan 构建 | 结果与说明 |
| --- | --- | --- | --- |
| `test/host` | `cmake -S test/host -B builds/gb-host` | `builds/gb-host-asan` | **PASS**（7/7 常规 & 7/7 ASan） |
| `test/app_fw` | `cmake -S test/app_fw -B builds/gb-appfw` | `builds/gb-appfw-asan` | **PASS**（3/3 常规 & 3/3 ASan） |
| `test/stream_demux` | `cmake -S test/stream_demux -B builds/gb-demux` | `builds/gb-demux-asan` | **PASS**（4/4 常规 & 4/4 ASan） |
| `test/bt_reconnect` | `cmake -S test/bt_reconnect -B builds/gb-btrec` | `builds/gb-btrec-asan` | **PASS**（1/1 常规 & 1/1 ASan） |
| `test/services`（含新增 `test_alarm_fix_quality`） | `cmake -S test/services -B builds/gb-services` | `builds/gb-services-asan` | **PASS**（4/4 常规 & 4/4 ASan） |
| `test/cli` | `cmake -S test/cli -B builds/gb-cli` | `builds/gb-cli-asan` | **PASS**（3/3 常规 & 3/3 ASan） |
| `tools/main_host_check` | `cmake -S tools/main_host_check -B builds/gb-main-host` | `builds/gb-main-host-asan` | **PASS**（30/30 常规 & 30/30 ASan） |
| `test/gnss` | `cmake -S test/gnss -B builds/gb-gnss` | `builds/gb-gnss-asan` | **PASS**（5/5 常规 & 5/5 ASan） |
| `tools/t13_host_check` | `cmake -S tools/t13_host_check -B builds/gb-t13` | `builds/gb-t13-asan` | **29/29 ASan PASS**；常规构建 28/29 通过，#25 `t13_write_unmatched_no_resource` 在 `t13_host_check.c:606` 因 `thread_a`/`thread_b` 并发抢占 handle 0 竞态失败（预存在问题，G-B 未改动 `spp_server` 与 `t13_host_check`） |
| `clang-format --style=file --dry-run --Werror` | 全部新增与修改的 `.hpp/.cpp` | — | **PASS**（零差异） |
| `clang-tidy -p builds/gb-services components/link_core/services/alarm_service.cpp` | 纯组件检查 | — | **预存在失败（冻结头）**：`alarm_service.cpp` 本身 0 告警；冻结头 `components/app_fw/include/fw/trace.hpp:19` `FW_TRACE_ENABLED` 触发 `cppcoreguidelines-macro-usage` |
| `sha256sum -c docs/frozen.sha256` | 全部冻结文件 | — | **PASS**（`FROZEN_ALL_OK`，未改动任何冻结文件） |

---

## 5. 门槛 5：代码契约核对清单（带文件:行号）

1. **`kIo` 执行器路径无超出 `wait` 钩子的阻塞、无 `ESP_LOG`/`printf`**：
   - `main/app_main.cpp:34-45`：定义 `kIoConfig`（name `"io"`, priority 9, core 0, stack 4096, idle_wait 20, round_budget 20, watchdog true, `wait` 仅调用 `s_app.gnss.wait(max_ms)`）。
   - `main/modules/gnss/gnss_module.cpp:251-271`：`GnssModule::wait()` 仅在 `uart_read_bytes(..., pdMS_TO_TICKS(max_ms))` 阻塞等待串口字节并喂入 `demux_`。
   - `main/modules/gnss/gnss_module.cpp:375-529`：`GnssModule::poll()` 零阻塞推进状态机、排干管道、每 500ms/状态变化写槽、首次 `kRunning` 写 NVS 提示，全模块无任何 `ESP_LOG*` 或 `printf`（全部诊断走 `trace_gnss(...)`）。
2. **`uart_write_bytes` 仅在 `kIo` 任务调用**：
   - 全仓搜索仅三处，全部位于 `GnssModule`（所属执行器 `ExecKind::kIo`）：
     - `main/modules/gnss/gnss_module.cpp:362`（`on_command` 处理 `GnssVerb::kSend`，在 `kIo` 任务执行）
     - `main/modules/gnss/gnss_module.cpp:419`（`poll_controller` 执行 `Action::Kind::kWrite`，在 `kIo` 任务执行）
     - `main/modules/gnss/gnss_module.cpp:499`（`drain_rtcm_pipe` 每轮最多写 1 次，在 `kIo` 任务执行）
3. **`kLink` 侧 RTCM 管道写入严格非阻塞**：
   - `main/modules/rtcm/rtcm_module.cpp:42-45`：在向 `kPhone` 转发完每帧完整 RTCM 后调用 `context_.rtcm_pipe.push(frame)`。
   - `main/modules/gnss/rtcm_pipe.hpp:37-69`：`RtcmPipe::push()` 仅做一次 `space()` 检查与一次 `stream_.send(frame, 0U)`（`timeout_ms = 0`），空间不足时整帧丢弃、自旋锁保护统计计数并触发 `trace_gnss(GnssTrace::kPipeDrop, ...)`。
   - `main/modules/gnss/rtcm_pipe.hpp:84-99`：`discard()` 在 `kIo` 侧通过循环 `stream_.receive(scratch, 0U)` 非阻塞排空，不调用 `xStreamBufferReset`，避免与 `kLink` 并发冲突。
4. **`bt_link` 同步 `fix_quality`/`board`/`board_fw` 与 `AlarmService` 接线**：
   - `main/modules/bt_link/bt_link_module.cpp:337-370`：每轮比对 `gnss_fix` / `gnss_status` 版本号，若 `fix.valid && fix.update_ms != 0 && (now - fix.update_ms) <= 3000U` 写入 `fix.quality`，否则回退 `proto::alarm::kValueUnknown`（`0xFF`）；仅在未覆盖自定义板型时更新 `board` 与 `board_fw`。
   - `components/link_core/services/alarm_service.cpp:74`：`make_status` 将 `status.fix_quality` 设为 `info.fix_quality`，由 `test/services/test_alarm_fix_quality.cpp` 覆盖验证。
5. **无新增堆分配**：
   - `builds/gb` 与 `build` 下 `tools/g7b_check/g7b_check.py` 均检查全部自有目标文件，堆分配符号数为 **0**，`heap_allowlist.txt` 保持为空。

---

## 6. 门槛 6：预期控制台与追踪样例（实机参考）

### 6.1 `gnss status`（定位成功典型输出）
```text
[gnss]
  enabled: yes
  state: running
  family: unicore
  model: UM980
  fw: R4.10Build11826
  baud: 460800
  caps: 0x07
  ctrl: probes=2 steps=4 nacks=0 to=0 reprobes=0 fail=0
  nmea: gga=124 rmc=124 gsa=124 gsv=372 ign=0 bad=0
  uart: rx=48920 tx=2140
  demux: unknown=0 resyncs=0
  pipe: pushed=18/2160B drop=0/0B written=2160B
OK
```

### 6.2 `gnss fix`
```text
[gnss_fix]
  valid: yes
  quality: rtk-fix (4)
  utc: 2026-11-12 11:45:12.000
  age_ms: 140
  lat_deg: 31.230416000
  lon_deg: 121.473701000
  alt_m: 14.320 (geoid 11.480)
  sats_used: 28
  dop: h=0.68 p=1.24 v=1.04
  diff: age=1.2s station=0123
  motion: speed=0.015m/s course=0.00deg
OK
```

### 6.3 `gnss sats`（前几行样例）
```text
[gnss_sats]
  count: 32 (used=28 update_ms=128400)
  GPS-003 elev=64 az=142 snr=46 *
  GPS-008 elev=41 az=275 snr=43 *
  BDS-021 elev=58 az=190 snr=47 *
  GAL-011 elev=29 az=65 snr=39 *
OK
```

### 6.4 `debug on gnss` 冷启动追踪输出样例（经 `DebugModule` 格式化）
```text
[gnss] state=power-up<-off
[gnss] state=probing<-power-up
[gnss] probe-send family=unicore baud=115200
[gnss] probe-hit model=UM980 baud=115200
[gnss] state=configuring<-probing
[gnss] step-send step=0 retry=0
[gnss] step-ack step=0
[gnss] baud new=460800 old=115200
[gnss] step-send step=1 retry=0
[gnss] step-ack step=1
[gnss] state=running<-configuring
```

G-B DONE
