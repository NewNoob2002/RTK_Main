# B3：P2 充电芯片探测、USB 检测与电池电量

分支 `feat/b3-charger`，实施和体积对比基线为用户指定的 `main 84025fd`。实现范围为任务卡 §1 和 §3。实板 HIL 由 Architect 执行。

## 1. 开工前核实的硬件依据

| 项目 | 旧固件 | P2 v1.41 原理图与结论 |
| --- | --- | --- |
| GPIO27 `VBUS_DET_ESP32` | `../../P2_V3.1_IDF/main/user_include/user_gpio.h:21` 定义 GPIO27。`../../P2_V3.1_IDF/main/user_src/user_gpio.c:16` 调用 `gpio_init_1(..., GPIO_MODE_INPUT)`。`../../P2_V3.1_IDF/main/src/xyz_gpio.c:40-52` 明确关闭内上拉、下拉和中断。`../../P2_V3.1_IDF/main/main.c:119` 与 `../../P2_V3.1_IDF/main/user_src/user_battery.c:56-57` 将高电平解释为 USB 在位。 | `docs/hardware/P2/schematic/p2v1.41.pdf:11-20`，sheet 1 显示 VBUS–R164（100 kΩ）–分压点–R165（100 kΩ）–GND，分压点经 R179（0 Ω）接 `VBUS_DET_ESP32`。同文件 `:692`，sheet 7 接到 U11 焊盘 12。使用高有效、纯输入、内部上下拉关闭。 |
| GPIO34 `BAT_V_ADC` | `../../P2_V3.1_IDF/main/user_include/user_gpio.h:22` 定义 GPIO34。`../../P2_V3.1_IDF/main/src/xyz_gpio.c:150-158,175-188` 配置 ADC1_CH6、12 bit、旧名 11 dB。`../../P2_V3.1_IDF/main/user_src/user_battery.c:177-190` 的原始码换算包含 ×2 分压和旧经验校正项 1.12。 | `docs/hardware/P2/schematic/p2v1.41.pdf:332-355`，sheet 5 显示 VBAT–R217（100 kΩ）–`BAT_V_ADC`–R218（100 kΩ）–GND，C161（0.1 µF）从采样节点接地。同文件 `:682`，sheet 7 接到 U11 焊盘 6。校准后的引脚 mV 乘 2 得到电池端电压。 |
| I2C0 上拉与充电参数 | `../../P2_V3.1_IDF/main/src/xyz_iic.c:11-17` 开启内部上拉。`../../P2_V3.1_IDF/main/user_src/user_tools.c:163-200` 先探测 0x1A，应答后设 ICHG=0x34 和 REG03=0xAA。 | `docs/hardware/P2/schematic/p2v1.41.pdf:663-672`，sheet 7 的 R66/R68 是 4.7 kΩ 外部上拉。同文件 `:365`，sheet 5 标注 SGM41513。内部弱上拉并联，保持 100 kHz。 |

PDF 行号来自 `pdftotext -layout docs/hardware/P2/schematic/p2v1.41.pdf - | nl -ba`，连线和接地点另按对应页图形复核。

## 2. 改动文件与行为

