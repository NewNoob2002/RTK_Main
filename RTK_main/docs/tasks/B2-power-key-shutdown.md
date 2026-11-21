# B2：P2 电源按键与关机管理

- 依据：ADR-0015 §2.4（开机保持 + 关机协议）、§3 引脚表、§4 未知项；原理图 `P2 手持式GNSS接收机项目/p2v1.41.pdf`
  sheet 5（电源：U3 9325-4531、S2、Q16、R185/R186、R182/R96、D16/D20、U29 SC8105、U1 SGM41513）与 sheet 7（ESP32 U11）；
  旧固件 `源码/P2 V2.7 基于V2.6增加电子围栏多区块/P2_V2.7/`（`key.ino`、`P2_V2.7.ino` setup 与 `Power_onoff_Event()`、
  `Battery.ino` `Low_Battery_Event()`/`Charge_check()`、`GPIO.ino`、`main.h:77-88`）。
- 基线：main `0260d2c`（含 B1 电源保持、W7）。
- 分支 `feat/b2-power-key`，工作树 `RTK_Main-b2`，构建前缀 `builds/b2-*`。
- 执行者不提交、不烧录、不连串口；HIL 由 Architect 做。
- 用户决定（2026-11-21）见 §6：Q1 充电待机、Q2 长按 2.5 s 走电源管理请求、Q3 有 USB 关机 = 关 U3 + 重启。

## 0 硬件分析（Architect，2026-11-21，供执行者理解，不需复核）

```
 sys(VBAT/SYS) ─ U3 VDD            U3 OUT ──D20──┬── U29 SC8105 EN ─→ VCC_3V3（ESP32 及外设）
 S2 按键 ─┬─ U3 KEY（内部上拉）                   │
          │                     VBUS ──D16──R89──┘   （USB 在位时 EN 也被 VBUS 拉高）
          ├─ Q16(2N7002) 漏极，栅极 = GPIO4 TELL_MCU_KEY（高 = 模拟按下）
          └─ R185 100k / R186 200k 分压 → GPIO18 PWR_KEY_DETEC（按下 = 低）
 U3 OUT ── R182 50k / R96 100k 分压 → GPIO25 ON_OFF_ADC（OUT 高 ≈ 2/3·sys ≈ 2.3–2.8 V）
```

- **按键能否直接开机：能。** S2 把 U3 KEY 拉低，U3 锁存 OUT 高 → SC8105 EN 高 → 3V3 上电 → ESP32 启动，全程不依赖 MCU。
  纯电池供电时 ESP32 能运行就说明 OUT 已为高；B1 的 GPIO4 模拟按压只是“补足按压时间、确认锁存”。
- **USB 插入（关机状态）**：VBUS 经 D16 直接使能 SC8105，ESP32 上电而 U3 OUT 仍低（GPIO25 低）。当前 B1 与 V2.7 都会
  在启动时模拟按键把 U3 打开 → 插 USB 即整机开机（B1 HIL：`power hold ok sense=2328 mV after 29 ms`，USB 在位）。
  原理图注释的设计意图是：GPIO25 低 → 只显示充电、关闭 ESP32 以外外设（见 §6 Q1）。
- **关机**：GPIO4 高 = 按住按键；U3 长按后 OUT 拉低。无 USB 时 3V3 随之掉电；**有 USB 时 3V3 由 VBUS 维持，ESP32 不会掉电**。
  V2.7：用户按住 ≥ 2 s 后 GPIO4 置高（与用户按压叠加），低电量路径先高 2 s 再高/低各 3.1 s 交替直到掉电
  → 推断 U3 关断需约 3 s 连续按压（`UNKNOWN`，HIL 测）。U3 是否在 MCU 挂死时也能被用户长按硬关断同样待测。
- **其他信号**：GPIO2 `PWR_CHG` = SGM41513 STAT（低 = 充电中，启动绑定脚，只读）；GPIO39 `USB_Detec` = SGM41513 nPG
  （低 = 输入电源良好，外部 10k 上拉）；GPIO27 `VBUS_DET_ESP32`（V2.7 只配置未使用）；GPIO34 电池 ×2 分压（ADC1）。
- **ADC2 限制**：GPIO25 是 ADC2_CH8，Wi-Fi 运行时不可用 ADC2。运行期改为**数字输入**读 U3 OUT（2/3·sys 高于 VIH）。

