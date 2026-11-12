# J1 执行器实现 + app_link 模块化拆分（BATCH-J，ADR-0007）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`（全文，重点 §2、§3.2–§3.5、§4.1–§4.4、§7、§9），
`components/app_fw/include/app/*.hpp`、`components/rtk_os/include/rtk/os/{executor,spin_lock,static_task,static_queue}.hpp`，
以及现有 `main/app_link.{hpp,cpp}`、`main/app_console.cpp`、`main/app_main.cpp`、`main/app_rtcm.{hpp,cpp}`、`tools/main_host_check/`。

## 交付
1. `components/rtk_os/executor.cpp`：实现 `executor.hpp` 全部语义；`components/rtk_os/CMakeLists.txt` 加源文件，REQUIRES 增加 `app_fw`、`esp_system`（看门狗）。
   - 每轮：处理命令队列（本轮开头）→ 按 `poll_period_ms` 调用本执行器的模块 `poll` → 记录轮次耗时与 overrun → 有 pending 则下一轮等待 0，否则调用 `wait` 钩子（无钩子时阻塞在命令队列上，最长 `idle_wait_ms`）→ `vTaskDelay(yield_ticks)`。
   - `init` 只对 `executor == cfg.id` 的模块调用 `ops->init`；失败模块记为不可用（不 start/poll，命令返回 `kInvalidState`），不阻止其他模块。
   - `call()`：单调用者，seq 匹配，超时 `kTimeout`；迟到结果丢弃（不得错配给下一次调用）；结果队列满丢最旧并计入 `result_drops`。
   - 统计用 `stats_mux_` 保护，`stats()` 可在任意任务调用。
2. `main/` 拆分，删除 `main/app_link.{hpp,cpp}`：
   - `main/app_ids.hpp`：`enum class ModuleKind`、`enum class ExecKind`（只有 kLink），以及各模块的 verb 枚举。
   - `main/app_data.hpp`/`.cpp`：ADR-0007 §3.4 全部主题，每个主题一个 `constinit app::Slot<T>`，绑定 `rtk::os::spin_lock_ops`；`kSystemMode` 只登记（值固定为 `kNormal`）。
   - `main/bt_link_module.{hpp,cpp}`：会话表、事件处理、`drain_rx`、分流、`LinkService` 调用、`tick`；**调度契约与基线逐条一致**（每轮最多一个产品帧、256 B/20 ms 预算、64 B 预读跨轮保留、轮询游标、`rx_pending` → poll 返回 true）。仍使用现有 `CommandDispatcher`（J2b 再换）。等待钩子用 `rtk_bt_spp_poll_event`。
   - `main/storage_module.{hpp,cpp}`：白名单与 SN 的 `app::StoreKey` 表；去抖 500 ms；失败按 1→2→4…30 s 退避重试；`StoreStatus` 主题。
   - `main/rtcm_module.{hpp,cpp}`：包装现有 `RtcmForwarder`（`app_rtcm.*` 可保留或并入），`rtcm reset` 走 on_command。
   - `main/system_module.{hpp,cpp}`：`bt_ready`、设备名/SN、`ExecStats`（含栈余量）发布。
   - `main/app_main.cpp`：`constinit` 模块表 `kModules[]` 与 kLink 的 `ExecutorConfig`（core1、prio 10、栈 6144、`idle_wait_ms` 20、`round_budget_ms` 20、`yield_ticks` 1、watchdog）。
3. `main/app_console.cpp`：只做最小适配——读状态改为读各主题 Slot，改状态命令改为 `Executor::call`（`kCommandTimeoutMs` 1000 不变）。命令名、参数、输出文本保持不变，`status` 增加一行 `exec: rounds=.. overruns=.. max_round_ms=.. commands=.. result_drops=.. stack_free=..`；`stats:`、`rtcm:` 行一个字节都不能变（上板脚本依赖）。rtk_console 组件化是 J3 的事，本任务不做。
4. `tools/main_host_check`：随拆分更新源文件列表与桩，现有用例全部保留并通过（两种策略）。
5. 报告 `docs/reports/J1.md`：含 `idf.py size` 与基线对比（DRAM、IRAM、flash、app 分区余量）、链路任务栈水位的推算。

## 要求
- AGENTS.md 全部 C++ 规则；模块操作表为 `constexpr app::ModuleOps`；模块实例 `constinit`；不得新增虚函数。
- 模块之间不得直接调用对方方法：bt_link → storage 的白名单变化经主题或 storage 的 `mark_dirty` 接口（同一执行器内允许的单向调用须在头文件注释中说明），其余跨模块只走 Slot / Executor::call。
- 行为不变：除 ADR-0007 §7 第 1、2 条外，任何协议字节、时序、控制台输出都不得改变。
- 源文件头 `Generated-by: pi + codex/gpt-6.1-sol`，人工负责人 `UNKNOWN（待人工审查签字）`。

## 自检（报告逐项 PASS/FAIL 并给出命令）
1. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py build`：0 warning。
2. `tools/main_host_check` 两种策略 ctest 通过。
3. `test/host`、`test/stream_demux` 普通与 ASan/UBSan 回归通过；`sha256sum -c docs/frozen.sha256` 全部 OK。
4. clang-format dry-run 对改动文件无违规。

## 并行约束
J2a、J4 同时运行，它们使用 `build-j2a/`、`build-j4/`。本任务独占默认 `build/` 与根目录 `sdkconfig`；不得删除或清理其他 `build-*` 目录。

## 禁止
修改冻结文件（AGENTS.md §4，含 ADR-0007 与 J0 新增的所有 include 头）；修改 `components/rtk_core/**`（J2a）、`components/app_fw/**`、`components/bt_reconnect/**`、`test/app_fw/**`（J4）、`test_apps/**`；
烧录、monitor、连接串口、联网安装、git commit。完成后最后一行输出 `J1 DONE` 或 `J1 BLOCKED: <原因>`。
