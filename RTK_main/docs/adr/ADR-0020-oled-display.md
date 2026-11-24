# ADR-0020：P2 OLED（SH1106）显示驱动与 WouoUI 只读状态页

状态：**Proposed**（草稿，待 Architect 评审定稿）。由任务卡 `docs/tasks/OL1-oled.md`（OL1）实施。
关联：ADR-0008（代码组织与堆分级）、ADR-0009（日志通道与控制台）、ADR-0011（启动顺序与内存）、ADR-0015（板级识别与 `Feature::kOled`）、ADR-0016（控制台档位）、ADR-0017 §7（OLED 交互菜单延后）。
硬件依据：`docs/hardware/P2/schematic/p2v1.41.pdf`、`docs/reports/P2-legacy-gap.md` §1.5/§2.2；旧实现参考（只读）：`P2_V3.1_IDF/main/src/xyz_oled.c`、`main/user_src/user_oled.c`。

> 命名：OL = OLED。

## 1 背景

- P2 v1.41 配备 128×64 单色 OLED（控制器 SH1106，I2C0 7-bit 地址 `0x3C`，电源使能脚 `GPIO12` 高有效，`Feature::kOled`）。
- 现固件仅登记了 `Feature::kOled`，未驱动 `GPIO12` 与 `0x3C`，导致手持机脱离手机时无法查看定位状态、卫星数、电量/充电状态和射频模式。
- 充电芯片 SGM41513（`0x1A`）与 SH1106（`0x3C`）共用同一条板级 I2C0 总线（SDA=`GPIO21`，SCL=`GPIO22`），当前 `main/board/Charger.cpp` 独占创建总线。
- 用户已选定 **WouoUI-PageVersion**（MPL-2.0，纯 C、无堆分配）作为后续界面基础库；本期（OL1）先实现只读状态页与单键短按翻页，不实现设置菜单与动作项（ADR-0017 §7）。

## 2 决定

### 2.1 硬件事实与驱动约束

| 项 | 值 | 来源 |
| --- | --- | --- |
| 控制器与分辨率 | SH1106，128×64 单色，内部 RAM 132 列（列偏移 2：`0x02`/`0x10`） | `P2_V3.1_IDF/main/src/xyz_oled.c:54-69,342` |
| I2C 接口 | `I2C_NUM_0`，SDA=`GPIO21`，SCL=`GPIO22`，7-bit 地址 `0x3C`，速率 **400 kHz** | `p2v1.41.pdf`、ADR-0015 §3、OL1 任务卡 |
| 电源使能 | `GPIO12`（`OLED_POWER_ON`，MTDI 启动绑定脚），推挽输出高有效；上电延时 100 ms | `p2v1.41.pdf`、`xyz_oled.c:333-337` |
| 屏幕方向 | P2 v1.41 物理倒装 180°（`0xA0` 段重映射 + `0xC0` COM 扫描） | `xyz_oled.c:13-25,384` |
| 关屏断电序列 | 写 `0xAE`（关显示）→ `0xAD 0x8A`（关电荷泵）→ 拉低 `GPIO12` | `xyz_oled.c:46-51` |

- 仅当 `boardProfile` 包含 `Feature::kOled` 且 `oled.i2cPort >= 0` 时才拉高 `GPIO12` 并探测 `0x3C`；无该特性的板型（如 E1 DevKit）不触碰 `GPIO12` 和 I2C0。
- 探测 `0x3C` 无应答或初始化失败时，仅记录**一行** W 级日志，立即拉低 `GPIO12` 并禁用显示模块，不影响其他模块与启动流程。

### 2.2 板级 I2C0 总线共享

- 新增 `main/board/I2cBus.hpp/.cpp`（命名空间 `app::board::i2c`）作为板级 I2C0 单一总线所有者：
  - `ensureInit(port, sda, scl)` 幂等创建一次 `i2c_new_master_bus`（保持 `enable_internal_pullup=1`、`glitch_ignore_cnt=7`）与一把静态 FreeRTOS 互斥锁（`xSemaphoreCreateMutexStatic`，具备优先级继承）。
  - `Charger`（100 kHz，`0x1A`）与 `Sh1106`（400 kHz，`0x3C`）各自调用 `i2c_master_bus_add_device` 挂载在同一总线句柄上；ESP-IDF `i2c_master` 按设备句柄切换时钟速率。
  - 锁粒度与超时恢复：OLED 刷屏**按页（8 page）**获取/释放总线互斥锁（单页 128 B @ 400 kHz 约 3 ms，单次传输超时上限 8 ms，确保双传输持锁上限 16 ms 严格小于跨任务互斥等待上限 20 ms），绝不跨整帧长期持锁；单页失败立即中止本轮剩余页，连续失败执行有界退避，`ESP_ERR_TIMEOUT` 在释锁后调用 `i2c::resetBus()` 复位总线防止 SDA 卡死；`Charger::refreshInputStatus` 继续使用 0 tick 非阻塞尝试加锁，确保 `kLink` 绝不被 OLED 阻塞。
  - `Charger` 既有 B3 行为（启动探测 `0x1A`、无芯片只报一行 W、REG02 读改写/REG03 校验、`power i2c scan`）完全不变。