| 文件 | 改动 |
| --- | --- |
| `main/board/BoardProfile.{hpp,cpp}` | P2 登记 GPIO27、GPIO34、两个充电寄存器目标和硬件版本 `V1.41`。E1 台架板沿用既有协议值 `V1.1.0`，未知板型报 `UNKNOWN`。版本为 0x01 原有 8 字节 ASCII 定长字段。 |
| `main/board/Charger.{hpp,cpp}`、`main/board/BoardStartup.{hpp,cpp}` | I2C0 先探测 0x1A。探测失败发一次 W 并禁用充电事务，保留同一总线上的手动扫描。应答后按旧 P2 对 REG02 读改写，保留高两位，低六位 ICHG 设为 0x34（1980 mA）。REG03 写全字节 0xAA（120/120 mA）。回读首个不一致的寄存器发一次带期望值和实读值的 W。启动 USB 判定依次取 GPIO27、在线芯片 REG08、STAT。 |
| `main/board/BatteryAdc.{hpp,cpp}`、`main/CMakeLists.txt` | P2 启动期初始化一个 ADC1 oneshot 单元和一个 ESP32 line fitting 校准句柄。优先使用 eFuse，缺少时以标称 1100 mV 校准。由 SDK 校准取代旧经验校正项 1.12。ADC 初始化失败释放已获得的资源，功能降级。 |
| `main/board/PowerLogic.{hpp,cpp}`、`main/modules/power/PowerModule.{hpp,cpp}`、`main/def/SystemDef.hpp` | GPIO27 可用时直接取样，REG08 仅在回退时读取。保留 B2 的读锁争用重试和切电前刷新。ADC 电压和电量发布到 `data::DataCenter::powerStatus`。启动输出一行汇总，运行期 I 级 USB 日志由在位或来源变化触发。 |
| `main/modules/power/PowerCmds.cpp` | DEV 新增 `power i2c scan`，扫描 0x08–0x77，每地址探测超时上限 10 ms，上报有 ACK 的地址。扫描和充电驱动使用同一总线和静态互斥，逐地址持锁。完整扫描末行 `OK`，总线故障返回错误。`power status` 带 `usb_src`、`bat_mv`、`bat_pct`。 |
| `main/modules/bt_link/BtLinkModule.cpp` | 在应用装配层用当前板型填充 0x01 硬件版本。0x03 由电源状态槽更新 `batteryPct`、`onBattery`。保留 link_core 通用构造器的默认契约。 |
| `tools/main_host_check/BoardProfileCheck.cpp`、`PowerLogicCheck.cpp`、`MainHostCheck.cpp`、`WebModuleCheck.cpp`、`CMakeLists.txt` | 覆盖电量表节点、边界和单调性，低电计时与 USB 取消、回绕，USB 来源组合，真实电源模块的 B2 仲裁路径，蓝牙 0x01/0x03 和原有 B2 回归。 |
| `tools/main_host_check/ChargerCheck.cpp`、`shim/driver/i2c_master.h`、`shim/freertos/semphr.h` | 用替身运行真实充电驱动。核对唯一一次启动探测、离线寄存器零事务、逐地址持锁与总线复用、扫描范围及错误传播、REG02 高位保留及回读掩码、REG03 全字节回读和不一致日志。 |

旧 P2 每 1 s 开一组，每组 20 次，组内间隔 5 ms（`../../P2_V3.1_IDF/main/main.c:60-72,201-205`，`../../P2_V3.1_IDF/main/user_src/user_battery.c:177-190`）。B3 首组间隔 5 ms，随后每组完成后至少等待 1000 ms，再由 10 ms 的 kLink 电源轮询每轮采一个样本。正常运行约每 1.2 s 发布一组结果。取 20 个校准样本的中值可抑制负载尖峰，运行期采样不需要在 kLink 等待。电压使用旧 3340–4150 mV 分段表插值（`../../P2_V3.1_IDF/main/user_src/user_battery.c:135-146`），向下取整到整数百分比。ADC 样本失效时控制台显示 `n/a`，蓝牙值为协议的 `0xFF`。

只有可靠确认 USB 拔出且有效电量连续 30 s 低于 10% 时，才提交 B2 关机请求并记录一行带电压的 W。插入 USB、恢复到 10% 或样本失效时清除计时。STAT 高可能意味着充满，单独出现时不足以触发低电保护。由 B2 原有的 `PowerRequests`、保存、静默及切电阶段处理。K1 按键回归、S1 设置准入和保存栅栏、X2 关机与重启竞争均通过现有测试。OTA 上传从同一个仲裁标志感知关机，沿已有路径中止并释放上传会话（`main/modules/web/WebOta.cpp:200-203,480-487`）。

## 3. 资源对比

默认 ESP32 固件。将 `git archive 84025fd` 解包到本工作树的 `builds/b3-baseline-src/` 后以同一 ESP-IDF v6.1 和默认配置构建。体积取自 `stat -c %s`、`idf.py size`、`python -m esp_idf_size --diff`，另执行 `cpp_cost.py diff` 交叉检查 ELF。对应日志在 `builds/b3-baseline.size.log`、`builds/b3-idf.size.log`、`builds/b3-size.diff.log` 和 `builds/b3-cpp-cost.diff.log`。

| 指标 | 基线 `84025fd` | B3 | 差值 |
| --- | ---: | ---: | ---: |
| `rtk_main.bin` | 1,516,736 B | 1,520,688 B | +3,952 B |
| DRAM | 96,483 B | 96,619 B | +136 B |
| IRAM | 116,139 B | 116,139 B | 0 B |

## 4. 门禁和实测状态

使用指定 IDF 6.1 与 Python 3.13 环境，`IDF_COMPONENT_MANAGER=0`，缓存的 lizard 1.26.1。独立 ASan 小程序复现沙箱内 LeakSanitizer 自检失败。正式门禁设置 `ASAN_OPTIONS=detect_leaks=0`，保留 AddressSanitizer 和 UBSan。

