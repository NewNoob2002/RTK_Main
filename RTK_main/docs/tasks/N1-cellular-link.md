# N1 — 4G EC600U 驱动与链路状态机（ADR-0021 第一期）

- 分支/工作树：`feat/n1-cellular`，`../RTK_Main-n1`，基线为本卡提交
- 门禁：`tools/ci/run_gates.sh --prefix n1 --base 32a0dd7`（需 `export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`）
- 报告：`docs/reports/N1-cellular-link.md`；完成时输出 `N1-FINISHED`
- 设计依据：**`docs/adr/ADR-0021-cellular-ec600u.md`（已批准）**，本卡只列范围与验收，细节以 ADR 为准；冲突时先在报告中写明，不自行改 ADR。

## 硬件事实（已核对，ADR-0021 §2.1 / §5 Q1）
- EC600U：UART1 RX=GPIO32 / TX=GPIO33，上电 115200，`AT+IPR` 切 460800。
- 唯一控制脚 `GPIO26`=`4G_PWREN`（高有效，板上下拉，默认断电）。PWRKEY 经 1 kΩ 接地 → 上电自动开机；RESET_N/NET_STATUS/RI/DTR 未接。→ 无软开机/硬复位，L3 = PWREN 断电 ≥ 2 s 重上；无 RI → kNet 必须轮询 UART。
- 仅 `Feature::kCellular`（P2 v1.41 profile）启用；其他板型（E1 GPIO26 为电源保持）零初始化、不碰 GPIO26/UART1、不创建 kNet。

## 范围（ADR-0021 §4 N1 行）
1. `components/cellular/`（纯逻辑、无 IDF）：
   - `AtEngine`：命令队列（单在途）、行分帧、`OK/ERROR/+CME ERROR` 结果、URC 前缀分发（§2.4 列表）、`>` 提示与定长数据读、每命令超时、连续 3 次无响应 → 通道失效。
   - `LinkFsm`：Off→PowerOn→Sync→SimCheck→Register→PdpUp→Ready；L1/L2/L3 分级恢复与退避（§2.5 表，常量集中 `constexpr` 表）；Ready 每 30 s `CSQ`+`CEREG?` 巡检；APN 空则不发 `QICSGP`。
   - L1 连接级在 N1 只做框架与接口（连接槽 0–2，打开/关闭/收 `closed`/`recv` URC 的回调），NTRIP 客户端留给 N2。
2. `main/modules/net/NetModule`：UART1 驱动安装（启动期一次分配，RX 2048 B、事件队列 16）、GPIO26 控制、kNet 执行器（core 0、prio 8、roundBudget 20 ms、静态栈 4096 B、watchdog）、DataCenter 4G 快照（阶段、SIM 状态、IMEI、ICCID、CSQ、注册、PDP、恢复级别/计数/退避剩余）。
3. 设置键（ADR-0018 注册表，`Owner::kNet`）：`net.enable`（默认关）、`net.apn`、`net.user`、`net.pass`（只写，读回打码）；`Caller::kMqtt` 对 `net.*` locked。
4. 电源规则：WiFi 配置模式（`RadioMode::kWifiConfig`）4G 保持断电；电池 < 10% 不上电；PW1 `kQuiesce` 参与者：`QICLOSE` → `AT+QPOWD=1`（等 `POWERED DOWN` ≤ 1.5 s）→ PWREN 低，总 ≤ 2 s，AT 无响应直接断电；USB 在位关机、低电关机同样关 4G。
5. 控制台（ADR-0016）：`net status`（所有档位，ICCID 部分打码）；DEV 档 `net power on|off`、`net reset`、`net at <cmd>`（经 kNet 队列，拒绝 `ATE1`/`AT+IPR`/`AT+QISWTMD` 等）。
6. 更新相关文档：PROGRESS 不改（合并时由我改）；ADR-0015 中 GPIO26 “安全输出置低”描述若与新行为冲突，在报告中列出，由我改。

## 不做
- NTRIP、RTCM 仲裁、`ntrip.*` 键、手机 0x11/0x12 映射（N2）；MQTT/TLS/围栏/旧 JSON/ThingsBoard（N3）；OLED 4G 行（后续卡）。
- 不改冻结协议头。

## 约束
- 运行期无堆分配；DRAM ≤ 9 KB（含 UART 驱动），IRAM 0，Flash ≤ 25 KB（N1 部分）；WiFi 构建最小空闲堆不得低于现状（报告给出前后 `idf.py size` 与启动后 free/min）。
- kNet 不得忙等/`delay`，所有超时用 tick 比较推进；遵守 ADR-0018 单写者与线程模型。
- `net.enable` 默认关：未开启时行为与现状一致（GPIO26 低、UART1 不安装、kNet 可不创建或空转，报告说明选择）。

## 验收
- 门禁全绿；主机测试：AT 分帧（半行/粘包/回显/URC 插在响应中间）、超时与通道失效、`+CME ERROR`、`>` 数据阶段；状态机全路径、每级恢复触发与退避上限、SIM 拔出、注册超时 180 s、`pdpdeact`；关机参与者时序（正常/AT 无响应）；低电/WiFi 模式不上电。新增用例在基线上失败。
- 报告：资源前后、任务栈高水位估计、AT 命令超时表、所有对 ADR 的偏离。
- HIL 步骤（由我与用户在有串口的 P2 v1.41 板执行）：插 SIM `net.enable 1` 开机到 Ready，记录上电到 AT 可用、到 Ready 耗时；拔天线/`AT+CFUN=4` 恢复；拔 SIM 触发 L3；460800 稳定性（ADR-0021 Q2）；关机 ≤ 2 s；低电与发射峰值不掉压（Q3）；模块固件版本 `ATI`/`AT+QGMR`（Q5）。
