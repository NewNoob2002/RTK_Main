# ADR-0015：P2 板级识别与硬件配置表（Board Profile）

状态：已批准（2026-11-18，用户）。由 B 系列任务卡实施（B1 起，见 §5）。
关联：ADR-0011（启动步骤与内存）、ADR-0012（GNSS 驱动）、ADR-0014（启动早期配置）、AGENTS.md §1（板级引脚须经 ADR）。
硬件依据：`docs/hardware/P2/schematic/p2v1.41.pdf`；行为参考（只读）：P2 V2.7 Arduino 固件
`ESP32_PROJ/P2 手持式GNSS接收机项目/源码/P2 V2.7 基于V2.6增加电子围栏多区块/P2_V2.7/`（下称 V2.7）。

> 命名：B = Board。
>
> 范围补充（用户 2026-11-23）：RTK_main 后续要适配多个产品，不只 P2。本 ADR 的 GPIO35 识别与配置表即“启动前产品识别 → 运行对应产品适配”的机制，
> P2 v1.41 是首个登记项；新增产品沿用 §2.1 窗口规则登记，产品特有的外设与流程另立 ADR。

## 1 背景

- 量产目标是 P2 手持接收机；E1-5.4 板只作台架测试，不是产品（用户 2026-11-18）。P2 v1.41 之前的板已停产，不兼容。
- 现固件按 E1-5.4 引脚写死：启动第 1 步把 GPIO26 拉高做电源保持，GNSS 电源 GPIO25。P2 上 GPIO26 是 4G 模块使能，
  GPIO25 是开关机检测输入，GPIO13/15/2 是蜂鸣器/马达/充电状态——**现固件不能直接跑在 P2 上**。
- 目标：一份固件支持 P2 后续各硬件版本，启动时识别版本并按配置表驱动引脚与模块（参照 SparkFun RTK Everywhere 的
  `productPropertiesTable` + GPIO35 ID 分压）。

## 2 决定

### 2.1 识别：GPIO35 分压

- P2 每个硬件版本在 GPIO35（`DEVICE_SENSE`，ADC1_CH7，仅输入）上焊一对分压电阻：3V3 —R134— IO35 —R135— GND。
  v1.41：R134 = R135 = 100k → 标称 1650 mV（用户确认）。
- 读法：ADC1 oneshot，衰减 12 dB，12 bit，校准方案 line fitting；丢弃首次采样后取 8 次平均，换算 mV。
  eFuse 无 Vref 数据时仍用 line fitting + 标称 1100 mV（12 dB 档非线性，理想线性换算会偏出窗口），打一行 W。
- 匹配：配置表每项带闭区间 `[idMinMv, idMaxMv]`；落在某窗口内 → 该版本；否则 → 未知。
- 窗口规则（host 测试强制）：各窗口互不重叠，相邻窗口间隔 ≥ 100 mV，且都在 [150, 2450] mV（ESP32 12 dB 线性区）。
- v1.41 窗口：**[1500, 1800] mV**（1% 电阻、3V3 ±3%、ADC 校准误差合计约 ±100 mV，再留余量）。
- 新硬件版本须换分压比，优先选与已有窗口相距 ≥ 250 mV 的标称值（例：100k/47k ≈ 1060 mV，47k/100k ≈ 2245 mV，
  100k/22k ≈ 595 mV）；硬件负责人登记到本 ADR §3 再出板。

### 2.2 启动顺序

- 新增启动步骤 **`identify`**，位于 `kBootSteps` 第一位（`power` 之前），原因：`power` 及之后的步骤都要按板驱动引脚。
  `identify` 先调用 `installDirectLog()`（从 `stepPower` 前移），再读 GPIO35、匹配配置表、打 I `board <名称> id=<mV> mV`；
  成功后只设置 §2.5 的安全输出电平，其余输出脚不动。
- 识别失败（未知窗口或 ADC 失败）：打 E `board unknown id=<mV> mV`，步骤返回 kFail → 现有降级路径进 console-only。
  不做电源保持、不驱动任何板级输出；电池供电时松开按键即断电，USB 供电时控制台可用于诊断。
  这是有意的：错误的引脚表比关机更危险。
- 结果存入 `AppContext`（`const board::Profile *`，constinit 表内指针，启动后只读），各模块通过访问函数读取。

### 2.3 配置表 `app::board::Profile`

