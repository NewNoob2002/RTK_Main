# T13 修复复审记录

日期：2026-11-10。Profile：esp-idf。用户指定在 Herdr 拆分窗口，通过 omp 的 DeepSeek 模型修复 T13，完成后复审。

## 执行方式与边界

- 在当前 tab 拆出右侧 pane `wF:p7`，使用 `omp --model magpie/workbuddy/deepseek-v4.1-flash --no-prewalk --no-session`。
- 修复前实现快照：`/tmp/rtk-t13-before-repair-20261110.tar`。工程在父仓库中整体未跟踪，保留用户已有文件，不依赖空 git diff 判断改动。
- 第一轮 DeepSeek 因上游 424 限流退出。已有驱动和 `tools/t13_host_check/` 回归框架保留；后续在同一 pane 重启相同模型接续。
- 两路辅助审查因服务 424/429 限流失败，没有产生可用审查结论。下列发现来自主窗口直接阅读源码与检查实际输出。
- 未连接串口、未烧录、未安装或下载依赖、未 commit/push、未改 T14/T15。用户通过表单明确批准了两处 rtk_os 冻结头及对应两条 `docs/frozen.sha256` 哈希的例外修改；主窗口已应用 DeepSeek 提供的补丁，并修正新增表达式的格式。

## 首轮修复发现与退回项

以下均已交回 DeepSeek 处理，不把初次写入等同于修复完成：

1. `rtk_bt_spp_wait_stage` 按值接收阶段 status，等待前取得的失败状态不会随异步回调更新，实际成功可能被误报失败。需在等待之后读取同步后的实际 INIT/START 结果。
2. `write` 在查找未知句柄前返回零预算 TIMEOUT，违反未知句柄应返回 NOT_FOUND 的冻结契约。
3. 分块认领与快速完成路径不检查共享预算，可在预算耗尽后继续提交后续块。
4. GAP 的 PIN/SSP 确认仍直接在 BTC 回调调用 IDF；本地 `esp_gap_bt_api.c:433,505` → `btc_task.c:366` 使用无限队列投递，需移到任务上下文。
5. 待接入表快照到实际发布 OPEN 之间可发生 CLOSE，造成幽灵会话；待接入期间 DATA 也会因找不到活跃会话而丢失。已要求简化为只在 BTC 中分配槽、无安全槽时由任务异步拒绝，不保留非需求的 1 秒重试接纳策略。
6. WRITE 完成只校验 status、未校验回报 len，短写可能被误报成功；无句柄的错误完成也不能把发送状态永久留在 SUBMITTED。
7. 冻结候选补丁把成功返回值从完整事件位 snapshot 改为 requested mask，超出本次修复范围；已要求成功保留原 snapshot，仅未满足等待条件返回 0。
8. 回归检查存在编译错误、volatile 跨线程同步、未实现的 100 ms 延迟假设，以及只占用一个槽却假定没有其他空槽的错误前提；已要求校正，并用真实 read 路径的确定性交错覆盖旧读者/新连接竞争。
9. 全局在途 owner 在 CLOSE 后等待不存在的 WRITE 回调，可能永久卡住后续会话。IDF `btc_spp.c:1188–1204` 在 CB 模式关闭时释放槽及 TX 队列，并未逐条合成 WRITE；已退回增加 CLOSE 无后续 WRITE 的恢复场景并修复。

## 已执行检查

