# G-B GNSS main 侧集成（ADR-0010 §2.7/§2.8）

执行方：pi + antigravity/gemini-3.8-flash（或 Claude 指定的模型）。分支 `feat/gnss`（不要切分支、不要 commit）。
与 G-A 并行：G-A 负责 `components/gnss/src/**` 与 `test/gnss/**`（纯逻辑实现）。**你不得修改这些路径**。
在 G-A 完成前，gnss 组件源文件可能缺失导致 IDF 链接/编译失败：先写代码，构建验证时若唯一错误是 `components/gnss/src/...` 缺失，
每 5 分钟检查一次（`ls components/gnss/src`），等 G-A 的报告 `docs/reports/G-A.md` 末行出现 `G-A DONE` 后再跑全部门槛。

先完整阅读 `AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0010-gnss.md`、`docs/adr/ADR-0007*`（模块/执行器）、`docs/adr/ADR-0009-trace-console.md`、
`main/modules/_template/README.md`，再读冻结头 `components/gnss/include/gnss/*.hpp`、`main/def/gnss_def.hpp`、`components/app_fw/include/fw/{trace,command,store_key,module}.hpp`、
`components/rtos/include/rtos/{executor,static_stream_buffer}.hpp`、`components/link_core/include/core/reply.hpp`，以及 `main/app_main.cpp`、`main/app_config.hpp`、`main/app_context.hpp`、
`main/module_list.inc`、`main/data/app_data.*`、`main/modules/{rtcm,storage,bt_link,debug,system}/`、`components/link_core/services/{alarm_service,device_service}.cpp`。

## 文件归属

- `main/**`（除冻结的 `main/def/gnss_def.hpp`）、`main/CMakeLists.txt`
- `components/link_core/services/alarm_service.cpp`、`device_service.cpp`（只为 fix_quality/board 字段）
- `test/services/**`、`test/app_fw/**`、`tools/main_host_check/**`、`tools/t13_host_check/**`（只为跟随改动修编译/补用例）
- `docs/reports/G-B.md`

## 要做

1. **配置与登记**：`ModuleKind` 末尾追加 `kGnss`，`ExecKind` 追加 `kIo`；`module_list.inc` 在 `CONFIG_RTK_GNSS_ENABLE` 下登记 `MODULE_DEF(GnssModule, gnss, "gnss", kGnss, kIo, 0)`；`app_main.cpp` 新增 `kIoConfig`（name "io"，prio 9，core 0，栈 4096，idle_wait 20，round_budget 20，watchdog true，wait 钩子调用 `s_app.gnss.wait(max_ms)`），仅 GNSS 启用时创建并启动；`static_assert(CONFIG_RTK_GNSS_UART_NUM != 控制台 UART)`。`system` 模块与 `sys tasks`/`status` 中按现有写法加入 io 执行器统计与栈 HWM。
2. **GnssModule**（`main/modules/gnss/gnss_module.{hpp,cpp}`，按 `_template`）：
   - init：`uart_driver_install(port, 4096, 1024, 0, nullptr, 0)`、`uart_param_config`（8N1、`CONFIG_RTK_GNSS_DEFAULT_BAUD`）、`uart_set_pin`；电源脚 `gpio_config` 输出（POWER_GPIO = -1 时跳过）；`demux::StaticDemux` 挂 `demux::nmea`、`demux::rtcm3`、`demux::unicore_hash`、`demux::ubx`（存在则）及 `enabled_dialects()` 各自 `decoders`（地址去重，上限 8）；`Controller::init` 用 NVS 提示（ns "gnss"，见第 5 条）、波特率表（Kconfig 默认在前 + 115200/460800/921600/230400/38400/9600 去重）、`RoverConfig{rate_hz, gsv, port=CONFIG_RTK_GNSS_PORT}`；`start(now)`。
   - `wait(max_ms)`：`uart_read_bytes(port, buf, 512, pdMS_TO_TICKS(max_ms))`，有数据 → `demux.feed` → 帧回调：所有帧 `controller.on_frame`，NMEA 帧另交 `NmeaParser::feed`；kFix → 写 `gnss_fix` Slot，kSatsReady → 写 `gnss_sats` Slot。
   - poll：循环 `controller.poll(now)` 直到 kNone，执行动作（kPower → gpio 电平，考虑 ACTIVE_HIGH；kSetBaud → `uart_set_baudrate` + `uart_flush_input` + 发 kBaud 追踪；kWrite → `uart_write_bytes`）；状态变化/探测/步骤事件按 `GnssTrace` 发追踪（a/b 含义见 gnss_def.hpp）；然后排空 RTCM 管道写 UART（每轮最多一次 `uart_write_bytes`，写不下留到下轮，`written_bytes` 计数）；每 500 ms 或状态变化时写 `gnss_status` Slot。进入 kRunning 后若探测结果与 NVS 不同，经 storage 写 family/model/baud。
   - 命令（CommandBus，verb 见 `GnssVerb`）：kProbe → `request_reprobe`；kReset → `stop`+`start`；kSetRate → 校验 1/2/5/10/20、改 `RoverConfig`、写 NVS、`request_reconfigure`；kSend → 状态须 kRunning/kFailed，否则返回错误；payload 文本 + `\r\n` 直接写 UART。
