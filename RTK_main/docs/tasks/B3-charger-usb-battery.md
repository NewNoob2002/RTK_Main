# B3：P2 充电芯片探测、GPIO27 USB 检测与电池电压/低电关机

- 依据：ADR-0015（板级配置、电源，§2.4 关机序列、§3 引脚表）、`docs/tasks/B2-power-key-shutdown.md` 与 `docs/reports/B2-power-key.md`
  （关机仲裁 `PowerRequests`、`kQuiesce`，不得回退 B2 第二轮修复 K1/S1/X2）；ADR-0011（内存）；`AGENTS.md`、`docs/CODING-STYLE.md`。
- 背景：`docs/reports/P2-legacy-gap.md` §2.3、§3 P0-1/P0-3/P0-4。新 P2 v1.41（带 F9P）实板开机报
  `charger charge params apply failed (io-error)` 与 `charger REG08 read failed, falling back to STAT pin`（I2C 0x1A NACK）；
  旧板（CM680）上同一代码读到 `reg08=0x54`。
- 参考（只读）：旧固件 `../../P2_V3.1_IDF/main/user_src/user_tools.c:160-205`（SGM41513 探测/参数）、`user_battery.c`（ADC/电量/低电关机）、
  `../include/xyz_iic.h`、`xyz_iic.c`（400 kHz、内部上拉）、`main/main.c:110-220`、`main/user_include/user_gpio.h:21`（GPIO27）。
  原理图 `docs/hardware/P2/schematic/p2v1.41.pdf`。
- 基线：main `1b01eef`。分支 `feat/b3-charger`，工作树 `../RTK_Main-b3`（仓库根），构建前缀 `builds/b3-*`。
- 执行者：Codex（gpt）。评审：Codex（neon）。执行者不提交、不烧录、不连串口；HIL 由 Architect 做。

## 1 范围

1. **I2C 诊断命令**（DEV 档，`power` 命令组）：`power i2c scan` 扫 I2C0 的 0x08–0x77（`i2c_master_probe`，每地址 ≤ 10 ms），
   列出应答地址，末行 `OK`。与充电驱动共用同一总线句柄与互斥，不另建总线。
2. **充电芯片探测**：`charger::init()` 后先 `i2c_master_probe(0x1A)`；
   - 无应答：一行 W `charger: no ack at 0x1A, i2c path disabled`，之后不再发 I2C 事务、不再刷错误日志（参数写入与 REG08 读取全部跳过）；
   - 有应答：写入充电参数 **REG02 = 0x34（1980 mA）、REG03 = 0xAA（预充/终止 120/120 mA）**（用户 2026-11-23 选定，与旧 P2 V3.1 一致），
     替换 `BoardProfile.cpp` 中 51 / 0xF8；写后回读校验，不一致一行 W。
   - I2C0 开启 ESP32 内部上拉（与旧固件一致；板上 4.7 kΩ 外部上拉并联无害），速率保持 100 kHz（OLED 速率留给 OLED 卡定）。
3. **USB 在位检测改走 GPIO27 `VBUS_DET_ESP32`**（高有效，P2 profile `usbDetectGpio = 27`；先核对原理图与旧固件的上下拉配置）：
   - USB 在位判定优先级：GPIO27 → I2C REG08（仅芯片在线时）→ GPIO2 STAT（仅前两者都不可用时）；`power status` 显示 `usb_src=gpio|i2c|stat`；
   - 启动判定 `power_on=charge`、插 USB 关机走充电待机（ADR-0015 §76 现行行为不变）、电池满时 STAT 为高导致的误判由此消除；
   - GPIO27 只作输入，不改其他引脚；E1 profile 不受影响（`-1` 保持）。
4. **电池电压与电量**（GPIO34 `BAT_V_ADC`，ADC1，`esp_adc` oneshot + 校准方案，按旧固件分压系数）：
   - 采样周期与滤波按旧固件（不快于 1 s，取中值或滑动平均，说明选择）；电压 → 百分比沿用旧固件曲线/表，放无 IDF 头的纯函数；
   - 结果写入现有数据层（`AppData` 或等价），蓝牙 `0x03` 上报真实 `batteryPct` 与 `onBattery`（`DeviceService.cpp`/`Reply.hpp` 现恒为 `0xFF`/1）；
   - **低电关机**：仅电池供电（USB 不在位）且电量 < 10% 持续 ≥ 30 s（防负载瞬降误判）→ 经 B2 现有关机仲裁发起正常关机，一行 W 记录电压；
     USB 在位或 OTA 上传中按 B2 仲裁规则处理（上传中关机会先中止上传，与长按一致）。
   - `power status` 增加 `bat_mv=… bat_pct=…`。
5. **硬件版本号**：`DeviceService.cpp:215` 的 `hwVer="V1.1.0"` 改为取当前 `BoardProfile`（P2 v1.41 → `"V1.41"` 或 profile 中的字符串，说明格式），
   E1 测试板给对应值。
6. **日志**：启动时一行 I 汇总 `charger=ok|absent usb=0|1 src=… bat=…mV …%`；运行期只在状态变化时打 I。

## 2 不做

- OLED、蜂鸣器/马达/LED、4G；充电待机的指示方式（ADR-0015 现行“插 USB 关机 → 重启进充电待机”保持不变，待用户定）；
  改 `partitions.csv`、`sdkconfig.defaults`、ADR 文本（偏离写进报告，由 Architect 改 ADR）。

## 3 测试（执行者）

- Host（普通 + ASan/UBSan）：电压→百分比表边界与单调性；低电判定（持续时间、USB 插入取消、回绕）；USB 来源优先级组合（gpio 有/无 × i2c 在线/离线 × stat）；
  探测失败后不再发 I2C 事务（注入计数）；参数写入/回读不一致。
- IDF 构建：默认、`CONFIG_RTK_TRACE_ENABLE=n`、`RTK_WIFI_CONFIG_ENABLE=n`、PRODUCTION 与 NONE 档，全部 0 warning。
- 全量门禁 `bash tools/ci/run_gates.sh --prefix b3` 全过（含 doc_check、clang-format、frozen 校验）。
- 报告 `docs/reports/B3-charger-usb-battery.md`：改动文件、DRAM/IRAM/bin 差值、门禁逐项、对 ADR 的偏离（供 Architect 改 ADR-0015 §3 引脚表与参数）、
  未决问题、HIL 步骤（见 §4）。

## 4 HIL（Architect，新 P2 v1.41 + F9P）

1. `power i2c scan`：记录应答地址（预期 0x1A 充电芯片、0x3C OLED；若 0x1A 不在，记录实际地址并停在此项向用户报告）。
2. 开机日志无 I2C 错误刷屏；芯片在线时 `power charger` 回读 REG02=0x34、REG03=0xAA。
3. 插/拔 USB：`power status` 的 `usb` 随之变化，`usb_src=gpio`；电池满时插 USB 关机仍进充电待机。
4. `bat_mv` 与万用表读数误差 ≤ 50 mV；手机 App 显示电量。
5. 低电关机：可用可调电源模拟电池 < 10% 阈值，30 s 后正常关机；插 USB 后不关机。