## 1 范围

1. **新模块 `power`**（`main/modules/power/`，按 R6 模块模板），kLink，`ModuleEnable::kAlways`；仅当板卡
   `powerHold.kind == HoldKind::kKeyEmulate`（P2）时 `init` 生效，其他板 `init` 直接返回不启用（不占轮询）。
   不引入新的按 `features` 跳过模块机制（ADR-0015 §2.6 留给 4G/OLED 卡）。
2. **引脚**（写进 `PowerHold` 或新 `PowerKey` 配置结构，不硬编码在模块）：key 检测 GPIO18（输入、关内部上下拉，
   分压已给电平；HIL 若浮动再改上拉）、U3 OUT GPIO25（B1 释放 ADC2 后改数字输入、无上下拉）、
   USB GPIO39 nPG（输入，低有效）、GPIO4 已由 B1 配置为输出低。E1 配置全部为 -1。
3. **按键状态机（纯函数，放 `main/board/` 或 `components/` 纯文件，host 测试 + ASan）**：
   - 10 ms 轮询、30 ms 消抖；输入 = (时间, 原始电平)，输出 = 事件 `kShort` / `kLongHold`。
   - **启动时按键可能仍被按住**（用户开机长按）：须先看到一次稳定松开，之后的按下才计数。
   - 长按阈值 Kconfig `RTK_POWER_OFF_PRESS_MS`（默认 2500，依据原理图注释“按下超过 2.5 秒执行软件关机”），
     达到即发 `kLongHold` 一次，不等松开。
   - `kShort`（< 1 s 松开）本卡只打 trace，留给 OLED 菜单卡。
4. **电源管理：请求 + 关机状态机**（参考 SparkFun RTK Everywhere：按键任务只 `requestChangeState(STATE_SHUTDOWN)`，
   主状态机再执行 `powerDown()`：停 SD 日志 → `gnss->standby()` → `peripheralsOff()` → 打印并 flush → 断电；
   `Tasks.ino:2544-2580`、`States.ino:669-672`、`Buttons.ino:35-86`。设置在修改时已 `recordSystemSettings()`，关机不额外保存）。
   - 入口 `power::requestShutdown(Reason)`（`kKey` / `kConsole`，预留 `kLowBattery`、`kPhone`），任何模块/命令只发请求，
     线程安全（原子标志或 executor 投递），不在调用方上下文里执行关机；“关机中”时再次请求忽略。
   - power 模块在 kLink 轮询中按阶段推进，每阶段打一行 trace：
   1. `kAnnounce`：打 I `power off reason=<r> usb=<0|1>`；停止接受新的设置写入（Settings Service 对后续请求回 busy/拒绝，
      已排队的执行完）；
   2. `kSave`：把所有待落盘数据写入 NVS：Storage 的延迟写（白名单 `flushWhitelist` 等 pending）、Settings Service 已排队请求、
      其他模块的延迟提交（执行者盘点所有“延迟写/去抖写”路径并在报告列出）；最多等 1000 ms，超时继续并打 W 列出未完成项；
      不新增 NVS 键；
   3. `kQuiesce`：断开蓝牙 SPP（让手机看到正常断开，不等重连）、Wi-Fi 模式则停 httpd/AP；GNSS 不发 `freset`/`reset`
      （UM98x `freset` 恢复出厂；SparkFun UM980 `standby()` 亦为空操作）；外设回安全电平（复用 B1 `setSafeOutputs`：
      4G 低、蜂鸣器/马达/LED 关）；打 I `device powered down` 并等待日志输出完成；
   4. `kCutPower`：见下两条；
   - **无 USB**：GPIO4 高 2 s → 高/低各 3.1 s 交替（ADR-0015 §2.4），每次低电平期读 GPIO25；30 s 仍未掉电 → 打 E、
      GPIO4 拉低、退出“关机中”回到正常运行（不重启，避免反复开关）；
   - **有 USB**：拉 GPIO4 直到 GPIO25 读到低（U3 关断，拔 USB 后保持关机；30 s 超时同上回退），然后 `esp_restart()`；
      重启后按 §1.5 进入“充电待机”。不做 V2.7 的 GNSS `freset`（UM98x `freset` 会恢复出厂配置）。
   - 关机序列不阻塞 kLink：做成模块内定时状态机，每轮只看时间推进。
