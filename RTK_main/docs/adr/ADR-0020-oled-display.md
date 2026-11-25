# ADR-0020：P2 OLED（SH1106）显示驱动与 WouoUI 只读状态页

状态：**Proposed**（草稿，待 Architect 评审定稿）。由 OL1–OL6 任务卡实施，已合入 main（OL1 `2eb65ba`，OL2–OL6 `c2e9293`，2026-11-25 P2 v1.41 HIL 通过）；任务卡已清理，见 git 历史。
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
  - OLED 按页获取和释放共享互斥锁。页地址、列地址与脏列像素用 SH1106 的 Co 控制位在一次同步 I2C 事务中发送，传参预算为 **8 ms**（`kPageTimeoutBudgetMs`）。ESP-IDF 同步驱动内部等待及错误恢复仍可能延长持锁，最大时长以实板测量为准。`ESP_ERR_TIMEOUT` 与 `ESP_ERR_INVALID_RESPONSE` 在释放页锁后尝试 `i2c::resetBus()`，传输失败页摘要失效，下一次可完整重写该页。Charger 保持 0 tick 尝试加锁。
  - `Charger` 既有 B3 行为（启动探测 `0x1A`、无芯片只报一行 W、REG02 读改写/REG03 校验、`power i2c scan`）完全不变。

### 2.3 模块归属、任务调度与刷新策略

- **模块归属**：`main/modules/oled/`（`OledModule`、`OledUi`、`OledCmds`），在 `module_list.inc` 登记为 `kAlways`（覆盖 `kNormal` 蓝牙/Wi-Fi 与 `kChargeStandby` 充电待机）。
- **独立低优先级任务**：仅在面板探测成功时由 `OledModule::start()` 创建静态任务 `"oled"`（并登记到 `sys tasks` 供 `stack_hwm` 观测）：
  - 核心：**Core 1**（远离 Core 0 上的 Bluedroid/Wi-Fi/GNSS-IO/Console），优先级：**1**（严格低于 `kLink` 的优先级 10，随时让位于链路与按键轮询）。
  - 栈与帧缓冲：**3072 B** 任务栈（`task.oled`）与 **1024 B** 帧缓冲（`oled.fb`）均按 ADR-0011 §7 经 `BootArena` 在 `allocateBootBuffers()` 启动期一次性分配并绑定，**仅当板型具备 `Feature::kOled` 时分配**（E1 等无 OLED 板型与 console-only 降级启动零占用）；驱动与总线控制块全部全零初值落入 `.dram0.bss`。
  - 刷新以事件驱动。活动期使用 34/33/33 ms 的绝对节拍（每 100 ms 3 帧），跳过已错过的槽。按键、关机与数据变化可唤醒 OLED，按键提前唤醒沿用已排定的节拍与单击截止。关机、重启和 I2C 恢复各按原有紧急性或有界退避安排。
  - **空闲不写 I2C**。唯一 1 KB 外供帧缓冲为 `ScreenBuff[8][128]`。每页保留 8 个 16 列组的 32-bit FNV-1a 摘要，按首末变化列组计算连续传输范围。未变化的页跳过，首次刷新及失败后对相应页完整发送。


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

## 5 OL2 增补：表盘 UI 架构、按键方案与关机进度

由 OL2 任务卡实施（报告 `docs/reports/OL2-oled-ui.md`），在 OL1 驱动与共享 I2C0 基础上升级为“开机自检 → 表盘 ↔ 四子菜单 + 真实进度关机页”架构，继续仅使用 WouoUI `graph/font` 子集（无新增第三方源文件、无浮点、无运行期堆）。

### 5.1 UI 模块拆分

| 文件 | 职责 | 约束 |
| --- | --- | --- |
| `main/modules/oled/OledAnim.hpp/.cpp` | 单键短按/双击消歧器 `ClickDetector`、Q10 定点三次缓出 `easeOutCubic` / `lerpFixed`、Q8 定点阻尼弹簧 `FixedSpring`、数字滚动 `NumberRoll`、页面过渡 `PageTransition` | 纯逻辑、无 IDF/FreeRTOS 头、0 浮点、0 堆 |
| `main/modules/oled/OledUi.hpp/.cpp` | 只读视图模型 `ViewModel`、开机自检状态机 `SystemCheckSm`、主界面控制器 `UiController`、8 页 FNV-1a 脏页跟踪 `FrameTracker`、纯整数格式化与 `SatTable` 紧凑摘要 | 纯逻辑、全零落 `.dram0.bss` |
| `main/modules/oled/OledIcons.hpp` | 自绘 XBM 风格单色位图（列主序垂直字节，卫星/蓝牙/Wi-Fi/闪电/告警/勾选/叉号/4×14×14 入口图标）与 32 点关机进度环定点坐标表 | `constexpr` 常量表，全落 `.rodata`，0 DRAM / 0 IRAM |
| `main/modules/oled/OledRender.cpp` | WouoUI 图形渲染器（Logo 逐列展开、自检列表、表盘、卫星/位置/设备/电源四子菜单、充电待机、关机圆环进度、滑动视差与图标展开/收回过渡） | 单线程调用、无硬件访问、渲染结束置空 `p_cur_pen` |
| `main/modules/oled/OledModule.hpp/.cpp` | 30 fps 动画 / 静止 0 写调度、只读快照采集（`collectSatSummary` 独立栈帧避免撑大栈）、FPS/I2C/帧耗时与脏页统计 | Core 1、prio 1、栈 3072 B + 1 KB 帧缓冲仍由 `BootArena` 仅在 `Feature::kOled` 分配 |