| 项目 | 命令与实际结果 |
| --- | --- |
| 修复前冻结校验 | **PASS**：`sha256sum -c docs/frozen.sha256 --quiet`，exit 0 |
| 独立事件组契约基线 | **FAIL（预期复现）**：编译 `/tmp/rtk-t13-event-fixture/check.cpp` 后运行，4 个等待未满足却返回非零的断言失败。该 fixture 只模拟已核实的 FreeRTOS 事件位返回语义，用于检查包装层；不是实际 RTOS/HIL 证据 |
| 首轮新回归构建 | **FAIL**：`cmake -S tools/t13_host_check -B /tmp/rtk-t13-fix-host && cmake --build /tmp/rtk-t13-fix-host -j 4`；`shim/fake_idf.c:673` 调用 `fake_dispatch` 缺少第三个 `sync` 参数，原型位于 `:163` |
| 首轮定向 INIT 场景 | **NOT_RUN**：上述构建失败，`ctest --test-dir /tmp/rtk-t13-fix-host -R '^t13_init_waits_async$' --output-on-failure` 没有执行 |
| 第二轮新回归构建 | **FAIL**：同目录构建到 `t13_host_check.c:17` 找不到 `fake_idf.h`；CMake 没有传播 `shim/` 头目录，已退回修复，测试未执行 |
| ESP32 实际驱动编译 | **PASS**：`. ~/esp/esp-idf/export.sh >/dev/null && idf.py build`；`rtk_bt_spp.c.obj` 实际重新编译、组件归档及镜像链接成功，0 编译器 warning；IDF 自带 Kconfig NOTE 单独保留。后续生产代码变更仍需重新验证 |
| 纯组件普通回归 | **PASS**：`cmake -S test/host -B /tmp/rtk-host-t13-final && cmake --build /tmp/rtk-host-t13-final -j 4 && ctest --test-dir /tmp/rtk-host-t13-final --output-on-failure`，8/8 通过 |
| 纯组件 ASan/UBSan | **PASS**：配置 `/tmp/rtk-asan-t13-final`，C/C++/链接加 address,undefined（C++ 另加 frame-pointer/g）；构建后 ctest 8/8 通过，无 sanitizer 报告 |
| 冻结候选行为 | **PASS（临时副本）**：`g++ -std=c++20 -Wall -Wextra -Werror -I/tmp/rtk-t13-frozen-candidate -I/tmp/rtk-t13-event-fixture -Icomponents/rtk_proto/include /tmp/rtk-t13-event-fixture/check.cpp -o /tmp/rtk-t13-event-fixture/check-after && /tmp/rtk-t13-event-fixture/check-after`；timeout/any/all/clear 与成功完整快照契约通过；真实冻结头未改 |
| 本轮冻结复查 | **PASS**：`sha256sum -c docs/frozen.sha256 --quiet`，exit 0 |
| 最终驱动 host 回归 | **PASS（主窗口执行）**：`cmake --build /tmp/rtk-t13-fix-host -j 4 && ctest --test-dir /tmp/rtk-t13-fix-host --output-on-failure`，22/22，6.54 秒；包括 INIT/START、真实 read/reset 竞争、短写/匿名错误、共享预算、CLOSE 无 WRITE 及提交中 CLOSE 取消 |
| 最终 ESP32 构建 | **PASS（主窗口执行）**：`idf.py build`，exit 0；最终检查为增量，生产源已由 DeepSeek 在该修订重编。此前主窗口也观察过实际 `rtk_bt_spp.c.obj` 编译通过 |
| 最终驱动格式 | **PASS（主窗口执行）**：`clang-format --style=file --dry-run --Werror components/rtk_bt/rtk_bt_spp.c`，exit 0 |
| 驱动 ASan/UBSan | **PASS（实现方执行记录）**：`docs/reports/T13-FIX.md` 与 herdr 交付输出报告 22/22、无 sanitizer 报告；主窗口未重复执行此套驱动 sanitizer 检查 |
| 获授权的真实事件组头 | **PASS**：使用工程 `components/rtk_os/include` 编译独立行为检查；timeout/any/all/clear、成功完整快照均通过，原先 4 条失败已消除 |
| 获授权的头文件目标语法 | **PASS**：从 `build/compile_commands.json` 的 `spp_transport.cpp` 提取实际 Xtensa g++ 编译参数，对 `/tmp/rtk-t13-approved-headers.cpp` 以 `-fsyntax-only` 实例化 `StaticEventGroup`、`StaticTask<6144>`、wait 和高水位方法，exit 0，0 诊断 |
| 获授权的冻结哈希/格式 | **PASS**：仅更新 `static_event_group.hpp`、`static_task.hpp` 两条 SHA256；`sha256sum -c docs/frozen.sha256 --quiet` 与两头 `clang-format --dry-run --Werror` 均 exit 0。第一次格式检查发现续行缩进问题，格式化该表达式后通过 |
| 获授权改动后的目标构建 | **PASS**：`idf.py build`，exit 0；本次增量检查未实例化这些尚无应用调用方的 RAII 头，目标编译证据由上一行独立 Xtensa 实例化补足 |
| 硬件验证 | **NOT_RUN**：本次尚无实际 RTOS/连接/吞吐/拥塞/scatternet 运行证据 |

**最终结论：本轮 T13 指定修复通过软件复审，原报告 1–6 已处理；没有剩余的本轮阻塞发现。** 读者生命周期、真实 INIT/START 状态、GAP 阻塞投递迁移、发送共享预算/串行化、短写/错误归属、CLOSE 取消/迟到完成均已核对。22 项驱动回归、既有 8 项普通与 sanitizer 回归、ESP32 构建通过；获授权的事件组行为及两个头的实际 Xtensa 实例化通过。此结论只覆盖软件证据，连接/吞吐/拥塞/scatternet/HIL 未执行。

## 授权后的最终改动

- `components/rtk_bt/rtk_bt_spp.c`：DeepSeek 完成驱动修复；主窗口多轮源码复审、退回修正、独立执行最终回归。
- `tools/t13_host_check/`：驱动真实源码的独立 host 回归工程与 RTOS/IDF 依赖替身，22 个生命周期/故障场景；不修改冻结的 `test/host/**`。
- `components/rtk_os/include/rtk/os/static_event_group.hpp`：按用户本次例外授权修正等待未满足时返回 0；成功保留完整快照。
- `components/rtk_os/include/rtk/os/static_task.hpp`：按同次授权仅更正已核实的单位注释，不改变数值行为。
- `docs/frozen.sha256`：按同次授权只更新上述两头的校验值，其余条目不变。
- `docs/reports/T13-FIX.md`、本报告：实现与独立复审证据，保留前序失败记录；既有 QA-E/QA-E-REVIEW 历史记录不被改写为通过。

剩余限制：依赖 IDF 当前 CLOSE/WRITE 顺序与状态语义；控制请求已被任务取走后的极短句柄复用窗口及真实双会话时序尚无 HIL 证据，见实现报告风险节。当前 main 仍未集成这些组件，完整固件资源占用与运行就绪属于 T14；本次不宣称整机可交付。Herdr 右侧修复窗口保留，未关闭用户工作窗口。
