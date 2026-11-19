# G-baud460k：探测后默认把 GNSS 串口切到 460800（2026-11-19 Architect）

依据：用户 2026-11-19 要求“探测到 GNSS 模块后，所有模块默认 UART 460800”。P1 HIL（P2 v1.41 + CM680，115200）观察到 `io_exec overruns=467/26827 max_rnd=44 ms`（预算 20 ms）；推测之一是 RTCM 写 UART 时 1 KB TX 缓冲满而阻塞（115200 下 1 KB ≈ 89 ms，460800 下 ≈ 22 ms）。

分支 `feat/g-baud460k`，基线为 P1 合入后的 main。

## 1 行为

1. 新 Kconfig `RTK_GNSS_TARGET_BAUD`（默认 460800，范围 9600–921600，取 `validUserBaud` 支持的值），含义：**探测成功后的运行波特率**。与现有 `RTK_GNSS_DEFAULT_BAUD`（首次探测起点）分开。
2. 目标值优先级：用户 NVS `gnss/baud`（`gnss baud <n>` 设定，非 0）> `RTK_GNSS_TARGET_BAUD`。目标 == 当前主机波特率时不切换。
3. 切换走现有机制：`RoverConfig::portBaud = 目标`，各驱动（Unicore/CM680、Comnav、u-blox）已有改波特率脚本步骤与 `switchBaud` → `Action::kSetBaud`。审查各驱动该步骤是否对所有型号成立，CM680 用 `CONFIG COMn <baud>`（CM680 协议 PDF V1.2.8 核对该命令与应答；不支持则该型号回退保持当前波特率并记录 trace，不得让配置流程卡死）。
4. **每次启动只改运行波特率，不发 SAVECONFIG**（避免每次上电写模块 flash；模块掉电后回到出厂 115200，下次探测照常从提示/候选表找到）。`gnss baud <n>` 的现有持久化（SAVECONFIG）语义不变。
5. 探测提示 `gnss.cache` 的 baud 记录**切换后的实际波特率**，使 ESP32 单独复位（模块未掉电、仍在 460800）时第一候选即命中；模块掉电后则由候选表中的 115200 命中。验证两种场景下探测都能在现有超时内完成。
6. 切换失败（命令无应答或切换后探测不到）：回退到切换前波特率重新探测一次，trace + `status gnss` 计数 `baud_fail`，不重复无限尝试（每次启动最多 1 次）。
7. `status gnss` 的 `state` 行已显示 `baud=`，确认显示的是切换后的值。

## 2 约束与测试

- 无堆；不改冻结头（如必须改 `components/gnss/include/**`，在报告中列出并说明，等待 Architect 授权）。
- 主机测试：目标等于当前不切换；用户 NVS 覆盖 Kconfig；切换后 hint 记录新值；切换失败回退一次；SAVECONFIG 不在默认启动脚本中出现。
- 门禁 `tools/ci/run_gates.sh --base <P1 合入后的 main> --prefix gb`；报告 `docs/reports/G-baud460k.md`（含 §文档待改：acceptance.md、ADR 中 GNSS 波特率描述）；打印 `GB DONE`。不烧录、不提交。
- HIL（Architect）：P2 冷上电、ESP32 单独复位两种场景 `baud=460800`；20 Hz 下 `pipe drop=0`、app_ring 无丢；10 分钟 RTCM 下对比 `io_exec overruns`。
