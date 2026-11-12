# T2 控制台按组重排、status 分节、sys/log 命令（ADR-0009 §2.5）

执行方：pi + antigravity/gemini-3.8-flash。前置：T1a、T1b 完成。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0009-trace-console.md`、`docs/CODING-STYLE.md`、`docs/reports/T1a.md`、`docs/reports/T1b.md`，再读 `components/cli/include/cli/console.hpp`、`main/modules/*/*_cmds.cpp`、`main/modules/debug/`、`main/support/`、`tools/hil/spp_rtcm_mix.py`（`SerialConsole`、`RTCM_LINE_RE`、`STATS_LINE_RE`）。

## 要做

1. 命令按 ADR-0009 §2.5 表重排（全部用 `cli::Verb` + `dispatch` + `add_verbs`，失败一律 `cli::fail("<原因>")`，原因用简短英文如 `bad MAC`、`pairing busy`、`state 0-2`）：
   - `bt list | pair [秒] | policy`（原 `pair`、`policy`，会话列表来自原 status）；`wl list | add <MAC> | del <MAC> | clear`（`add/del` 需要 LinkManager 支持：若现有接口没有单条增删，**本步只做 list/clear，add/del 打印 `ERR not supported` 并在报告列为后续项**，不改冻结头）；`alarm show | set | clear`；`rtcm stats | reset`；`store show | sn <值>`；`log level <tag|*> <E|W|I|N>`；`sys info | heap | tasks | reboot`。
   - `status [link|rtcm|sys|all]`：默认一屏概览（每节 2–4 行要点）；分节用 `cli::print_section` + `cli::print_kv`，带单位（KB、ms、B/s）。**`status rtcm` 与 `status all` 必须原样包含 ADR-0006 §2 的 `rtcm: frames=… …` 与 `stats: frames_rx=… …` 两行**（字段顺序与名称不变）。
   - `sys tasks`：已知任务（link、async_tx、spp_worker、trace_out、console、main/idle 可选）的核号、优先级、栈余量（`uxTaskGetStackHighWaterMark`，字节）；`sys heap`：free / min / largest block（内部 RAM）；`sys info`：版本、IDF 版本、芯片 rev、BT MAC、SN、运行时长、复位原因；`sys reboot` 打印 `OK` 后 100 ms 再 `esp_restart`。
   - 旧顶层命令 `pair`、`policy`、`sn` 删除（不留别名）；`help` 由 cli 自定义实现。
2. `tools/hil/spp_rtcm_mix.py`：`read_status` 发 `status rtcm`；`reset_rtcm` 等待 `OK`；`--selftest` 样例同步。全仓搜索其他发送旧命令的脚本（`tools/`、`scratchpad` 不在仓库内不管）并同步。
3. `main/modules/_template/template_cmds.cpp` 改为 Verb + dispatch 写法，`tools/main_host_check` 的 template 用例同步。
4. `tools/main_host_check` 中引用旧命令输出的断言同步，用例数不减。

## 门槛（写入 `docs/reports/T2.md`）

1. `IDF_COMPONENT_MANAGER=0 idf.py -B builds/t2 build` 0 warning；size 与 `builds/t1b` 对比并解释。
2. 全部 host 测试普通 + ASan/UBSan 通过（`builds/t2-*`）；`python3 -I tools/hil/spp_rtcm_mix.py --selftest` 通过。
3. g7b PASS；clang-format；frozen OK。
4. 报告附**完整的预期控制台转录**：`help`、`help wl`、`status`、`status rtcm`、`sys tasks`、`wl add zz`（ERR 示例），供 T3 上板逐条比对。

报告最后一行 `T2 DONE` 或 `T2 BLOCKED: <原因>`。

## 禁止

修改冻结文件；改变协议、NVS、蓝牙名、Kconfig 选项名；烧录、连接串口、联网安装、git commit。只用 `builds/t2*`。
