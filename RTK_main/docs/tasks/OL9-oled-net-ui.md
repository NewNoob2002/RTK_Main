# OL9 OLED 网络信息与预留界面

- 分支 / 工作树：`feat/ol9-net-ui`，`../RTK_Main-ol9`，基于 N1 合并后的 main（N1 提供 `netStatus` 槽，main/data/AppData.hpp）
- 报告：`docs/reports/OL9-oled-net-ui.md`；完成后最后一行单独输出 `OL9-FINISHED`
- 门禁：`export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env && tools/ci/run_gates.sh --prefix ol9 --base <N1 合并提交>`
- 用户决定（2026-11-27）：信号图标方案同意；网络信息单独开 NET 入口；预留项只做 1、2、3、6。

## 范围

### A 状态栏 4G 图标（x84–102 空档，约 19 px）
| 状态 | 显示 |
|---|---|
| 无 kCellular 或 net.enable=0 | 不显示，空位保持 |
| 上电 / 同步 / 搜网 | 空心格轮流点亮动画 |
| 无 SIM / SIM 错误 | 空心格 + 小 `x` |
| 已注册未拨号 | 按信号显示格数，无 `4G` |
| PDP 已建立 | 格数 + `4G` |
| NTRIP / MQTT 已连（N2/N3 后） | `4G` 旁小上下箭头（本卡只留接口，数据恒为未连） |

CSQ→格数：<10→1，10–14→2，15–19→3，≥20→4，99/未知→0。升格须连续保持 10 s，降格立即（防闪烁）；纯函数 + 主机测试。

### B NET 入口（仅 kCellular 板型出现；其它板 Dock 与现在完全一致）
NET 一级页 3 行摘要，选中按键进入详情：
- `4G    <phase> CSQxx` → 4G 详情：状态、信号、注册、PDP、运营商（无数据则 `--`）、IMEI、ICCID（中段掩码，与 net status 一致）、恢复级别/次数
- `NTRIP <state>` → NTRIP 详情：服务器:端口、挂载点、连接状态、差分接收量、龄期、GGA 上传间隔、当前差分来源 auto/phone/ntrip（N2 前显示占位 `--`）
- `MQTT  --` → MQTT 详情：连接状态、服务器、设备 ID、上/下行最近时间、围栏同步版本（N3 前全部 `--`）
长文本沿用 OL8 跑马灯；详情行数超屏沿用现有滚动。

### C 预留项
1. 全屏告警层：统一优先级（围栏进入/离开、精度丢失、低电、网络告警），显示围栏名与告警类型，按键可确认关闭；优先级低于关机页与 OTA 页。本卡只做告警层框架、优先级仲裁与一个主机可注入的测试源，不接围栏/蜂鸣器（WP-FENCE / WP-ALARM 再接）。
2. 定位页：增加差分龄期与来源（phone/ntrip）；来源 N2 前恒为 phone 或 `--`。
3. 设备页：显示上次关机原因 / 最近故障（复用 pwr.lastShutdown；含超时参与者名，如 RF TIMEOUT 对应 radio_pause），无记录显示 `--`。
6. OTA 页数据源抽象：现有 OTA 页改为从统一 OTA 进度源读取，Web OTA 为当前唯一来源，为 4G OTA 预留接入点；外观不变。

## 不做
- NTRIP/MQTT 实际连接（N2/N3）；蜂鸣器/LED/马达；亮度调节；注册状态显示。
- 不改冻结头；如确需，报告说明，由 Architect 重算 `docs/frozen.sha256`。

## 约束
- DRAM 增量 ≤ 256 B，IRAM 0，不新增堆分配；OLED 任务栈余量不得低于当前（656 B）减 128 B，否则报告说明。
- 渲染只读 ViewModel；网络数据经 DataCenter 快照进入 ViewModel，OLED 不直接访问 NetModule。
- 主机测试覆盖：CSQ 映射与迟滞、各状态图标选择、无 kCellular 时 Dock 不变、告警层优先级（关机/OTA 压过告警）、ICCID 掩码。
- HIL（报告列步骤，由用户执行）：图标随拔天线变化、NET 三级页浏览、无 4G 板 Dock 不变、告警测试源显示与关闭。