`bash tools/ci/run_gates.sh --prefix b3 --base 84025fd` 退出码 0，**43 PASS / 0 FAIL**。逐项日志位于 `builds/b3-logs/`。

| 检查 | 结果 |
| --- | --- |
| G1 frozen `sha256sum -c --quiet docs/frozen.sha256` | PASS |
| G2 / G2b host | PASS / PASS |
| G2 / G2b app_fw | PASS / PASS |
| G2 / G2b services | PASS / PASS |
| G2 / G2b stream_demux | PASS / PASS |
| G2 / G2b bt_reconnect | PASS / PASS |
| G2 / G2b cli | PASS / PASS |
| G2 / G2b gnss | PASS / PASS |
| G2 / G2b nvs_store | PASS / PASS |
| G2 / G2b web_proto | PASS / PASS |
| G2 / G2b settings | PASS / PASS |
| G2 / G2b ota | PASS / PASS |
| G2 / G2b main_host_check | PASS / PASS，每套 122/122 |
| G2 / G2b t13_host_check | PASS / PASS |
| G2 / G2b gnss-all | PASS / PASS |
| G3 默认固件 | PASS，编译零告警 |
| G3 trace-off | PASS，编译零告警 |
| G3 wifi-off | PASS，编译零告警 |
| G3 ota-off | PASS，编译零告警 |
| G3 PRODUCTION | PASS，编译零告警 |
| G3 NONE | PASS，编译零告警 |
| G3 NONE + Wi-Fi | PASS，编译零告警 |
| G3b rtk_pure ESP32 | PASS，编译零告警 |
| G3b rtk_pure ESP32-C3 | PASS，编译零告警 |
| G5 clang-format | PASS |
| G5b clang-tidy | PASS |
| G6 `python3 -I tools/doc_check/doc_check.py --base 84025fd` | PASS |
| G7b 运行期符号及堆白名单 | PASS |
| G7c 无控制台固件 CLI 依赖 | PASS |
| `git diff --check`、体积对比 | PASS |
| 实板 HIL | NOT_RUN，待 Architect 执行 |

ESP-IDF 全新配置时报告了已有的跨组件私有 include 目录 CMake 提示。基线和 B3 的干净默认构建均有同样 7 条提示，G3 的编译器告警检查通过。

## 5. 待 Architect 同步的冻结文档

- `docs/adr/ADR-0015-board-profiles.md` §2.2 补充硬件版本和电池 ADC 配置字段。§2.4 记录 USB 在位关机后重启到充电待机及低电关机复用仲裁。§3 更新 GPIO27 高有效无内部上下拉、GPIO34 校准及 ×2、GPIO2 STAT 的回退位置和 I2C0 探测及参数。GPIO39 保留既有停用状态。
- `docs/protocol/phone_protocol.md` §4 更新 0x01 硬件版本及 0x03 电量和供电来源的实现描述，字段布局沿用既有协议。
- `docs/tasks/B3-charger-usb-battery.md` 所列 `1b01eef` 与用户指定实施基线 `84025fd` 不同，验收记录应使用后者。将 §1.2 和 §4.2 的 REG02 预期值改为低六位等于 0x34、高两位保留。

## 6. 未决问题及 HIL 步骤

新 P2 v1.41 + F9P 的实际充电芯片型号和地址、GPIO27 插拔实测、充满待机路径及 ADC 绝对精度仍待实板核实。

1. 在新 P2 v1.41 + F9P 执行 `power i2c scan`，记录应答地址。预期 0x1A、0x3C。若未见 0x1A，记录实际地址，停在此步向用户报告。
2. 核对开机日志没有 I2C 错误刷屏。芯片在线时执行 `power charger`，应满足 `(REG02 & 0x3F) == 0x34`，高两位保持写入前的值。若写入前高两位为 0x80，应回读 REG02=0xB4。REG03 应为 0xAA。
3. 插拔 USB 并执行 `power status`，核对 `usb` 随插拔改变且 `usb_src=gpio`。充满时带 USB 关机应重启到充电待机。
4. 用万用表核对 `bat_mv`，误差应不超过 50 mV。确认手机 App 显示电量。
5. 用可调电源模拟 <10% 电量。无 USB 持续至少 30 s 后应正常关机，计时中插入 USB 应取消低电关机。

## 7. 评审修复

