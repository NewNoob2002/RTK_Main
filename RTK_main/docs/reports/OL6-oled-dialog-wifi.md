# OL6 OLED 确认弹窗与 COMM Wi-Fi 信息

基线为 `113f186`，工作分支为 `feat/ol2-oled-ui`。此基线的 OL5 实板验收由用户确认。10 s bench 达到 30.0 fps，CPU 3.8%，渲染最大 1.48 ms，帧间隔最大 35 ms，I2C 错误为 0，OLED 栈余量 784 B。这些值属于 OL5。

## 确认弹窗

| 弹窗 | 默认项与确认结果 | 另一项与确认结果 | 到期行为 |
| --- | --- | --- | --- |
| `POWER OFF?` | Cancel，关闭 | OK，提交按键关机 | 5 s 自动关闭 |
| `SWITCH TO WIFI?` | Cancel，关闭 | OK，提交 Wi-Fi 切换 | 等待输入 |
| `PAIR REQUEST` | Reject，提交拒绝 | Accept，提交接受 | 30 s，由 SPP worker 发送拒绝 |

单击只循环移动焦点。双击确认逻辑选中项，即使弹簧仍在运动也使用目标选项。每次打开弹窗都回到安全项。关机确认优先于配对请求，取消后立即显示仍在有效期内的配对弹窗。到期和弹窗上下文切换会清理旧输入，防止待消歧的按键被当成新弹窗的确认。

按钮共用 OL4 定点弹簧和挤压反馈，运动由 OL5 的绝对帧节拍驱动。已绘制的标题、边框和配对信息保存在原帧缓冲中。焦点移动只重绘第 6、7 页的按钮区域，关机计时另更新第 4 页。配对设备名或数字校验信息变化时重绘提示，长按浮层退出时恢复底图。长按进入关机确认和 8 s 强制关机继续由原电源路径处理。配对的手动拒绝和超时拒绝沿用 SPP 路径。

## COMM 页

Web 启动 SoftAP 后，将 SSID 与 IPv4 一起写入现有 DataCenter 主题槽。SSID 是 `startSoftApWifi` 使用的 `SystemStatus::deviceName`，IPv4 来自设置 netif 地址的 `kApIpv4`。OLED 仅在 Wi-Fi 模式且 SoftAP 就绪时消费该快照，沿用 DataCenter 共享锁。渲染层将 IPv4 格式化为点分十进制，地址使用全宽信息行。SSID 按已有字符清理和行宽规则截断。进入 Wi-Fi COMM 即可看见 SSID、`192.168.10.12` 和客户端数，Back 可向下选中。BT COMM 保留既有画面和动作。

主机回归覆盖单击循环、Cancel/Reject、OK/Accept、超时安全退出、弹窗优先级、按钮区域脏页、配对设备名晚到、长按强制关机，以及 Web 配置地址与 DataCenter、OLED 视图之间的数据传递。模拟器更新了三种弹窗的默认和确认焦点截图，以及 Wi-Fi COMM 截图。 [模拟器截图](../../builds/ol14-oled-screens.png)

## 资源

比较 OL5 的 `builds/ol13-idf` 与 OL6 的 `builds/ol14-idf`。两者使用 ESP-IDF v6.1、ESP32 和同一 `sdkconfig`。

| 项目 | OL5 | OL6 | 增量 |
| --- | ---: | ---: | ---: |
| 固件 bin | 1,580,736 B | 1,582,304 B | +1,568 B |
| Flash code | 1,136,972 B | 1,138,556 B | +1,584 B |
| Flash data | 274,572 B | 274,556 B | −16 B |
| DRAM | 98,419 B | 98,475 B | **+56 B** |
| IRAM | 116,139 B | 116,139 B | **0 B** |

DRAM 增量全部位于 .bss。现有 3072 B OLED 任务栈和单份 1024 B 帧缓冲保持原预算。

## 验证记录

| 命令或检查 | 结果 |
| --- | --- |
| `tools/ci/run_gates.sh --prefix ol14 --base 113f186` | **PASS**，43/43。包括冻结校验、普通和 ASan/UBSan 主机套件、ESP-IDF 构建、格式、Doxygen、clang-tidy、堆申请和 CLI 依赖门禁。 |
| `ctest --test-dir builds/ol14-main_host_check --output-on-failure` 与对应 ASan/UBSan 构建 | **PASS**，各 183/183。 |
| `ctest --test-dir builds/ol14-main_host_check -R '^oled_' --output-on-failure` | **PASS**，26/26，包含 3 个模拟器 golden 场景。BT COMM 截图与 OL5 逐像素相同。 |
| `builds/ol14-main_host_check/oled_module_check bench_and_event_wait` | **PASS**，在虚拟 400 kHz 时钟下 1 s 写出 30 帧，最大间隔 34 ms。 |
| `idf.py -B builds/ol14-idf size` 与两份构建的 `config/sdkconfig.json` 对比 | **PASS**，配置一致，DRAM +56 B，IRAM +0 B。 |
| `git diff --check` 与 `sha256sum -c --quiet docs/frozen.sha256` | **PASS**。 |
| OL6 实板弹窗、SoftAP 显示和 OLED 栈余量 | **NOT_RUN**，本轮仅执行本地构建、主机测试和模拟器。 |

首轮门禁在 IDF 构建阶段收到 SIGTERM。三个尚未运行的配置单独完成构建后，以原命令重跑全量门禁。最终日志位于 `builds/ol14-gates-final.log` 和 `builds/ol14-logs/`。主机 sanitizer 在允许 ptrace 的环境运行，保留了泄漏检查。

实现涉及 OLED 视图状态、绘图和采集，Web SoftAP 及 DataCenter 快照，主机测试和 SDL 模拟器脚本及 golden。文档变动限于本报告与 ADR-0020 §5.2 的确认弹窗交互。