### 2.3 模块归属、任务调度与刷新策略

- **模块归属**：`main/modules/oled/`（`OledModule`、`OledUi`、`OledCmds`），在 `module_list.inc` 登记为 `kAlways`（覆盖 `kNormal` 蓝牙/Wi-Fi 与 `kChargeStandby` 充电待机）。
- **独立低优先级任务**：仅在面板探测成功时由 `OledModule::start()` 创建静态任务 `"oled"`（并登记到 `sys tasks` 供 `stack_hwm` 观测）：
  - 核心：**Core 1**（远离 Core 0 上的 Bluedroid/Wi-Fi/GNSS-IO/Console），优先级：**1**（严格低于 `kLink` 的优先级 10，随时让位于链路与按键轮询）。
  - 栈与帧缓冲：**3072 B** 任务栈（`task.oled`）与 **1024 B** 帧缓冲（`oled.fb`）均按 ADR-0011 §7 经 `BootArena` 在 `allocateBootBuffers()` 启动期一次性分配并绑定，**仅当板型具备 `Feature::kOled` 时分配**（E1 等无 OLED 板型与 console-only 降级启动零占用）；驱动与总线控制块全部全零初值落入 `.dram0.bss`。
  - 刷新率：**≤ 5 Hz**（`ulTaskNotifyTake(..., pdMS_TO_TICKS(waitMs))`），收到短按或关机事件可立即唤醒，减少交互延迟；`kCutPower` 期间按精确剩余毫秒数唤醒，避免错过 USB 关机重启前干净断电。
  - **空闲不写 I2C**：帧缓冲为唯一一份 1 KB 外供 `ScreenBuff[8][128]`；每页计算 64-bit FNV-1a 摘要（共 64 B 状态，省去第二份 1 KB 对比缓冲），仅当页内容变化时才写该页 I2C，静止画面 I2C 写入为 0。

### 2.4 页面清单与只读数据来源

所有页面只读消费 `DataCenter` 快照与 `AppContext` 只读字段，**不新增跨模块写接口**：

| 页面 | 触发条件 | 显示内容 | 数据来源 |
| --- | --- | --- | --- |
| 开机画面 (`kSplash`) | 正常启动初始化完成，持续 1.5 s（短按可提前跳过） | 产品短名（`P2`）+ 固件版本（`RTK_FW_VERSION`） | `board::productName()`、`appVersion()` |
| 状态页 (`kStatus`) | 开机画面结束或翻页 | 射频模式（BT 连接/等待、Wi-Fi AP）、电量 % 与充电标志、GNSS 状态/解状态/卫星数/hAcc | `radioMode`、`sessions`、`powerStatus`、`gnssStatus`、`gnssFix` |
| 设备信息页 (`kDeviceInfo`) | 状态页短按翻页 | 产品板型、SN、硬件版本 `hwVer`、固件版本 `fwVer` | `board::current()`、`systemStatus`、`appVersion()` |
| 充电待机页 (`kChargeStandby`) | `bootMode == kChargeStandby`（USB 供电开机） | 仅显示充电状态、电量 %、电压与进度条（短按不切至其他页） | `powerStatus` |
| 关机提示页 (`kShutdown`) | `shutdownStage != kIdle` | 关机提示（低电关机附加 `BATTERY LOW`），随后关面板并拉低 `GPIO12` | `powerStatus` |

无 GNSS、无定位解或无电池 ADC/充电芯片时，统一显示 `--` / `n/a` 占位，不崩溃、不越界。

### 2.5 按键与关机时序

