# B3 独立评审报告

- 评审对象：`/home/gtc/Desktop/workspace/ESP32_PROJ/RTK_Main-b3/RTK_main`（分支 `feat/b3-charger`，基线 `84025fd`，工作树未提交改动 + 未跟踪文件）
- 依据：任务卡 `docs/tasks/B3-charger-usb-battery.md`、执行报告 `docs/reports/B3-charger-usb-battery.md`、`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0015-board-profiles.md`、`docs/protocol/phone_protocol.md`、B2 任务卡/报告（K1/S1/X2）、旧固件 `P2_V3.1_IDF`（只读）与本地 ESP-IDF v6.1 源码
- 评审方式：只读代码审阅 + 独立运行主机普通/ASan+UBSan 测试（`builds/rb3-host*`）、`clang-format` 与冻结校验；未修改任何源码、未提交、未烧录

## 1. 核对摘要

| 维度 | 结论 | 说明 |
| --- | --- | --- |
| 任务卡 §1.1–§1.6 覆盖 | 基本符合（见发现） | `power i2c scan`、0x1A 探测、GPIO27 高有效检测、ADC1+line fitting 电池采样/电量表/低电关机、硬件版本与日志均已落地；REG02 写全字节与扫描持锁粒度存在问题（见 B3-R1、B3-R2）。 |
| ESP-IDF API 使用 | 基本正确（见 B3-R4、B3-R6） | `adc_oneshot` + `adc_cali_create_scheme_line_fitting` 生命周期与错误回滚无泄漏，与启动期 `BootAdc`（`stepIdentify` 释放 ADC1 后 `PowerModule::init` 接管）无冲突；GPIO27 纯输入关闭内部上下拉，与原理图 R164/R165（100 kΩ/100 kΩ）及旧固件一致。 |
| 并发与阻塞 | 存在缺陷（见 B3-R2） | `kLink` 轮询调用 `refreshInputStatus()` 采用非阻塞锁（`ticks=0`），正常不会卡死 `kLink`；但 `charger::scan()` 整轮 112 地址持锁会饿死 I2C 回退路径。 |
| 低电关机防误触发 | 大体稳健（见 B3-R2、B3-R3） | 启动 ADC 失败、运行期样本失效、`usbKnown==false`（含回退 STAT 高）、充电待机均不会误触发；B2 仲裁 `PowerRequests`（K1/S1/X2）与 `WebOta` 中止路径无回退。但 `kBatteryFreshMs` 窗口偏紧（B3-R3）。 |
| 蓝牙 0x01/0x03/0x40 编码 | 符合协议 | `0x01` 偏移 24（8 B 定长 ASCII）按板型填 `V1.41`/`V1.1.0`/`UNKNOWN`；`0x03` `[0]=!usbPresent`、`[2]=batteryPct`（失效为 `0xFF`），并经共享 `DeviceInfo` 同步至报警器 `0x40`，与 `docs/protocol/phone_protocol.md` §4/§5 一致。 |
| 资源与门禁 | 合格 | 实测 `builds/rb3-host` 与 `builds/rb3-host-asan` 均 109/109 PASS；`clang-format` 与 `docs/frozen.sha256` PASS；增量 Flash +3,712 B、DRAM +120 B、IRAM 0 B；`cmdPowerI2cScan` 栈帧 112 B 在控制台栈安全范围内。 |

## 2. 发现

### B3-R1
- **Severity**: major
- **Location**: `main/board/Charger.cpp:244-262`, `main/board/BoardProfile.cpp:45`, `main/board/BoardStartup.cpp:356-357`
- **Failure Scenario**:
  1. B3 删除了原有的 `maskSgmReg02`，改为直接向 SGM41513 `REG02` 写入整字节 `0x34`（`writeRegLocked(kSgmReg02, 0x34)`）。而旧固件 `P2_V3.1_IDF/main/user_src/user_tools.c:165-177` 明确注释并实现为读-改-写，保留高两位 `[7:6]`（`BOOST_LIM`、`Q1_FULLON`）：`newVal = (currentVal & 0xC0) | (0x34 & 0x3F)`。SGM41513/BQ25601 上电复位时 `REG02[7]`（`BOOST_LIM`）默认为 `1`（1.2 A），整字节写 `0x34` 会将其静默清零为 `0`（0.5 A）并清除 `Q1_FULLON`，与任务卡 §1.2“与旧 P2 V3.1 一致”存在行为偏离（尽管 §1.2/§4.2 字面写 `REG02=0x34` 导致歧义）。若实板芯片或替代料 `REG02[7:6]` 存在只读/固定非零位，整字节回读比对 `actual == expected[i]` 还会触发误报 `kCorrupt`。
  2. 回读不一致时，`BoardStartup.cpp:357` 仅输出 `"charger: charge parameter readback mismatch"`，未打印具体寄存器号、期望值与实读值，不利于 HIL 快速定位是 REG02 高位差异还是芯片型号/写保护问题。