| 发现 | 修复位置与行为 | 覆盖检查 |
| --- | --- | --- |
| B3-R1 | `main/board/Charger.cpp:259` 按旧 P2 执行 REG02 读改写，回读仅校验低六位，REG03 校验全字节。首个不一致在 `Charger.cpp:291` 记录寄存器、期望和实读字节，`main/board/BoardStartup.cpp:362` 避免重复记录。 | `tools/main_host_check/ChargerCheck.cpp:98` 的 `charger_online`、`charger_readback02_high_bits`、`charger_mismatch02`、`charger_mismatch03`，同时核对 W 日志次数与内容。 |
| B3-R2 | `main/board/Charger.cpp:312` 每地址获取并释放一次互斥锁，扫描中断时保留已应答前缀。 | `tools/main_host_check/ChargerCheck.cpp:72` 的 `charger_absent` 核对每次探测对应一次释放，`ChargerCheck.cpp:123` 的 `charger_scan_errors` 覆盖争用与超时。 |
| B3-R3 | `main/modules/power/PowerModule.cpp:303` 在每次成功采样后更新时间戳，`PowerModule.cpp:325` 以独立的组结束时间维持 1 s 组间隔。保留 2 s 新鲜度窗口，因为采样持续推进时即使一组耗时超过 2 s 也可继续计时，实际采样中断达到 2 s 仍会清零低电计时。 | `tools/main_host_check/MainHostCheck.cpp:704` 的 `power_battery_freshness`，覆盖 250 ms 慢轮询和 2 s 采样间断。 |
| B3-R4 | `main/board/Charger.cpp:164` 区分无 ACK 与总线故障，后者记录 SDK 错误名并返回映射状态。设备保持离线。 | `tools/main_host_check/ChargerCheck.cpp:72` 的 `charger_absent` 与 `ChargerCheck.cpp:112` 的 `charger_init_timeout`，覆盖两种日志和无寄存器访问。 |
| B3-R5 | `tools/main_host_check/MainHostCheck.cpp:631`、`MainHostCheck.cpp:664`、`MainHostCheck.cpp:685`、`tools/main_host_check/ChargerCheck.cpp:123` 增加对应场景。 | `power_battery_read_failure` 校验清除低电计时、蓝牙 0x03 上报 0xFF 及恢复后重新计时。`power_low_battery_unknown_usb`、`power_low_battery_standby` 校验不关机。`power_battery_median` 校验 20 点中值抑制单点尖峰。`charger_scan_errors` 校验互斥争用与超时提前退出。 |
| B3-R6 | `main/board/BoardStartup.cpp:299`、`BoardStartup.cpp:320` 分离输入配置和读电平，`BoardStartup.cpp:530` 在轮询前各配置一次。 | `tools/main_host_check/PowerLogicCheck.cpp:351` 的 `power_logic_check` 验证冷启动等待与来源优先级。本地专项 `builds/b3-review-boot/BootPollCheck.cpp` 用 SDK 替身运行真实 `startupPower()`，GPIO27 正常及配置失败回退 STAT 两种路径均只配置每脚一次，选用的引脚各读四次。 |
| B3-R7 | `main/board/PowerLogic.hpp:41`、`PowerLogic.hpp:206`、`main/board/Charger.hpp:19`、`main/modules/power/PowerModule.hpp:2` 同步关机来源、USB 取样顺序、重复初始化返回值和 I2C 等待上限。 | `doc_check --base 84025fd` 及契约核对，重复初始化行为由 `charger_absent`、`charger_init_timeout` 覆盖。 |

- 最终执行 `ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix b3 --base 84025fd`，43 PASS / 0 FAIL。普通和 ASan/UBSan 的 `main_host_check` 均为 122/122，`doc_check` 检查 31 个函数、0 个不合格。汇总见 `builds/b3-review-gates.log`，逐项日志见 `builds/b3-logs/`。
- 首轮全量验证 41 PASS / 2 FAIL。G5b 的探测函数复杂度和局部常量命名问题已修正。既有 `t13_gap_pairing_window` 在首轮 ASan/UBSan 中失败，随后单项复跑通过，第二轮全量 ASan/UBSan 通过 45/45。首轮与单项复跑日志见 `builds/b3-review-gates-first.log`、`builds/b3-review-tidy-first.log`、`builds/b3-review-t13-asan-first.log` 和 `builds/b3-review-t13-asan-retry.log`。
- 实板 HIL 为 NOT_RUN，验收步骤见 §6。