### 5.2 按键方案 A（单键消歧）

- 物理按键仍由 `PowerModule` 内 `PowerKeySm` 采样（`kLink` 10 ms 周期），短按上限对齐 `600 ms`（`kKeyShortMaxMs`），按住状态与起点/阈值经 `UiEventBus` 无锁原子暴露给 OLED：
  - **长按进度与防误触（正常模式）**：按住 ≥ 600 ms 起叠加长按进度浮层（600→2500 ms 线性填充）；2500 ms 前松开 150 ms 收缩收起，不产生短按、不关机。到达 2500 ms 时，OLED 在线仅投递长按到达事件进入 `POWER OFF?` 确认页（默认选中 `Cancel`，5 s 倒计时自动取消，选中 `OK` 后双击才经 `OledAction::kShutdownKey` 发起 `power::requestShutdown(kKey)`，期间配对弹窗排队）。无 OLED 或面板离线时保持长按 2.5 s 直接关机。
  - **短按（< 600 ms 松开）**：向 `UiEventBus` 无锁 SPSC 时间戳队列投递消抖短按生产时刻并唤醒 `oled` 任务。
- `OledAnim::ClickDetector` 在 OLED 任务内按消抖短按的**生产时间戳**完成短按/双击消歧（窗口常量 `kDoubleClickWindowMs = 300 ms`，先按序消费全部队列事件再判绝对截止，不受 OLED 刷屏忙期漂移影响）：
  - **短按（`kSingle`）**：表盘下循环切换底部 5 个入口图标（`SATS/POS/COMM/DEVICE/POWER`），驱动 `FixedSpring` 弹簧选中框跟随；卫星子页切换“星座柱状图 / Top-8 CN0 条”；其余统一子页（位置/通信/设备/电源）在右侧可滚动列表行间循环（弹簧行间移动 ≤ 180 ms、平滑滚动）；确认弹窗内循环切换两个选项，焦点使用相同的定点弹簧和挤压反馈。
  - **双击（`kDouble`）**：自检页跳过剩余展示滑入表盘；表盘下伴随选择框挤压/回弹（≤ 120 ms）展开进入子页（内容行 40 ms 错峰上移 6 px）；子页下执行选中动作（如 `Power off` 进入确认页、通信模式/配对切换）或由末尾左箭头 `Back` 收回返回表盘；确认弹窗内执行当前选中项。子页面无操作 `60 s`（`kSubpageIdleTimeoutMs`）自动收回回表盘。
  - **确认弹窗（OL6）**。关机和切换到 Wi-Fi 使用 `Cancel` / `OK`，蓝牙配对使用 `Reject` / `Accept`。新弹窗默认选中安全项。关机和 Wi-Fi 的 `Cancel` 直接关闭，配对的 `Reject` 向 SPP 提交拒绝，30 s 未判定时由 SPP worker 拒绝。选框动画沿用 OL5 节拍，仅重绘选项区域，关机弹窗独立更新倒计时区域。
  - 发起每个脏页前检查单击绝对截止（`firstPressMs + 300 ms`）。若当前同步传输跨过截止，完成该页后立即回到 UI 状态机，下一页待重新渲染。状态机先消费队列中的生产时间戳，再判定到期单击。休眠前仍重算剩余窗口。
### 5.3 开机自检、表盘/子菜单与关机进度

- **开机自检（`kSystemCheck`）**：单色 Logo 逐列展开（450 ms + 200 ms 停留）→ 5 项非阻塞真实自检（电源/电池、NVS、蓝牙/Wi-Fi 射频、充电芯片、GNSS 探测含型号/固件）→ 滑动视差切入表盘（300 ms）。
  - 每项最短展示 `kMinStepMs = 200 ms`（≤ 300 ms），仅用于可读性，不阻塞任何模块初始化；充电芯片或未启用 GNSS 缺失显示 `-`（`kAbsent`）不算失败；失败项不阻塞，入表盘后状态栏常驻告警三角图标。
  - 最小只读快照增补：`PowerSnapshot` 新增 `batteryAdcReady`、`chargerPresent`、`shutdownDoneMask`；`DataCenter` 新增无锁原子 `wifiReady`（由 `WebModule` 写）。