- **按键**：`PowerModule` 在 `KeyEvent::kShort` 分支仅向 `DataCenter::uiEvents`（无锁原子计数 + 可选任务唤醒钩子）发送事件，不在 `PowerModule` 内直接画屏。`OledUi` 状态机消费短按：正常模式下在 `kStatus ↔ kDeviceInfo` 之间循环（在 `kSplash` 时提前进入 `kStatus`）；充电待机和关机中忽略短按。长按关机与待机长按开机行为完全不变。
- **关机**：
  - `PowerModule` 进入 `kAnnounce` 时唤醒 OLED 任务，OLED 立即绘制并刷出关机提示（约 25 ms）。
  - `PowerModule` 的 `kSave`/`kQuiesce`/`kCutPower` 状态机与时序保持 ADR-0015 原定值，**增加 0 ms 阻塞**。
  - 进入 `kCutPower` 后，OLED 在确保提示至少可见 200 ms（远短于无 USB 断电首脉冲 2000 ms）后，发送 `0xAE`/`0xAD 0x8A` 并拉低 `GPIO12` 断电；若 30 s 超时回退至 `kIdle`，OLED 检测到恢复后重新点亮。

### 2.6 WouoUI 集成与 MPL-2.0 合规

- 位置：`components/third_party/wououi/`，由 `components/third_party/CMakeLists.txt` 注册为独立 IDF 组件（及主机静态库 `third_party_wououi`），无需修改冻结的根 `CMakeLists.txt`。
- 最小裁剪：OL1 仅编译图形与字体子集（`WouoUI_graph.c`、`WouoUI_font.c` 及配套头文件），不编译未使用的页面/弹窗状态机（`WouoUI.c`、`WouoUI_page.c`、`WouoUI_win.c`、`WouoUI_user.c`）；后续 OL2 菜单卡可在同一 HAL 上增量引入。
- 零堆保证：确认上游 `Csource/` 全量无 `malloc`/`calloc`/`realloc`/`free`，并由 G7b 门禁对 `components/third_party` 目标文件做机器验证。
- MPL-2.0 合规：
  1. 完整保留上游 `LICENSE`（MPL-2.0 全文）于 `components/third_party/wououi/LICENSE`，并在 `README.md` 记录上游仓库、提交哈希（`82b985a`）及裁剪/修改清单。
  2. 所有第三方源文件保留原许可/作者注释头；任何必要修改（仅 `WouoUI_conf.h` 禁用 `printf` 日志与双缓冲宏）均在修改处以 `/* RTK_main modification: ... */` 显式标注。
  3. 门禁处理：第三方目录不参与自有格式/doxygen/clang-tidy 门禁（GEN-007），但参与 0-warning 编译与 G7b 运行时/堆检查。

### 2.7 控制台命令（ADR-0016）

- 新增 `oled` 命令组（默认子命令 `status`），归入 **DEV 档位**（与 `power`/`ota` 一致：PRODUCTION 与 NONE 档位不登记，由 `--gc-sections` 裁除，确保 `G7c-no-cli-deps` 通过）。
- 输出字段：`present`、`enabled`、`powered_off`、`page`、`frames`、`skipped`、`i2c_errors`。

## 3 内存与体积预算

| 资源 | 预算目标 | 说明 |
| --- | --- | --- |
| 帧缓冲（`oled.fb`） | 1024 B（`BootArena`，仅 `Feature::kOled`） | 单份 `ScreenBuff[8][128]`，无第二份 1 KB 对比缓冲；无 OLED 板型 0 B |
| 任务栈（`task.oled`）+ TCB | 3072 B（`BootArena`）+ ~128 B（`.bss`） | 静态任务 `oled`，仅 `Feature::kOled` 分配栈 |
| I2C 传输缓冲 + 页摘要 | ~240 B（`.bss`） | 129 B 单页写缓冲 + 8×8 B 页哈希 + 总线状态（全零初值） |
| Flash（WouoUI 子集 + 驱动/UI） | ≤ 20 KB | 仅链接实际使用的 `Font_6_8`/`Font_8_16` 与 `WouoUI_graph.c` |
| 堆分配 | 仅启动期一次性 | `BootArena`（4 KB 仅 `Feature::kOled`）+ IDF `i2c_master_bus_add_device`；运行期 0 堆 |
| IRAM 增量 | ≈ 0 B | 不放 IRAM，不影响剩余 14.9 KB IRAM 预算 |

实测值见 `docs/reports/OL1-oled.md`。

## 4 风险与待确认

- **HIL 验证**：本卡不烧录实板；SH1106 180° 旋转方向、对比度、上电 100 ms 稳定时间、充电待机显示及关机断电视觉效果需由 Architect 在 P2 v1.41 实板按 `docs/reports/OL1-oled.md` HIL 步骤确认。
- **与旧版差异**：本期仅实现只读状态页（开机/状态/设备信息/充电待机/关机），不含旧版短按菜单切换 Wi-Fi、OTA 进度页、电子围栏告警页与声光振反馈。