代码放 `main/board/`（命名空间 `app::board`）：`BoardProfile.hpp/.cpp` 为纯值类型与 `constexpr` 表、窗口匹配函数，无 IDF 头，
在 `tools/main_host_check` 测试；ADC 读取与电源保持放同目录的 IDF 文件。字段（B1 起，按需增加）：

| 字段 | 含义 |
| --- | --- |
| `id`（`enum class BoardId`） | `kP2V141`、`kE1DevKit` |
| `name` | 启动横幅与 `status` 显示，如 `"P2 v1.41"` |
| `idMinMv` / `idMaxMv` | §2.1 窗口；`kE1DevKit` 为 0/0（不参与自动识别） |
| `gnss` | UART 号、RX/TX GPIO、电源 GPIO（-1 = 无）、电源有效电平 |
| `powerHold` | 方式 `kNone` / `kLatchGpio`（E1：GPIO 高电平保持） / `kKeyEmulate`（P2，见 §2.4）及参数 |
| `safeOutputs` | 识别后立即置为安全（关闭）电平的输出脚与电平列表（≤ 8 项） |
| `features` | 位掩码：`kCellular`、`kOled`、`kImu`、`kCharger`、`kBeeper`、`kMotor`、`kLeds`（B1 只登记，不据此启停模块） |

- 引脚类 Kconfig（`RTK_GNSS_RX_GPIO`、`RTK_GNSS_TX_GPIO`、`RTK_GNSS_POWER_GPIO`、`RTK_GNSS_POWER_ACTIVE_HIGH`）删除，
  由配置表给出；`RTK_GNSS_UART_NUM` 保留（编译期 `static_assert` 与控制台 UART 冲突检查依赖它），配置表中的 UART 号
  须与之相等（host 测试 + 运行期断言）。
- 新增 Kconfig 选择 `RTK_BOARD`：`AUTO`（默认，§2.1 识别）/ `FORCE_E1_DEVKIT` / `FORCE_P2_V141`。强制时仍读一次 ID
  并打印 mV，但不据此判断；仅用于台架。

### 2.4 P2 电源保持（`kKeyEmulate`）

P2 无保持脚。GPIO4（`TELL_MCU_KEY`）经 Q16 模拟按键按下，电源锁存芯片据此保持；GPIO25（`ON_OFF_ADC`，ADC2_CH8）
读回电源通路状态（V2.7 `P2_V2.7.ino` setup：`TELL_MCU_KEY=HIGH; while(analogRead(ON_OFF_ADC) < 2000); TELL_MCU_KEY=LOW`）。

- 开机：GPIO4 输出高 → 每 10 ms 读一次 GPIO25（ADC2，12 dB，校准换算 mV）→ 连续 3 次 ≥ `senseOnMv` 视为保持成功 →
  GPIO4 拉低。超时 `holdTimeoutMs` 未成功：GPIO4 拉低，打 E，`power` 步返回 kFail（降级）。V2.7 无超时，此处必须有。
- 参数（v1.41）：`senseOnMv = 1600`（V2.7 阈值 2000/4095 @ Arduino 默认 11 dB 约 1.6–1.7 V，**待 HIL 校准**），
  `holdTimeoutMs = 5000`，GPIO25 读取期间保持输入、不上下拉。
- ADC2 与 Wi-Fi 冲突；本固件不开 Wi-Fi，且读取只在启动早期，蓝牙启动前完成。
- 关机（B2 实施，此处只定协议）：无 USB 供电时 GPIO4 高 2 s，然后高/低各 3.1 s 交替直至掉电（V2.7 `screen_power_off()`）；
  有 USB 供电时不能断电，改为关 4G（GPIO26 低）、复位 GNSS、进入充电显示。

### 2.5 P2 安全输出电平（识别成功后立即设置）

| GPIO | 网络 | 安全电平 | 依据 |
| --- | --- | --- | --- |
| 13 | BEEP_POWER | 高（蜂鸣器关，低有效） | V2.7 `BEEP_PWREN_OFF HIGH` |
| 15 | MOTOR_PWREN | 低（马达关） | V2.7 `gpio_init` |
| 23 | LED_POWER_ON | 低（LED 关） | V2.7 `gpio_init` |
| 26 | 4G_PWREN | 低（4G 关；B 系列 4G 模块实现前不开） | V2.7 关机路径 `GSM_PWREN LOW` |
| 4 | TELL_MCU_KEY | 低（由 §2.4 接管） | V2.7 `gpio_init` |