- **表盘、统一子页与充电待机（OL4）**：
  - 表盘（`kDialplate`）：10 px 顶栏（卫星图标 + 滚动数、自检告警、解状态、BT/Wi-Fi 图标、充电闪电、滚动电量 % + 电池图形）+ 中部大号解状态（`Font_12_24`，状态切换带 180 ms 竖向滚动）与 GNSS UTC / 精度简报 + 选中入口名称标签 + 底部 5 个 16×16 入口图标与弹簧选中框。
  - 统一子页：顶部沿用 10 px 状态栏；位置/通信/设备/电源采用左 `40×52` 主卡 + 右最多 3 行可滚动列表（图标 + 名称 + 右对齐值 + 滚动条 + 统一左箭头 `Back`）；卫星页保持 4 柱双段图与 `used/visible` 标题。电源页提供 `Power off` 项，双击进入 `POWER OFF?` 确认页。
  - 充电待机（`kChargeStandby`）：左大号滚动电量 + 电池条（充电未满时 ≤ 2 fps 流动动画，充满停止恢复 0 写入）+ `CHG`/`FULL`/`USB` 与电压；右电源图标进度环（随按住时长实时填充，松开回落）+ `HOLD 2s ON`；底行 `FW` / `HW`。
- **关机进度（`kShutdown`）**：
  - 首行展示关机原因（`LOW BATTERY` / `CONSOLE` / `KEY` / `PHONE`）与终态标签；进度环、阶段文案和三枚小图标勾选里程碑（保存 / 射频 / 断电）由 `PowerSnapshot::{shutdownStage, lastShutdown, shutdownDoneMask, shutdownDegradedMask}` 驱动。蓝牙收尾显示 `CLOSING SPP`，SPP 会话已断开且扫描关闭请求已提交时显示 `SCAN REQ OK`。该里程碑以 GAP 提交成功为准。SoftAP 模式在 AP 停用后显示 `AP MODE OFF`。保存超时（1 s）、无线收尾超时（BT 150 ms / SoftAP 5.5 s）或安全电平失败时，OLED 显示对应的 `SAVE TIMEOUT` / `RF TIMEOUT` / `SAFE IO ERR` 和叉号标记。低电、console 及手机来源不经确认页直接进入关机页。
  - **基线强制下电 / USB 重启兜底例外（既定策略）**：`PowerModule` / `ShutdownSm` 沿用基线 ADR-0015 策略——即使保存或射频等待超时、或安全电平配置报错，仍继续推进断电（无 USB）或 U3 关断后的 USB 重启，防止设备挂死不关机或电池过放；但不再伪造“全部完成”位，明确向用户展示降级文案。
  - **阶段停留**。OLED 在线时，告知、保存、静默和 USB 的断电进行中阶段在完整帧确认后至少停留 `kMinStageVisibleMs = 250 ms`。保存和静默仍须满足原收尾条件。中间阶段未收到刷出确认时，从阶段开始最多等待 100 ms 的刷屏宽限加 250 ms 停留。
  - **无 USB**。进入 `kCutPower` 发布 `kBatteryOffReady`，显示 `POWER OFF`。Power 观察到对应终帧刷出后至少停留 300 ms，再启动 GPIO4 首段高电平。100 ms 未确认终帧时记录超时，按原协议断电。无 OLED 时直接走断电脉冲。GPIO4 首段仍从实际拉高开始计时。
  - **有 USB**。U3 关断后发布 `kUsbOffReady`，显示 `RESTARTING`。日志首次排空或达到原上限后锁存放行，并启动未确认终帧的 30 ms 绝对截止。终帧完成可通知 `kLink`，等待前后复核截止且每次最多相对等待 1 tick。确认后至少停留 300 ms 再重启，终帧超时或 OLED 离线时按原重启路径推进。两种终帧凭据独立。OLED 清退避后以 `kWritten` / `kSkipped` 确认完整终帧，错误时 2 ms 后补刷。
  - **30 s 超时回退**。回到 `kIdle` 时恢复表盘，`kShutdownAbort` 记录从进入 `kCutPower` 起经过的毫秒数。

### 5.4 帧率调度、控制台统计与预算增量

- **帧率调度**。OL5 将动画、滚动和进度环接入 30 fps 绝对节拍，充电待机的电池条仍在 500 ms 相位边界变化。静止期等通知，有限的 UI 截止独立计时。子页转场仅下移与推入状态栏下的内容区，顶栏复用上一帧。每页单次事务仍使用 400 kHz、8 ms 超时传参，见 §2.2。
- **测量**。`oled` 显示最近 2 s 写屏帧率和实际唤醒数，以及刷出间隔、事务字节数、渲染耗时和单核 CPU 占用估算。`oled bench <seconds>` 重复表盘与电源子页转场，按整段采样并暂停 OLED 自身日志。
- **预算增补**：框架状态 DRAM 静态增量 ≤ 1 KB（`UiController` 落 `.dram0.bss`）；Flash 增量主要来自新启用 `Font_12_24` 与自绘位图/渲染器，实测见 `docs/reports/OL2-oled-ui.md`；IRAM 增量 0 B。
