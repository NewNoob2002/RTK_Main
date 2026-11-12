# T1b 追踪事件点接入 + DebugModule（ADR-0009）

执行方：pi + workbuddy/deepseek-v4.1-flash。前置：T1a 已完成（`fw::trace`、`TraceRing`、`cli::fail/Verb/dispatch/add_verbs` 可用）。
先完整阅读 `AGENTS.md`、`docs/adr/ADR-0009-trace-console.md`、`docs/CODING-STYLE.md`、`docs/reports/T1a.md`，再读冻结头
`components/app_fw/include/fw/trace.hpp`、`components/link_core/include/core/trace_codes.hpp`、`components/spp_server/include/spp_server.h`（追踪钩子段）、`components/app_fw/include/fw/store_key.hpp`，以及 `main/modules/_template/README.md`、`main/module_list.inc`、`main/app_context.hpp`、`main/app_main.cpp`。

## 要做

0. 说明：`test/host/CMakeLists.txt`（冻结）已由 Claude 在 T1a 合入后加上 `components/app_fw/src/trace.cpp`；`tools/main_host_check`、`test/services`、`tools/t13_host_check` 的 CMake 若因 link_core 调用 `fw::trace` 而链接失败，由你补上 `trace.cpp`。

1. **link_core 事件点**（只加 `core::trace_*` 调用，不改逻辑与冻结头）：`link_manager.cpp`（open 角色+LinkReason、close、kick、alarm rx 超时、pending 超时、角色变化、配对窗口开/关、白名单 add/clear）、`link_service.cpp` 与 `command_router.cpp`（帧、应答、无路由、CRC/长度错误、应答写失败）、`services/alarm_service.cpp`（状态、override、推送、推送失败）。按 `trace_codes.hpp` 注释填参数。`test/host`、`test/services` 用例数与结果不变（未安装 sink 时零副作用）；在 `test/services` 新增一个用例：安装记录型 sink，驱动一次 open/HELLO/close，断言事件序列。
2. **spp_server 钩子**：驱动内 `static _Atomic spp_server_trace_fn`（或 `volatile` + 内存屏障，说明理由）；在 SRV_START、CONG 变化、写完成/失败（带字节与耗时）、RX 溢出、AUTH 完成、事件队列满处调用。C 文件不得含 C++ 头。
3. **main 事件点**：`rtcm` 模块在 demux 回调中发 kRtcm（每帧 msg/len；拒绝原因；重同步；unknown bytes），事件码定义在 `main/def/rtcm_def.hpp`；`storage` 模块发 kStore（load/save 结果、去抖触发、重试 ms），码在 `main/def/storage_def.hpp`；执行器轮次超时与投递失败发 kExec（码在 `main/def/system_def.hpp`）。不得在这些路径加 `ESP_LOG`。
4. **DebugModule**（`main/modules/debug/`，在 `module_list.inc` 注册，按 `_template` 写法）：
   - 拥有 `fw::TraceRing` 与 256 条 `TraceEvent` 静态存储、`portMUX` 锁（`rtos::spin_lock_ops`）；启动早期 `fw::trace_install(TraceRing::sink, &ring, rtos::now_ms)`，并 `spp_server_set_trace_hook` 安装一个把 spp 码转发为 `fw::trace(kSpp, …)` 的 C 回调。
   - 输出任务 `trace_out`（prio 1，core 0，栈按实测 + ≥1 KB 余量）：每 20 ms 或环非空时取事件 → 查 (cat, code) 文本表 → 一行 `printf`：`[  12.345] link  open  h=0081 role=ALARM why=whitelisted`（时间秒.毫秒、类别名左对齐 5、事件名、键=值）；先输出 `[trace] dropped N`（若 take_dropped > 0）。文本表集中在 `main/modules/debug/trace_text.cpp`，每个码一项，未知码打印 `cat=… code=… a=… b=…`。
   - 周期报告：`report_s_` > 0 时每 N 秒一行 `[report] rtcm 39.8KB/s 221f/s drop+0 | prod 12/s | heap 142K min 136K | stk link 4.7K tx 2.3K | trace drop 0`；数据从 DataCenter 读，不跨任务读模块私有状态。
   - 命令组 `debug`（`cli::Verb` + `dispatch` + `add_verbs`）：`list`（每类别一行 `link   on   会话打开/关闭、角色判定` 风格，加 report 周期与 trace 环统计）、`on <类别…|all>`、`off <类别…|all>`、`report <秒|off>`（1..3600）、`save`。参数错误用 `cli::fail`。改状态经 `Executor::call` 投递到 DebugModule 所属执行器（ADR-0007 §4.3）；掩码本身是原子量，可直接 `trace_set_mask`。
   - 持久化（ADR-0009 §2.4）：`inline constexpr fw::StoreKey kKeys[]` 两项：ns `"dbg"` key `"mask"` kU32、key `"report"` kU16（0..3600），`static_assert(fw::valid(...))`。storage 模块新增通用“按 StoreKey 表 load/save”通路（启动时 load 所有登记表；`save_keys(span)` 同步写），`debug save` 调用它。现有 `sn`/`alarm_wl` 不迁移。nvs_store 若缺通用 get/set u32/u16 API，在 `nvs_store` 内部新增（`nvs_store.h` 是冻结头 → 不改；改为在 storage 模块内直接用 `nvs.h`，并在报告说明），NVS 命名空间 `"rtk"` 不受影响。
5. 堆：TraceRing 存储静态（.bss，4 KB），不得用堆。g7b allowlist 不应新增项。

## 门槛（写入 `docs/reports/T1b.md`，逐项 PASS/FAIL 附命令）

1. `IDF_COMPONENT_MANAGER=0 idf.py -B builds/t1b build` 0 warning；size 与 `builds/t1a` 对比：DRAM 增量 ≤ 6 KB，逐项解释。
2. 全部 host 测试（同 T1a 列表 + 新 services trace 用例）普通 + ASan/UBSan 通过，用例数只增不减；构建目录 `builds/t1b-*`。
3. g7b PASS；clang-format 覆盖改动文件；`sha256sum -c docs/frozen.sha256` 全 OK。
4. 代码检查清单（报告中逐条给出文件:行）：link 任务、async_tx 任务、spp 回调路径中无新增 `ESP_LOG`/`printf`；所有 `fw::trace` 调用点不在 ISR；输出任务是唯一的格式化者。
5. 在报告中给出一段**预期**控制台样例（`debug list`、`debug on link rtcm`、几行 trace、一行 report），供 T3 上板对照。

报告最后一行 `T1b DONE` 或 `T1b BLOCKED: <原因>`。

## 禁止

修改冻结文件；改变协议、NVS 命名空间 `"rtk"` 及其键、蓝牙名、Kconfig 选项名；烧录、连接串口、联网安装、git commit。只用 `builds/t1b*`。