- 不动：GPIO12（OLED_POWER_ON，MTDI 启动绑定脚，B 系列 OLED 卡再定）、GPIO2（PWR_CHG，输入）、GPIO0/5/14/19（未用或绑定）。
- GPIO15、GPIO2、GPIO12 是启动绑定脚：只在固件运行后驱动，不加外部状态假设。

### 2.6 不变

- 蓝牙名按板卡产品名 `<product>-<SN>`（N1，2026-11-19）；NVS 命名空间 `"rtk"`、产品 Kconfig 前缀 `RTK_*` 不变（AGENTS.md §2）。是否按板改蓝牙名另行决定。
- 模块清单 `module_list.inc` 仍编译期决定。按 `features` 在运行期跳过模块的机制，等第一个 P2 专有模块（4G/OLED/充电）
  落地时由对应任务卡引入（YAGNI）。

## 3 P2 v1.41 引脚表（原理图 sheet 7 U11，按 WROOM-32 焊盘号换算 GPIO）

| GPIO | 网络 | 方向 | 用途 / 使用卡 |
| --- | --- | --- | --- |
| 35 | DEVICE_SENSE | ADC1 输入 | 板级识别（B1） |
| 16 / 17 | GNSS_UART_TX1 / RX1 | UART2 RX / TX | UM980 COM1（B1，同 E1） |
| 4 | TELL_MCU_KEY | 输出 | 电源保持 / 关机（B1 / B2） |
| 25 | ON_OFF_ADC | ADC2 输入 | 电源通路检测（B1） |
| 18 | PWR_KEY_DETEC | 输入 | 按键（V2.7 `key_pin`，INPUT_PULLUP），B2 |
| 34 | BAT_V_ADC | ADC1 输入 | 电池电压（V2.7 ×2 分压），后续 |
| 27 | VBUS_DET_ESP32 | 输入 | USB 供电检测，后续 |
| 39 | USB_Detec | 输入 | SGM41513 nPG（v1.41 实板测得插拔均为 0 V 且固件读高，不可用，B2 禁用并改走 I2C 0x1A） |
| 2 | PWR_CHG | 输入 | 充电状态 STAT（低有效，R237 10k 上拉），B2（I2C 故障退化） |
| 21 / 22 | I2C SDA / SCL | I2C | SGM41513 充电芯片 0x1A（B2：USB 在位与充电参数）、SH1106 OLED 0x3C（后续） |
| 32 / 33 | ESP32_UART_RX2 / TX2 | UART | EC600U/Air600 4G 模块，后续 |
| 26 | 4G_PWREN | 输出 | 4G 使能（B1 置低） |
| 13 / 15 / 23 | BEEP / MOTOR / LED_POWER_ON | 输出 | B1 置安全电平；驱动后续 |
| 12 | OLED_POWER_ON | 输出（绑定脚） | OLED，后续 |
| 1 / 3 | TXD0 / RXD0 | UART0 | 控制台 + CH340 |

ID 窗口登记：

| 版本 | R134 / R135 | 标称 | 窗口 |
| --- | --- | --- | --- |
| P2 v1.41 | 100k / 100k | 1650 mV | [1500, 1800] mV |

## 4 风险与未知

- `senseOnMv` 与 ID 实测值需 HIL 校准（B1 验收项）。
- 4G 模块在 GPIO26 低时的漏电与状态未测；B 系列 4G 卡处理。
- `UNKNOWN`：锁存芯片（sheet 5 U3）型号与最短按压时间；以 V2.7 行为为准，超时 5 s 覆盖。

## 5 实施

| 卡 | 内容 | 状态 |
| --- | --- | --- |
| B1 | 配置表 + `identify` 步骤 + P2 电源保持 + 安全电平 + GNSS 引脚改读配置表 | 待实施 |
| B2 | P2 按键与关机（§2.4 关机协议），卡 `docs/tasks/B2-power-key-shutdown.md` | 已实现，分支 `feat/b2-power-key`（`612ff8a`）；2026-11-23 主路径 HIL PASS，已合入 main（`00f73c2`） |
| 后续 | 4G、OLED、充电/电池、蜂鸣器/马达/LED、IMU；引入按 `features` 启停模块 | 未开始 |