- **Suggested Fix**:
  - 与 Architect/用户确认 `REG02[7:6]` 策略：若需严格对齐旧 P2 V3.1，恢复读-改-写掩码（保留 `[7:6]`、写低 6 位 `0x34`），回读校验目标合并值（或低 6 位 `0x34`），并同步更新任务卡 §4.2 期望值（若 POR `BOOST_LIM=1` 则回读 `0xB4`）；若确实有意清零 `[7:6]`，在报告未决/偏离节显式记录 `BOOST_LIM` 变更。
  - 在 `applyChargeParams()` 检测到回读不一致时，记录具体寄存器地址、期望字节与实际读回字节（保持单行 W 日志）。

### B3-R2
- **Severity**: major
- **Location**: `main/board/Charger.cpp:275-294`, `main/modules/power/PowerModule.cpp:247-272`, `main/modules/power/PowerModule.cpp:338-346`
- **Failure Scenario**:
  - `charger::scan()` 在进入 `0x08..0x77`（112 个地址）循环前一次性获取 `s_charger.mutex`，直到全部 112 次 `i2c_master_probe(..., 10 ms)` 结束才释放（典型耗时 20–60 ms，总线异常或从机拉低时钟时最坏可达 `112 × 10 ms = 1.12 s`）。相比之下，`dumpRegs()`（`Charger.cpp:264-273`）明确按单寄存器分别持锁以避免饿死 `kLink`。
  - 当 GPIO27 不可用而走 I2C `REG08` 回退路径时，`PowerModule::samplePins()` 调用 `refreshInputStatus()` 使用非阻塞锁（`BusGuard(s_charger.mutex, 0U)`）。若此时控制台执行 `power i2c scan`：
    1. `refreshInputStatus()` 持续返回 `kBusy`，置 `chargerRetry_ = true`，导致 `PowerModule.cpp:260` 的 `fresh` 为 `false`、`pins.usbKnown` 为 `false`；
    2. `pollBattery()` 中 `onBattery = pins.usbKnown && !pins.usbPresent` 变为 `false`，立即清零已累积的 30 s 低电计时器；
    3. 若恰逢关机进入 `kCutPower`（`PowerModule.cpp:622`）或断电阶段 U3 跌落（`PowerModule.cpp:679-682`），`samplePins(nowMs, true)` 获取 `kBusy`，导致 `ShutdownSm` 收到 `std::nullopt`（U3 已低时停滞）或使用陈旧的 `usbPresent`（U3 仍高时）。
- **Suggested Fix**:
  - 将 `charger::scan()` 的 `BusGuard` 移入 `for` 循环内部，按单次 `i2c_master_probe` 粒度持锁释放（与 `dumpRegs()` 一致）。由于 `BusGuard` 默认等待 20 ms（大于 `refreshInputStatus()` 最长 10 ms 传输），既不会让 `scan()` 误报 `kBusy`，也允许 `kLink` 在地址间隙完成 REG08 采样。

### B3-R3
- **Severity**: minor
- **Location**: `main/modules/power/PowerModule.cpp:40-42`, `main/modules/power/PowerModule.cpp:328-345`
- **Failure Scenario**:
  - `lastBatterySampleMs_` 仅在整组 20 个样本收齐时更新（`batterySampleCount_ == 0U`）。每组先等待 `kBatteryPeriodMs = 1000 ms`，再在 20 次 `PowerModule::poll()` 中逐轮采样。标称 10 ms 轮询下整组周期约 1190 ms，距 `kBatteryFreshMs = 2000 ms` 仅剩约 800 ms 裕量（平均每轮仅允许 ~40 ms 额外延迟）。
  - 若 `kLink` 在 Wi-Fi/OTA 活动、蓝牙拥塞或 I2C 回退超时下平均每轮超过 ~52 ms，则在收齐第 20 个样本前 `nowMs - lastBatterySampleMs_` 就会达到 2000 ms，导致 `fresh` 变为 `false` 并反复清零 `LowBatteryTimer`，使低电关机在持续高负载下永远无法触发。
- **Suggested Fix**:
  - 放宽 `kBatteryFreshMs`（例如 5000 ms，仍远小于 30 s 低电保持窗），或单独记录最近一次成功采样的时间戳，避免正在正常推进的 20 样本采集把 `fresh` 判为过期。

### B3-R4
- **Severity**: minor
- **Location**: `main/board/Charger.cpp:177-184`
- **Failure Scenario**:
  - 按本地 ESP-IDF v6.1 `driver/i2c_master.h:248-258` 与 `i2c_master.c:1409-1414`，`i2c_master_probe()` 对地址 NACK 返回 `ESP_ERR_NOT_FOUND`，对总线拉死/缺少上拉/硬件超时返回 `ESP_ERR_TIMEOUT`。
  - 当前代码对所有 `err != ESP_OK` 一律打 `"charger: no ack at 0x%02X, i2c path disabled"` 并返回 `kNotFound`。当板上 SDA/SCL 异常拉低或硬件故障时，启动日志会将总线超时误报为“无应答”。
- **Suggested Fix**:
  - 仅在 `err == ESP_ERR_NOT_FOUND` 时输出 `"charger: no ack at 0x%02X, i2c path disabled"` 并返回 `kNotFound`；其余错误（如 `ESP_ERR_TIMEOUT`）输出包含 `esp_err_to_name(err)` 的单行 W 并返回 `rtos::fromEspErr(err)`，同时同样保持 `s_ready = false` 跳过后续充电 I2C 事务。

