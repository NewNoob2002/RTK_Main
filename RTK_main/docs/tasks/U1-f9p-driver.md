# U1：u-blox ZED-F9P 驱动补全（VALSET 头修正、NMEA 输出、星座/截止角）

- 依据：ADR-0010（GNSS）、ADR-0012（接收机驱动：家族方言表、探测、输出计划、主机测试要求）、ADR-0018（设置服务）、`AGENTS.md`、`docs/CODING-STYLE.md`。
- 背景：`docs/reports/P2-legacy-gap.md` §1.4、§3 P0-2。P2 新板贴 ZED-F9P；`UbloxReceiver` 至今未上板（`components/gnss/Kconfig:8`）。
- 参考（只读，**键 ID、数值、顺序须注明出处 文件:行**）：
  - SparkFun RTK Everywhere `../../SparkFun_RTK_Everywhere_Firmware/Firmware/RTK_Everywhere/GNSS_ZED.ino`
    （NMEA 关闭 :450-480、MINELEV :603 与 :2110、`setConstellations()` :2004、UART1 波特率 :924/:1965）与其 `GNSS_ZED.h`；
  - 旧 P2 `../../P2_V3.1_IDF/main/user_src/user_f9p.c`（P2 板上 F9P 实际用的口、波特率与输出集）；
  - u-blox F9 HPG 接口说明（UBX-CFG-VALSET/VALGET 帧格式、CFG-SIGNAL/CFG-MSGOUT/CFG-NAVSPG 键）。SparkFun 与本卡冲突时以 u-blox 文档为准并在报告说明。
- 基线：main `1b01eef`。分支 `feat/u1-f9p`，工作树 `../RTK_Main-u1`（仓库根），构建前缀 `builds/u1-*`。
- 执行者：AGY。评审：Codex（gpt）。执行者不提交、不烧录、不连串口；HIL 由 Architect 做。

## 1 范围

1. **VALSET 头修正**：UBX-CFG-VALSET 载荷头为 4 字节（version、layers、reserved[2]）。`components/gnss/src/drivers/UbloxWire.hpp:40`
   `kValsetHeadBytes = 3U` 与 1 参 `beginValset`（`UbloxUbx.cpp:94-98`，被 `cmdConfigureReceiver` `UbloxUbx.cpp:277` 使用）写 3 字节头，修正为 4 字节；
   统一 1 参 / 2 参两条路径（保留一条实现），主机测试按字节比对完整帧（含校验和）。
2. **NMEA 输出计划**：在 F9P 接 ESP32 的口（先由旧固件与原理图核实是 UART1 还是 UART2，ADR-0012 §2 现写 UART1=1）上：
   显式关闭出厂默认的 GLL/VTG/GSV 等计划外语句，只开 ADR-0010 输出计划里的语句与速率；USB/I2C/SPI 口不必动（说明理由）。
3. **星座与截止角**：`components/gnss/src/PppSequence.cpp:42` `supportsSatMask(kZedF9p)` 改为 true，并为 F9P 实现方言：
   - 星座 GPS/BDS/GLO/GAL/SBAS（及 QZSS 随 GPS，按 u-blox 约束说明）→ `CFG-SIGNAL-*_ENA` 键，一次 VALSET（RAM+BBR）下发，接收机会重启跟踪，等待/ACK 处理按 SparkFun；
   - 截止角 → `CFG-NAVSPG-INFIL_MINELEV`（0–90°，越界拒绝）；
   - 与 UM98x 共用现有设置键 `gnss.const.*`/截止角（`components/settings/src/Registry.cpp`、`main/modules/gnss/GnssModulePpp.cpp:64,101`、
     `GnssModuleProbe.cpp:345`），蓝牙 `0x06` 与网页配置对 F9P 生效；不改设置键名与存储格式。
4. **MON-VER / 序列号**：`readMonVer` 解析固件版本与扩展字段；板卡序列号取 UBX-SEC-UNIQID（若 ADR-0012 的 `boardSn` 字段需要），读不到留空不报错。
5. **ACK/NAK 与超时**：每条 VALSET 等 ACK-ACK/NAK（≤ 1 s），NAK 记一行 W 含键 ID，失败不阻塞后续步骤（与 UM98x 现有容错一致）。

## 2 不做

- 基站模式/Survey-in、RTCM 输出、NTRIP、ZDA/VTG/GLL 蓝牙直推（另卡）；UM98x/K803 行为变更（只允许共用代码的必要重构，须主机回归覆盖）；
  改 ADR 文本、冻结文件（偏离写报告）。

## 3 测试（执行者）

- Host（普通 + ASan/UBSan）：VALSET 帧字节级（1 参/2 参、多键、校验和）；NMEA 关闭帧集合；星座组合 → 键值表（全开/全关/仅 GPS/含 SBAS）；
  截止角边界 0/90/91；ACK/NAK/超时状态机；MON-VER/SEC-UNIQID 解析（截断、未知扩展）；UM98x 既有用例全部不回退。
- IDF 构建：默认、`CONFIG_RTK_TRACE_ENABLE=n`、`RTK_WIFI_CONFIG_ENABLE=n`、PRODUCTION 与 NONE 档，全部 0 warning。
- 全量门禁 `bash tools/ci/run_gates.sh --prefix u1` 全过。
- 报告 `docs/reports/U1-f9p-driver.md`：改动文件、每个键 ID 的出处、DRAM/IRAM/bin 差值、门禁逐项、偏离、未决问题、HIL 步骤（见 §4）。

## 4 HIL（Architect，新 P2 v1.41 + F9P）

1. 冷启动探测到 `ZED-F9P`，切速后输出计划内语句，抓 60 s 串口无 GLL/VTG/GSV。
2. 网页/蓝牙改星座（如关 GLO）与截止角 15°，重启后保持；`gnss` 状态里卫星数随之变化。
3. 手机转发 RTCM → 固定解（有基站时）；RTCM 通路无丢字节回退。