3. **RTCM 管道**：`RtcmPipe`（`main/modules/gnss/rtcm_pipe.{hpp,cpp}` 或放在 `AppContext`）= `rtos::StaticStreamBuffer<app::kRtcmPipeBytes>` + `RtcmPipeStats`（原子或在 Slot 内由写端/读端各自累加的计数，说明并发方案）；`push(frame)`：`space() < frame.size()` → 整帧丢弃、计数、发 `kPipeDrop` 追踪；否则一次写入。`rtcm` 模块在 PHONE 方向每个完整帧转发给手机/报警器之后调用 `push`（GNSS 关闭时编译掉）。控制器不在 kRunning/kConfiguring/kProbing 之外（即掉电期间）时 gnss 模块丢弃管道内容。
4. **DataCenter**：`app_data.hpp` 新增 `gnss_status`（`GnssStatus`）、`gnss_fix`（`gnss::Fix`）、`gnss_sats`（`gnss::SatTable`），GNSS 关闭时也保留类型但不写（或按现有条件编译风格，二选一并说明）。
5. **NVS**：`main/modules/gnss/gnss_keys.hpp` `kKeys[]`：ns `"gnss"`：`family` u8、`model` u8、`baud` u32、`rate` u8、`gsv` u8（`fw::StoreKey` 若无 u8 类型则用最近的已有类型并说明），`static_assert(fw::valid(...))`；在 storage 模块 `kKeyTables` 登记（GNSS 启用时）。
6. **链路**：`bt_link` 每轮比较 `gnss_fix`/`gnss_status` 的版本，更新 `DeviceInfo::fix_quality`（GGA `update_ms` 距今 > 3000 ms → `kValueUnknown`）、`board`（`gnss::model_name`）、`board_fw`（fw_version）；`AlarmService::make_status` 用 `info.fix_quality` 替换固定 `kValueUnknown`。`test/services` 加用例：fix_quality 写入后 0x40 字节 4 等于该值，默认 0xFF。
7. **控制台** `main/modules/gnss/gnss_cmds.cpp`：`gnss status|fix|sats|probe|reset|rate <hz>|send <text>`，全部 `cli::out_*`/`cli::print_kv`，成功末行 `OK`，参数错误 `cli::fail`；读 Slot，不读模块私有状态；改状态经 `Executor::call` 投递到 kIo。`fix` 打印 UTC、质量名、纬经度（1e-9 度整数转 `%d.%09u` 形式，禁止 float printf）、高度 m（mm 定点）、卫星数、HDOP、差分龄期/基站号；`sats` 每星一行（系统、PRN、仰角、方位、SNR、used）。`status` 输出状态、家族/型号/固件、波特率、能力位、ControllerStats、NmeaStats、UART/demux/管道计数。
8. **追踪文本**：`main/modules/debug/trace_text.cpp` 为 `GnssTrace` 每个码加一项文本（状态/家族/型号用名称显示）。
9. Kconfig 已由 Claude 写入 `main/Kconfig.projbuild` 的 `menu "GNSS (ADR-0010)"`，不要改选项名；如需新增选项先在报告提出。

## 门槛（写入 `docs/reports/G-B.md`，逐项 PASS/FAIL 附命令）

1. `IDF_COMPONENT_MANAGER=0 idf.py -B builds/gb build` 0 告警；`size` 与 main 分支基线（`git stash` 禁止——用 `git worktree add ../wt-base main` 构建到 `builds/gb-base`，结束后 `git worktree remove`）对比 DRAM/IRAM/镜像，逐项解释增量。
2. `CONFIG_RTK_GNSS_ENABLE=n`（`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;<scratch defaults>"` 构建到 `builds/gb-off`）0 告警，且与基线 DRAM 差 ≤ 512 B；`CONFIG_RTK_TRACE_ENABLE=n` 也构建一次（`builds/gb-notrace`）。
3. 全部 host 测试普通 + ASan：test/host、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/cli、tools/main_host_check、tools/t13_host_check、test/gnss（G-A 的），用例数只增不减。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/gb` PASS；clang-format 覆盖改动文件；`sha256sum -c docs/frozen.sha256` 全 OK。
5. 代码清单（文件:行）：kIo 路径无阻塞超过 wait 钩子的调用、无 `ESP_LOG`/`printf`；`uart_write_bytes` 只在 kIo 任务；RTCM push 在 kLink 只做一次 `xStreamBufferSend`（零等待）；无新增堆分配（g7b 证明）。
6. 报告给出**预期**控制台样例（`gnss status`、`gnss fix`、`gnss sats` 前几行、`debug on gnss` 后冷启动的追踪行），供 HIL 对照。

报告最后一行 `G-B DONE` 或 `G-B BLOCKED: <原因>`。冻结头有问题 → 停止并在报告写明。

## 禁止

修改冻结文件与归属外文件；改变协议、NVS 命名空间 "rtk"/"dbg" 及其键、蓝牙名、已有 Kconfig 选项名；烧录、打开串口、联网安装、git commit/切分支。构建目录只用 `builds/gb*`。