### B3-R5
- **Severity**: minor
- **Location**: `tools/main_host_check/MainHostCheck.cpp:569-614`, `tools/main_host_check/ChargerCheck.cpp:52-88,217-221`
- **Failure Scenario**:
  - `MainHostCheck.cpp::powerLowBattery` 仅覆盖 GPIO27 正常且恒定 3390 mV 的路径，未在真实 `PowerModule` 上回归验证：
    1. 运行期 ADC 采样失败清除 `batteryPct` 为 `0xFF`、取消进行中的低电倒计时并在蓝牙 `0x03` 上报 `0xFF`；
    2. `usbKnown == false`（GPIO27 配置失败 + 充电芯片离线 + STAT 高）不误触发低电关机；
    3. `BootMode::kChargeStandby` 下电量 < 10% 不触发低电关机；
    4. `PowerModule::takeBatterySample()` 20 点中值滤波对单点尖峰异常值的抑制。
  - `ChargerCheck.cpp` 未测试 `scan()` 遇到 `ESP_ERR_TIMEOUT` 提前中止并返回映射错误码（其替身 `fromEspErr` 在 219 行直接断言 `err == ESP_OK`），也未测试持锁竞争（`kBusy`）场景。
- **Suggested Fix**:
  - 在 `MainHostCheck.cpp` 补充上述 4 项运行期低电/滤波防误触发用例，在 `ChargerCheck.cpp` 补充 `scan()` 超时错误传播与互斥竞争用例。

### B3-R6
- **Severity**: nit
- **Location**: `main/board/BoardStartup.cpp:300-316`, `main/board/BoardStartup.cpp:370-380`, `main/board/BoardStartup.cpp:520-528`
- **Failure Scenario**:
  - `readBootUsb()` 每次调用都经 `readInputPin()` 重新调用 `gpio_config(&config)`。当 `!senseHigh && !firstUsb.present` 进入 `waitForBootUsb()`（最长 350 ms、每 20 ms 轮询一次，最多 18 轮）时，会重复配置 GPIO27（以及回退时的 GPIO2）十余次（IDF v6.1 `gpio.c:447` 为 `ESP_LOGD`，默认不刷屏，但属无谓重复配置）。
- **Suggested Fix**:
  - 在 `waitForBootUsb()` 前一次性配置 GPIO27/GPIO2，轮询回调内仅执行 `gpio_get_level()`。

### B3-R7
- **Severity**: nit
- **Location**: `main/board/PowerLogic.hpp:41,206`, `main/board/Charger.hpp:19`, `main/modules/power/PowerModule.hpp:2`
- **Failure Scenario**:
  - 四处文档注释与 B3 实际行为不一致：
    - `PowerLogic.hpp:41` 仍写 `预留 kLowBattery 与 kPhone`（`kLowBattery` 已启用）；
    - `PowerLogic.hpp:206` 仍写 `SGM41513 REG08 优先，失败退化到 STAT`（现为 GPIO27 优先）；
    - `Charger.hpp:19` 写 `@retval kOk 已初始化或重复调用`（首次探测无应答后的重复调用实际返回 `kNotFound`）；
    - `PowerModule.hpp:2` 文件头仍写 `poll 仅 kLink 任务调用（非阻塞...）`，与同文件 65 行 `I2C 单次传输最多等待 10 ms` 矛盾。
- **Suggested Fix**:
  - 同步更新上述四处注释，保持契约文档与实现一致。

## 3. 本轮独立复核命令与结果

| 检查项 | 结果 |
| --- | --- |
| `sha256sum -c docs/frozen.sha256` | PASS |
| `clang-format --style=file --dry-run --Werror <改动及新增 C/C++ 文件>` | PASS |
| `cmake -S tools/main_host_check -B builds/rb3-host -G Ninja && cmake --build builds/rb3-host && ctest --test-dir builds/rb3-host --output-on-failure` | PASS（109/109） |
| `cmake -S tools/main_host_check -B builds/rb3-host-asan -G Ninja`（ASan+UBSan，`-fno-sanitize=vptr`，`ASAN_OPTIONS=detect_leaks=0`）`&& ctest` | PASS（109/109） |
| 复核 `builds/b3-logs/`（G6 `doc_check` 29 函数 0 不合格、G7b 堆白名单、size/cpp-cost 差值） | PASS（构建产物时间戳晚于全部源码修改） |
| 实板 HIL | NOT_RUN（不在本评审范围） |

## 4. 评审结论

**ACCEPT-WITH-FIXES**

无阻塞级缺陷，核心架构、ESP-IDF ADC/GPIO 用法、蓝牙协议编码及 B2（K1/S1/X2）保护均符合要求；合并前请优先处理 **B3-R1**（明确并统一 `REG02[7:6]` 掩码策略与回读不一致明细日志）与 **B3-R2**（`charger::scan()` 改为逐地址持锁），并建议一并补齐 **B3-R3**、**B3-R4**、**B3-R5** 及两处 nit。