5. **启动分支（改 B1 `stepPower`，最小改动）**：拉 GPIO4 之前先读一次 GPIO25：
   - 已为高 → 按键开机，照 B1 流程确认保持，`power_on=key`；
   - 为低且 USB 在位 → **充电待机**（用户 Q1）：不模拟按键（U3 保持关），GNSS 不上电/不探测、蓝牙与 Wi-Fi 不启动，
     只跑 storage/system/power（及 DEV 档控制台）；4G/蜂鸣器等保持 B1 安全电平；`power_on=charge`。
     - 待机中长按 2.5 s → 照 B1 模拟按键确认 U3 保持（`power hold ok`）后 `esp_restart()`，重启后 GPIO25 高 → 正常开机；
     - 待机中拔掉 USB → ESP32 随之掉电（硬件行为，无需处理）；
     - 实现方式：在启动流程里新增一个“运行档”（如 `BootMode::kChargeStandby`），用现有 `ModuleEnable`/启动步骤机制跳过模块，
       不复制模块表；执行者在报告说明选择。
   - 为低且 USB 不在位（异常：U3 关却在运行，如按压太短）→ 照 B1 模拟按键开机，`power_on=key`，打 W。
   - 开机原因写入 `power status`，便于 HIL 区分。
6. **控制台**（DEV 档，`power` 组）：`power status`（key/u3_on/usb/chg 电平、开机原因、按键计数、最近关机原因）、
   `power off`（走 §1.4，`kConsole`）。生产档不注册 `power off`。
7. **Trace**：新增 `TraceCat` 不必要时复用 `kSys`；按键事件、关机各阶段一行，不刷屏。

## 2 不做

- 电池电量、低电量自动关机（需 GPIO34 标定，另立 B3；本卡只留 `kLowBattery` 枚举）。
- SGM41513 I2C 配置、BATFET/运输模式、OLED 充电图标、蜂鸣器/马达提示、深睡眠。
- 手机协议关机命令。

## 3 测试（执行者）

- Host：按键状态机（抖动、开机按住、刚好 2500 ms、短按、长按后继续按不重复触发、时间回绕）；关机序列状态机
  （阶段顺序 announce→save→quiesce→cut、保存超时继续、无 USB 2 s + 3.1 s 交替、30 s 超时回退、有 USB 等 GPIO25 低后重启、重复请求忽略）；启动分支（GPIO25 高 / 低+USB 充电待机 / 低无 USB）与充电待机长按开机。注入 GPIO/时间桩，不依赖 IDF。
- 全量门禁 `bash tools/ci/run_gates.sh --prefix b2` 全过；报告写 `docs/reports/B2-power-key.md`
  （改动文件、DRAM/IRAM/bin 差值、门禁结果、偏离与 ADR-0015 待改点）。

## 4 HIL（Architect）

1. GPIO18 松开/按下电平、GPIO25 在 U3 开/关时的电平（数字读 + 启动 ADC mV），校准 `senseOnMv`（ADR-0015 §4）。
2. 纯电池：短按开机是否需要固件补按（B1 超时 5 s 内）；长按 2.5 s 关机全程耗时；关机后 3V3 实测为 0。
3. U3 无 MCU 参与的硬关断：固件挂死（控制台 `crash` 类命令或断开 GPIO4 驱动的版本）下长按 10 s 是否断电，记录时间。
4. 关机状态插 USB → 充电待机（无 BT 广播、GNSS 不上电、`power_on=charge`）；待机长按 → 正常开机；正常运行中 `power off`
   → U3 关、重启进充电待机；拔 USB 后整机断电；再按键能开机。
5. 关机前修改白名单/设置，重新开机后保持（落盘验证）。
6. 蓝牙连接中长按关机：手机侧断开、无异常日志。

## 5 ADR 待改（Architect 合入时）

- ADR-0015 §2.4 补启动分支与 USB 关机实测；§4 写 U3 按压时间实测；§5 B2 状态改“完成”。

## 6 用户决定（2026-11-21）

- **Q1 关机状态插 USB**：进入“充电待机”（原理图注释意图），仅 ESP32 运行，只有长按才开机。
- **Q2 长按关机**：2.5 s；长按只向电源管理发关机请求，由电源管理执行保存配置等关机流程（参考 SparkFun，§1.4）。
- **Q3 有 USB 时关机**：关 U3 + 重启进入充电待机。
