# U2：u-blox ZED-F9P 驱动能力差距分析与 P0 设计方案

- **日期**：2026-11-24
- **范围**：只读调研（不修改仓库文件、不提交/烧录/接串口）
- **被测对象**：`RTK_main` 分支 `feat/u1-f9p`（已在 P2 v1.41 + ZED-F9P `HPG 1.12` 上完成 U1 HIL 验证，`docs/reports/U1-f9p-driver.md:272-285`）
- **主产品级参考**：SparkFun RTK Everywhere 固件（基于 SparkFun u-blox GNSS v3 3.1.14，`GNSS_ZED.ino`、`GNSS_ZED.h`、`GNSS.h`、`menuGNSS.ino`、`settings.h`）
- **协议级参考**：SparkFun u-blox GNSS v3 3.1.15（MIT，`src/u-blox_GNSS.{h,cpp}`、`u-blox_config_keys.h`、`u-blox_Class_and_ID.h`、`u-blox_external_typedefs.h`、`u-blox_structs.h`）
- **依据文档**：`RTK_main/AGENTS.md`、`docs/adr/ADR-0010-gnss.md`、`docs/adr/ADR-0012-gnss-receiver-drivers.md`、`docs/adr/ADR-0018-settings-service.md`、`docs/reports/U1-f9p-driver.md`、`docs/frozen.sha256`
- **事实性标注约定**：
  - `[verified-in-source]`：已直接阅读对应仓库源码核实。
  - `[from-doc]`：已直接阅读 u-blox 官方 PDF 手册或仓库 ADR/报告核实。
  - `[assumption]`：基于协议常识或典型载荷的工程估算/推断（含明确标注“未核实”的边界）。

---

## 1. 能力对比表（RTK Everywhere 产品层 vs SparkFun v3 协议层 vs RTK_main）

产品背景：`RTK_main` 当前核心场景为 **RTK 流动站/基站（UART1 连 ESP32 UART2 RX16/TX17，目标波特率 460800，手机 BT SPP 输入 RTCM3，GNSS 输出 NMEA 经 `stream_demux` 解析并转发，见 `ADR-0010`、`BoardProfile.cpp:28`）**。

> 注：RTK Everywhere 在硬件架构上走 **I2C 控制 + UART1 数据/NMEA + UART2 电台/修正**（`GNSS_ZED.ino:131,374-416,2360-2380` `[verified-in-source]`），而本工程单用 **UART1** 复用 UBX 控制 + NMEA/RTCM3 数据（`BoardProfile.cpp:28` `[verified-in-source]`）。

| # | 功能项 | RTK Everywhere (产品层, `file:line`) | SparkFun v3 3.1.15 (协议层 API + Keys/UBX, `file:line`) | 本工程 `RTK_main` (`file:line` 或 missing) | 与本产品相关性（UART1 Rover/Base, BT RTCM 入, ADR-0010 NMEA 出） | 优先级 |
|---|---|---|---|---|---|---|
| 1 | **导航/测量频率**（`CFG-RATE-MEAS`/`NAV`、型号与 HPG 上限） | `GNSS_ZED.ino:853-873,2513-2591`：`fixRateIsAllowed` 上限 `f9pMaxRateHz=20.0` Hz、下限 `0.00012` Hz（`measRate<=65000`×`navRate<=127`）；默认 `measurementRateMs=250`（4 Hz，`settings.h:877`）；基站模式强制覆写 1 Hz（`GNSS_ZED.ino:2524-2528`）；改率时同步重算 GSV 频次至 1 Hz 防刷爆 SPP（`GNSS_ZED.ino:2559-2574`）；`menuGNSS.ino:28-34,263-295` `[verified-in-source]` | `setNavigationFrequency`/`setMeasurementRate`/`setNavigationRate`（`u-blox_GNSS.cpp:17977-18079`，`u-blox_GNSS.h:1123-1129`）；下限 `measRate>=25` ms（40 Hz），`navRate` 1..127；键 `UBLOX_CFG_RATE_MEAS=0x30210001`、`UBLOX_CFG_RATE_NAV=0x30210002`（`u-blox_config_keys.h:1145-1146`） `[verified-in-source]` | **部分实现（存在 P0 缺口）**：`UbloxWire.hpp:74` 定义 `kKeyRateMeas=0x30210001`；`UbloxUbx.cpp:97,125` 在 `roverStep(0)` 写入 `CFG-RATE-MEAS`；**未写 `CFG-RATE-NAV`**（依赖默认 1）；**未覆盖 `Receiver::navRatesHz()`**（`Receiver.cpp:13-16` 返回空），导致 `gnss.rate` 在网页 `caps` 缺失且控制台/WS 报 `unsupported`；`UbloxReceiver.cpp:228` 仍标 `cap::kRate20Hz` 与 4 星座 RTK 实际 7–8 Hz 上限不符 `[verified-in-source]` | **核心必须**：直接决定流动站定位刷新率、网页/控制台 `gnss.rate` 可用性以及所有 NMEA 输出周期分母 | **P0** |
| 2 | **逐报文 NMEA/UBX 输出频率**（`CFG-MSGOUT-*_UART1`） | `GNSS_ZED.h:61-238` 定义约 90 条 `ubxMessages[]` 含 `msgDefaultRate` 与 HPG 最低版本（默认开 NMEA 5 句：GGA/GSA/GST/RMC=1、GSV=4；其余 NMEA 置 0）；`GNSS_ZED.ino:450-477` 关 I2C/UART2/SPI 全部 NMEA；`GNSS_ZED.ino:2208-2351` 按 Class 分批 VALSET 下发并校验 NTRIP/Tilt 必需句；`settings.h:1138` `[verified-in-source]` | 通用 `setVal8`/`addCfgValset8` 写 `UBLOX_CFG_MSGOUT_*`（`u-blox_GNSS.cpp:10209,10414`）；键表 `u-blox_config_keys.h:268-315`（GGA `0x209100bb`、RMC `0x209100ac`、GSA `0x209100c0`、GST `0x209100d4`、GSV `0x209100c5`）；UBX 类提供 `setAuto*rate()`（如 `u-blox_GNSS.cpp:11640`） `[verified-in-source]` | **部分实现（存在 P0 缺口）**：`UbloxReceiver.cpp:333-402` 已实现 `outputMask`（GGA/RMC/GSA/GST/GSV）与 `buildOutput`（按 `round(periodMs*rateHz/1000)` 写 `CFG-MSGOUT-*`，UART2=`key+1`）；`UbloxWire.hpp:77-98` + `UbloxExt.cpp:232-249` 已在启动步静默 12 条标准 NMEA；**但未覆盖 `Receiver::outputPeriodsMs()`**（`Receiver.cpp:18-22` 返回空），导致 `gnss.out.*` 全不可配置（HIL `config set gnss.out.gsv 0` 返回 `unsupported`） `[verified-in-source]` | **核心必须**：ADR-0010/0012/0018 闭环要求；且 F9P 按历元分频（`period = n × T_nav`），必须过滤非整倍周期 | **P0** |
| 3 | **波特率与端口协议入/出掩码** | `GNSS_ZED.ino:374-428` 批量配置 UART1/UART2/SPI/I2C/USB 全部 `INPROT`/`OUTPROT`（关 SPI 降负载）；`GNSS_ZED.ino:1942-1985,3003-3022` 支持 4800..921600 波特率（默认 dataPort 230400、radio 57600，`settings.h:1100-1104`），UART2 切速带渐进 `maxWait`（1100→5000 ms）；`setExternalCorrections`（`GNSS_ZED.ino:2047-2108`）按来源切换 UART2INPROT `[verified-in-source]` | `setSerialRate`（`u-blox_GNSS.cpp:8248-8256`）、`setUART1Output`/`setUART2Output`（`:8268-8285`）、`setUART1Input`/`setUART2Input`（`:8316-8335`）；键 `UBLOX_CFG_UART1_BAUDRATE=0x40520001`、`CFG-UART1{IN,OUT}PROT-*`（`u-blox_config_keys.h:1364-1383`） `[verified-in-source]` | **已实现**：`UbloxUbx.cpp:93-145,289-302` 实现 UART1/UART2 `INPROT-RTCM3X=1`、`OUTPROT-NMEA=1`、`OUTPROT-RTCM3X`（基站）及 `baudStep`（RAM 层，不期望 ACK，主机切速后重探验证）；`GnssKeys.hpp:25-35` 支持 9600..921600，HIL 已验证 115200→460800 `[verified-in-source]` | **核心已有**：满足单 UART1 流动站/基站需求；可选 P2 显式关闭未用 SPI/I2C 输出以略降 F9P 负载 | **skip**（已满足） |
| 4 | **ACK/NAK 匹配与重试/超时** | `GNSS_ZED.h:243` `MAX_SET_MESSAGES_RETRIES=5`；`GNSS_ZED.ino:1973-1981` 动态拉长 `maxWait`（1100→5000 ms）；`GNSS_ZED.ino:2221,2278-2292` 分批 VALSET 失败回退重试 3 次；`GNSS_ZED.ino:2985-2988` 3 次重试；无迟到 ACK 静默窗 `[verified-in-source]` | 阻塞 `waitForACKResponse`（`u-blox_GNSS.cpp:5753-5932`），默认 `kUBLOXGNSSDefaultMaxWait=1100` ms（`u-blox_external_typedefs.h:59`），按发送帧 `(cls,id)` 通用匹配 `UBX-ACK-ACK`/`NAK`（`u-blox_GNSS.cpp:1881-1911,3342-3366`），无内置自动重试 `[verified-in-source]` | **已实现（非阻塞更优，有小缺口）**：`ControllerConfigure.cpp:104-184,309-385`、`ControllerMaintenance.cpp:208-265`、`UbloxExt.cpp:474-546` 实现非阻塞超时（500 ms，save 1800 ms，sat 1000 ms）、重试、**300 ms VALSET 重同步窗**、在途键日志与静默步非致命跳过；**小缺口**：`UbloxReceiver::classifyReply()`（`UbloxReceiver.cpp:311-315`）硬编码只匹配 `CFG-CFG`/`CFG-VALSET`，若未来发其他 `CFG-*` 需泛化；500 ms 在高负载 RTK 下偏紧 `[verified-in-source]` | **高相关**：所有配置可靠性基础；泛化 `classifyReply` 成本极低（数行代码） | **P1**（泛化匹配与超时复核） |
| 5 | **VALSET 存储层（RAM/BBR/FLASH）与保存（`CFG-CFG`）** | 几乎全部运行期 VALSET 直接写 `VAL_LAYER_ALL`（RAM+BBR+FLASH，`GNSS_ZED.ino:292,369,2020,2555`），注释明确已直写三层无需再调 `saveConfiguration`（`GNSS_ZED.ino:619`）；也保留 `saveConfiguration()` 调 `UBX-CFG-CFG`（`:1990-1997`） `[verified-in-source]` | `VAL_LAYER_RAM=1`、`BBR=2`、`FLASH=4`、`RAM_BBR=3`、`ALL=7`（`u-blox_config_keys.h:59-63`）；`newCfgValset`/`addCfgValset*`/`sendCfgValset`（`u-blox_GNSS.cpp:10310-10480`，上限 64 键）；`saveConfiguration`/`saveConfigSelective` 发 12 B `UBX-CFG-CFG`（`u-blox_GNSS.cpp:8445-8484`） `[verified-in-source]` | **已实现（层策略存在轻微分叉）**：按 ADR-0010 §2.4 不主动写模块 Flash（防磨损）；`UbloxUbx.cpp:223-233` `saveStep()` 发 13 B `UBX-CFG-CFG`（`saveMask=0x01`, `deviceMask=0x03` BBR\|Flash，1800 ms 超时）；注意 `cmdConfigurePort`/`baudStep`/`buildOutput(useConfiguredPort=true)`/`modeStep` 写 **RAM**，而 `stepSilenceNmea(useConfiguredPort=true)` 写 **RAM**，`buildSatTrackValset` 写 **RAM\|BBR**（`UbloxExt.cpp:330`） `[verified-in-source]` | **中相关**：本工程持久化真源在 ESP32 NVS，开机重放；仅 `gnss save` 才落盘模块 Flash，比 RTK Everywhere 的 `VAL_LAYER_ALL` 更保护模块 Flash 寿命 | **P2**（统一文档/层约定） |
| 6 | **恢复出厂（`CFG-CFG` clear+load / `factoryReset`）** | `GNSS_ZED.ino:704-720`：`factoryReset()` 发 13 B `CFG-CFG` clear+load（`devBBR\|devFlash`）→ `delay(2000)` → 再发 `saveMask=0xFFFF` → `hardReset()`（`UBX-CFG-RST`） `[verified-in-source]` | `factoryReset()`（`u-blox_GNSS.cpp:8382-8389`：13 B `CFG-CFG` clear + `hardReset()`）；`factoryDefault()`（`:8426-8439`：12 B `CFG-CFG` clear+load，等 ACK） `[verified-in-source]` | **已实现**：`UbloxUbx.cpp:262-287` `factoryResetStep()` 发 13 B `CFG-CFG` clear+load（`0x03` BBR\|Flash，`expectReply=false`，切主机波特率至出厂 `38400`），UART2 需 HPG≥1.32 门控；`buildFactoryResetProbe` 支持 UART2 先恢复 UBX 输出再探 `MON-VER` `[verified-in-source]` | **高相关**：维护命令 `gnss factory-reset` 已闭环（仅未发 `hardReset`，依赖 clear+load 即时重建 RAM；实板复测确认是否需要 `CFG-RST`） | **P2**（实板复核） |
| 7 | **`CFG-VALGET` 配置读回** | `GNSS_ZED.ino:1097-1122` 用 `getVal8` 读 `CFG-TMODE-MODE`/`POS_TYPE` 判断当前工作模式，避免重复写配置 `[verified-in-source]` | `newCfgValget`/`addCfgValget`/`sendCfgValget`、`getVal8/16/32/64`（`u-blox_GNSS.cpp:10544-10683`，`u-blox_GNSS.h:485-618`）；注意 VALGET 层号与 VALSET 不同：`RAM=0, BBR=1, FLASH=2, DEFAULT=7`（`:10578-10595`） `[verified-in-source]` | **missing**：`UbloxReceiver` 未覆盖 `buildReadback()`/`matchReadback()`，沿用基类返回 `false`/`0`（`Receiver.cpp:159-168`、`TestGnssReadback.cpp:680`）；控制台 `gnss config` 在 F9P 上报不支持 `[verified-in-source]` | **中高相关**：用于控制台 `gnss config` 诊断读回（查当前 `RATE_MEAS`、`MSGOUT`、`MINELEV`、`SIGNAL`、`TMODE`、`HIGHPREC`），极大方便现场排障 | **P1** |
| 8 | **动态模型（`CFG-NAVSPG-DYNMODEL`）** | `GNSS_ZED.ino:2490-2498` `setModel` 写 `UBLOX_CFG_NAVSPG_DYNMODEL`（`VAL_LAYER_ALL`）；`menuGNSS.ino:42-92,338-341` 支持 Portable/Stationary/Pedestrian/Automotive/Sea/Airborne1g/2g/4g/Wrist/Bike/Mower/E-Scooter；NTP 模式自动设 Stationary（`:575`） `[verified-in-source]` | `setDynamicModel`/`getDynamicModel`（`u-blox_GNSS.cpp:9024-9039`）；键 `UBLOX_CFG_NAVSPG_DYNMODEL=0x20110021`（`u-blox_config_keys.h:1026`） `[verified-in-source]` | **missing**：未定义 `0x20110021`，依赖 F9P 出厂默认 `PORTABLE (0)` `[verified-in-source]` | **中相关**：测绘手持/三脚架静态/车载场景有收益（如 Stationary vs Pedestrian vs Portable）；但当前网页/手机协议无对应设置键 | **P1** |
| 9 | **星座与频点信号（`CFG-SIGNAL-*`）** | `GNSS_ZED.h:28-38` 表驱 6 星座（BDS/GAL/GLO/GPS/QZSS/SBAS）+ 最低 HPG 版本 + 附属信号键（`QZSS_L1CA`）；`GNSS_ZED.ino:1585-1596` 强制 GPS 与 QZSS 联动防互相关；`:2004-2043` 逐星座发 VALSET `[verified-in-source]` | `enableGNSS`/`isGNSSenabled`（`u-blox_GNSS.cpp:9119-9155`）；键 `UBLOX_CFG_SIGNAL_*_ENA`（`u-blox_config_keys.h:1239-1262`） `[verified-in-source]` | **已实现（U1 完成并 HIL 通过）**：`UbloxWire.hpp:104-111`、`UbloxExt.cpp:321-344` 单帧原子 VALSET 写 `GPS`+`GPS_L1CA`+`QZSS`+`QZSS_L1CA`+`BDS`+`GLO`+`GAL`（+版本门控 `SBAS`），强制 GPS=1（拒 `gps=0`），`SatMaskSequence` 含 ACK 后 600 ms 非阻塞稳定期（`PppSequence.cpp:112-234`） `[verified-in-source]` | **核心已有**：网页 `gnss.const.*` 与手机 `0x06` 已在 P2 v1.41 + HPG 1.12 HIL 全通过（`U1-f9p-driver.md:280-282`） | **skip**（已完成） |
| 10 | **高度角截止掩码（`CFG-NAVSPG-INFIL_MINELEV`）** | `GNSS_ZED.ino:2110-2118` `setElevation` 写 `UBLOX_CFG_NAVSPG_INFIL_MINELEV`；默认 `10°`（`settings.h:1082`）；`menuGNSS.ino:208-210,398-402` 范围 0–90° `[verified-in-source]` | 通用 `setValSigned8`/`getValSigned8`（`u-blox_GNSS.cpp:10214`）；键 `UBLOX_CFG_NAVSPG_INFIL_MINELEV=0x201100a4`（I1，`u-blox_config_keys.h:1041`）；无专用包装函数 `[verified-in-source]` | **已实现（U1 完成并 HIL 通过）**：`UbloxWire.hpp:102-103`、`UbloxExt.cpp:331` 与星座同帧原子写入；接入 `gnss.cutoff`（0–90°，网页/控制台/手机 `0x06` 同步并 NVS 持久化，`U1-f9p-driver.md:280,283`） `[verified-in-source]` | **核心已有**：已在 P2 v1.41 + HPG 1.12 HIL 验证通过 | **skip**（已完成） |
| 11 | **最小信噪比门限（`CFG-NAVSPG-INFIL_MINCNO`）** | `GNSS_ZED.ino:2472-2485` `setMinCN0` 写 `UBLOX_CFG_NAVSPG_INFIL_MINCNO`；默认 `6 dBHz`（`settings.h:1079`）；`menuGNSS.ino:212-213,407-412` 范围 0–60 dBHz `[verified-in-source]` | 通用 `setVal8`；键 `UBLOX_CFG_NAVSPG_INFIL_MINCNO=0x201100a3`（U1，dBHz，`u-blox_config_keys.h:1040`、`UBX-22008968 §6.9.14`） `[verified-in-source]` `[from-doc]` | **missing**：未配置，使用 F9P 出厂默认 `6 dBHz` `[verified-in-source]` | **中低相关**：树荫/城市峡谷弱信号剔除有辅助价值，但优先级低于核心率/高精度 NMEA | **P2** |
| 12 | **SBAS 控制**（`CFG-SIGNAL-SBAS_ENA` + `CFG-SBAS-USE_*`） | `GNSS_ZED.h:36` 标注 `f9pFirmwareVersionSupported=113`（注释："v1.12 ZED-F9P firmware does not allow for SBAS control"）；通过 `setConstellations` 控制开关；未单独调 `CFG-SBAS-USE_*` `[verified-in-source]` | `enableGNSS(..., SFE_UBLOX_GNSS_ID_SBAS)`（`u-blox_GNSS.cpp:9123,9137`）；细粒度键 `UBLOX_CFG_SBAS_USE_{TESTMODE,RANGING,DIFFCORR,INTEGRITY,IONOONLY}`（`u-blox_config_keys.h:1169-1174`） `[verified-in-source]` | **已实现基本开关（带 HPG 版本门控）**：`UbloxWire.hpp:65,107`、`UbloxExt.cpp:315-319,337-339` 在 HPG≥1.13 写入 `CFG-SIGNAL-SBAS_ENA`，HPG 1.12 自动屏蔽（`U1-f9p-driver.md:281-282`）；网页暂无独立 SBAS 键（仅经手机 `0x06`/`gnss.sat_track` 控制）；未配 `CFG-SBAS-USE_*` `[verified-in-source]` | **中相关**：RTK 模式下通常建议关 SBAS 防短断差分跳变（ArduSimple/u-blox 建议）；HPG 1.12 硬件本身不支持 SBAS 控制，已正确门控 | **P2**（仅后续考虑网页独立键） |
| 13 | **NMEA 高精度模式 / NMEA 版本 / 扩展星号** | `GNSS_ZED.ino:442-443` 在基础配置中无条件开启 `UBLOX_CFG_NMEA_HIGHPREC=1` 与 `UBLOX_CFG_NMEA_SVNUMBERING=1`；未改 `PROTVER`（沿用出厂默认） `[verified-in-source]` | `setHighPrecisionMode`（`u-blox_GNSS.cpp:16938-16941`）写 `UBLOX_CFG_NMEA_HIGHPREC=0x10930006`（`u-blox_config_keys.h:1066`）；`setMainTalkerID`（`:16932`）；`UBLOX_CFG_NMEA_PROTVER=0x20930001`（`:1061`，值 21/23/40/41/42）；`UBLOX_CFG_NMEA_SVNUMBERING=0x20930007`（`:1067`） `[verified-in-source]` | **missing（严重缺口！）**：`UbloxWire.hpp` 未定义任何 `CFG-NMEA-*` 键！`UBX-18010802 §3.1.4.5` 明确说明 `CFG-NMEA-HIGHPREC` **出厂默认关闭**，经纬度仅 5 位小数（~1.8 cm 量化步长），**不足以表达厘米级 RTK 定位**！而我方 `NmeaParserNumeric.cpp:40,55` 已原生支持 7 位小数经纬度与毫米级高程 `[verified-in-source]` `[from-doc]` | **极高相关（RTK 精度硬伤）**：本工程按 ADR-0010 经 NMEA 交付 RTK 位置给手机/测绘软件，未开 `HIGHPREC=1` 会直接截断厘米/毫米级 RTK Fixed 精度 | **P0**（`HIGHPREC=1` + `SVNUMBERING=1`）；**P1**（`PROTVER` 实板确认） |
| 14 | **基站 Survey-In / Fixed（`CFG-TMODE*`）与 RTCM3 输出（1005/1074/1084/1094/1124/1230）** | `GNSS_ZED.ino:80-119`（默认 1005/1074/1084/1094/1124=1, 1230=10；低速档 MSM4=2, 1005/1230=10）、`:500-540`、`fixedBaseStart`（`:745-847`，支持 ECEF/LLH + 天线高）、`surveyInStart`/`surveyInReset`（`:2831-2943`，**切 Fixed 或重启 Survey-In 前强制清旧 Survey-In 态**，防 F9P 抑制 RTCM 输出）、`setMessagesRTCMBase`/`Rover`（`:2356-2459` 一次性 60 键写 5 端口）、基站强制 1 Hz（`:2524-2528`） `[verified-in-source]` | `setSurveyMode`/`setSurveyModeFull`（`u-blox_GNSS.cpp:8489-8520`）、`setStaticPosition`（`:8527-8554`）；键 `UBLOX_CFG_TMODE_*`（`u-blox_config_keys.h:1298-1314`）、`UBLOX_CFG_MSGOUT_RTCM_3X_TYPE{1005,1074,1084,1094,1124,1230}_UART1`（`:345-395`）；`UBX-NAV-SVIN` 状态读取 `[verified-in-source]` | **部分实现（已编入代码，未做 F9P 基站 HIL，缺 2 项实战保护）**：`UbloxWire.hpp:114-136` + `UbloxUbx.cpp:30-66,313-404` 已实现 `modeStep`（`kRover`/`kBasePrep`/`kBaseSurvey`/`kBaseFixed`，支持 LLH 高精度 + 1005/1230/MSM4/MSM7 UART1/2 输出）；**缺口**：① 未在切 Fixed/重启 Survey-In 前清除旧 `NAV-SVIN` 活跃/有效态（RTK Everywhere 实踩陷阱）；② 基站未强制锁定 1 Hz（若流动站设 5 Hz 再切基站，`rtcmPeriodToEpochs` 按 5 Hz 算但 F9P 基站运行在 5 Hz 功耗/带宽激增且 `UBX-18010802` 建议 1 Hz）；③ 未解析 `UBX-NAV-SVIN` 进度（仅靠收到 RTCM 帧推演） `[verified-in-source]` | **高相关（第二阶段基站核心）**：U1 任务明确基站不在本轮范围（`U1-f9p-driver.md:3`），但基站上板前必须补齐上述 3 项 | **P1**（基站阶段必做） |
| 15 | **RTCM 输入状态（`UBX-RXM-RTCM` / `MON-COMMS`）** | `GNSS_ZED.h:228` 表中含 `RXM_RTCM`（默认 0）；实际主要用 `UBX-MON-COMMS` 回调监控 UART2 `rxBytes` 增长判定外部修正活跃度（`GNSS_ZED.ino:1882,2757-2795`） `[verified-in-source]` | `UBX_RXM_RTCM=0x32`（`u-blox_Class_and_ID.h:287`）、`UBLOX_CFG_MSGOUT_UBX_RXM_RTCM_UART1=0x20910269`（`u-blox_config_keys.h:650`）；v3 库未提供专用 `getRXMRTCM` 封装，需经通用回调/自定义解析 `[verified-in-source]` | **missing**：仅在主机侧统计 `rtcmPipe` 写入字节与帧边界（`GnssModule.cpp:470-484`），不知道 F9P 内部是否真正 CRC 通过并采用该 RTCM 帧 `[verified-in-source]` | **中高相关**：手机 BT 透传 RTCM3 时，若基站 ID 不匹配/CRC 坏/缺 1230 导致不固定，`UBX-RXM-RTCM`（`crcFailed`/`msgUsed`/`subType`/`msgType`）是最直接诊断依据 | **P1**（诊断增强） |
| 16 | **二进制导航报文（`NAV-PVT` / `HPPOSLLH` / `RELPOSNED`）** | `GNSS_ZED.ino:1859-1865` 在 I2C 开启 `NAV-PVT` + `NAV-HPPOSLLH` 自动回调（降采样至 1 Hz）供内部 UI/状态机/NTRIP 使用；`GNSS_ZED.h:119,124-125` 含 `RELPOSNED`（默认 0） `[verified-in-source]` | `getPVT`/`setAutoPVT*`（`u-blox_GNSS.cpp:11584-11667`）、`setAutoHPPOSLLH*`（`:12446-12487`）、`setAutoRELPOSNED*`（`:13658-13699`）；键 `UBLOX_CFG_MSGOUT_UBX_NAV_PVT_UART1=0x20910007`、`HPPOSLLH=0x20910034`、`RELPOSNED=0x2091008e`（`u-blox_config_keys.h:505,535,540`） `[verified-in-source]` | **missing（按架构有意不启用）**：ADR-0010 §2.5/§2.9 统一经 NMEA（GGA/RMC/GSA/GST/GSV）解析 `Fix`/`SatTable` 并透传手机；F9P 单天线无 `RELPOSNED` 场景 `[verified-in-source]` `[from-doc]` | **低相关**：启用 `CFG-NMEA-HIGHPREC=1` 后 NMEA 已具备 1e-9° / 1 mm 精度与 RTK 状态，无需增加 UBX 导航冗余带宽；`RELPOSNED` 仅双天线/Moving Base 需要 | **skip**（`RELPOSNED`）；**P2**（`PVT`/`HPPOSLLH` 仅当需亚毫秒时间或内部无 NMEA 场景） |
| 17 | **`MON-VER` 与 `MON-HW` / `MON-RF`（天线开/短路监控）** | `GNSS_ZED.ino:156-217` 调 `getModuleInfo(1100)` 识别 `FWVER`/`MOD`；`:371-372` 开启 `CFG-HW-ANT_CFG_SHORTDET/OPENDET=1`；`:1875-1879` 开启 `MON-HW` 回调更新 `aStatus`（`isAntennaShorted`/`Open`，`:1333-1344`） `[verified-in-source]` | `getModuleInfo`（`u-blox_GNSS.cpp:8632-8738`）轮询 `UBX-MON-VER`；`getMONHW`/`setAutoMONHW*`/`getHWstatus`（`:15294-15375,19648`）；`getRFinformation` 轮询 `UBX-MON-RF`（`:9186-9220`）；键 `UBLOX_CFG_MSGOUT_UBX_MON_HW_UART1=0x209101b5`、`MON_RF=0x2091035a`（`u-blox_config_keys.h:430,445`） `[verified-in-source]` | **`MON-VER` 已实现；`MON-HW`/`MON-RF` missing**：`UbloxReceiver.cpp:88-114,200-206,238-256` 完整实现 `MON-VER` 轮询、Fletcher 校验、严格 `ZED-F9P` 匹配与 `FWVER=` 解析（HIL PASS）；无天线短路/开路或 RF 干扰监控 `[verified-in-source]` | **中相关**：外接 RTK 天线开/短路检测在量产设备故障告警上很有价值（`UBX-18010802 §3.1.4.4` 推荐优先用 `UBX-MON-RF`），前提需核实 P2 v1.41 硬件是否接了 F9P `ANT_SHORT_N`/`ANT_DETECT` 电路（目前**未核实**） | **P1**（核实 P2 硬件后决定接入 `MON-RF`/`MON-HW`） |
| 18 | **芯片唯一序列号（`UBX-SEC-UNIQID`）** | `GNSS_ZED.ino:219-222` 开机 `getUniqueChipId` + `getUniqueChipIdStr` 填入 `gnssUniqueId` `[verified-in-source]` | `getUniqueChipId`/`getUniqueChipIdStr`（`u-blox_GNSS.cpp:9929-9990`），兼容 F9/M9 5 B（v1）与 M10 6 B（v2） `[verified-in-source]` | **已实现（U1 完成并 HIL 通过）**：`UbloxWire.hpp:23-24,60-63,142-145`、`UbloxExt.cpp:346-375`、`GnssModuleProbe.cpp:551-564`、`GnssModuleFrames.cpp:193-197`；HIL 实测读出 `3F41CA671D`（`U1-f9p-driver.md:278`） `[verified-in-source]` | **核心已有**：用于手机 `0x01` `boardSn` | **skip**（已完成） |
| 19 | **省电与软硬复位（`UBX-CFG-RST` / `RXM-PMREQ` / `CFG-PM-*`）** | `GNSS_ZED.ino:1892-1898` `reset()` 调 `softwareResetGNSSOnly()`（`CFG-RST` resetMode=`0x02`）；`:718` 工厂复位末尾调 `hardReset()`（`navBbrMask=0xFFFF, resetMode=0x00`）；`standby()` 留空 TODO（`:2648-2651`） `[verified-in-source]` | `hardReset`/`softwareResetGNSSOnly`/`softwareEnableGNSS`/`cfgRst`（`u-blox_GNSS.cpp:8391-8422`，`UBX_CFG_RST=0x04`）；`powerOff`/`powerOffWithInterrupt`（`:8899-8960`，`UBX-RXM-PMREQ`）；键 `UBLOX_CFG_PM_OPERATEMODE=0x20d00001`（`u-blox_config_keys.h:1099`） `[verified-in-source]` | **missing**：当前 `gnss reset` 仅通过 `Controller::stop`/`start` 开关电源脚重启（`GnssModuleCmds.cpp:319-332`）；但 **P2 v1.41 无 GNSS 电源开关脚**（`BoardProfile.cpp:28` 未设 `powerGpio`，默认为 `-1`），故 `gnss reset` 在 P2 上无法真正复位 F9P 芯片！ `[verified-in-source]` | **中相关**：P2 v1.41 缺少 GNSS 电源脚，`UBX-CFG-RST`（`0x06/0x04`）软件热/冷复位可补齐 P2 上真正复位 F9P 的能力；RTK 场景不宜开 PSM 省电（会破坏载波相位连续性） | **P2**（`CFG-RST` 软件复位）；**skip**（`CFG-PM` 省电） |
| 20 | **时间脉冲（1PPS `CFG-TP-*` / `TIM-TP`）与外部事件（`TIM-TM2`）** | `GNSS_ZED.ino:251-319` `beginExternalEvent`（`TIM-TM2` 相机/触发时间戳）与 `setPPS`（配置 `CFG-TP-*` 锁定后输出 1 Hz 脉冲）；`:577-609` NTP 模式配 1PPS + `TIM-TP` `[verified-in-source]` | 通用 `addCfgValset` 写 `UBLOX_CFG_TP_*`（`u-blox_config_keys.h:1321-1345`，例程 `Example23_TimePulse_*`）；`setAutoTIMTP*`/`setAutoTIMTM2*`（`u-blox_GNSS.cpp:1534,4643`、`u-blox_structs.h:2248`） `[verified-in-source]` | **missing** `[verified-in-source]` | **低相关**：当前产品定义（ADR-0010/0015）无 1PPS 授时或相机快门触发需求，保留 F9P 默认 1PPS 即可 | **skip** |
| 21 | **芯片内电子围栏（`CFG-GEOFENCE-*` / `NAV-GEOFENCE`）** | 未在 `GNSS_ZED.ino` 使用 `[verified-in-source]` | `addGeofence`（`u-blox_GNSS.cpp:8765`）；键 `UBLOX_CFG_GEOFENCE_*`（`u-blox_config_keys.h:144`） `[verified-in-source]` | **missing** `[verified-in-source]` | **不适用**：ADR-0018 §6 Q2 明确电子围栏由主机/4G MQTT 侧统一管理，需跨 Unicore/Optisens/u-blox 通用，不应耦合 u-blox 私有围栏 | **skip** |
| 22 | **惯导融合 / 航位推算（`UBX-ESF-*`）** | 仅用于 ZED-F9R（HPS）或外置 IM19 IMU 倾斜补偿（`GNSS_ZED.ino:2596-2645` 实为 IM19 外置倾斜，非 F9P 内置 ESF） `[verified-in-source]` | `getESFAutoAlignment`/`getESFALG`/`getESFSTATUS`/`getESFINS`/`setAutoESF*`（`u-blox_GNSS.cpp:9158-9174,15461-16150`）；`UBLOX_CFG_SFIMU_*`（`u-blox_config_keys.h:1209`） `[verified-in-source]` | **missing** `[verified-in-source]` | **不适用**：ZED-F9P 无内置 IMU，不支持 ESF（`UbloxReceiver.cpp:67-79` 已明确排除 ZED-F9R） | **skip** |
| 23 | **AssistNow A-GNSS（`UBX-MGA-*`）** | 未在 `GNSS_ZED.ino` 使用 `[verified-in-source]` | `pushAssistNowData`（`u-blox_GNSS.cpp:6812-6836`、`u-blox_GNSS.h:291-296`）；`UBX_CLASS_MGA=0x13`（`u-blox_Class_and_ID.h:192-217`）；F9P 仅支持 AssistNow Online（`UBX-17051259 §1.3 Table 6`） `[verified-in-source]` `[from-doc]` | **missing** `[verified-in-source]` | **低相关**：需联网与 u-blox Token，且当前 4G 模块尚未接入；冷启动 24–25 s 在 RTK 测绘场景可接受 | **skip**（4G 接入后再评估） |
| 24 | **SPARTN / PointPerfect (PPL) / L-Band PMP** | `GNSS_ZED.ino:14-75` `applyPointPerfectKeys()`（下发 `UBX-RXM-SPARTNKEY`、设 `CFG-SPARTN-USE_SOURCE`、`CFG-NAVHPG-DGNSSMODE=3`、开 `RXM-COR`）；`:194-200` 明确告警 HPG < 1.30 不支持 SPARTN；`:2976-2998` `updateCorrectionsSource`；`settings.h:329,1047` `[verified-in-source]` | `setDynamicSPARTNKey(s)`（`u-blox_GNSS.cpp:9289-9515`）、`parseSPARTN`（`u-blox_GNSS.h:474`）、`setRXMPMPcallbackPtr`（`u-blox_GNSS.cpp:14103-14175`）；键 `UBLOX_CFG_SPARTN_USE_SOURCE=0x20a70001`、`UBLOX_CFG_PMP_*`（`u-blox_config_keys.h:1118,1273`） `[verified-in-source]` | **missing** `[verified-in-source]` | **不适用**：本产品通过手机 BT/CORS RTCM3 差分，无 PointPerfect 订阅或 NEO-D9S L-Band 硬件；且实板固件 `HPG 1.12` 根本不支持 SPARTN（`UBX-18010802 §3.1.6.1.2` 要求 HPG ≥ 1.30） | **skip** |

---

## 2. P0 项详细工程设计

本次调研识别出 **两项 P0 缺陷**：
- **P0-1**：ZED-F9P 实现 `navRatesHz()` / `outputPeriodsMs()`，以及 `gnss.rate` 变更与 `gnss.out.*` 周期联动重算、校验与回滚闭环（HIL 已复现的阻塞缺口）。
- **P0-2**：流动站基础配置启用 NMEA 高精度模式（`CFG-NMEA-HIGHPREC = 1` + `CFG-NMEA-SVNUMBERING = 1`），消除默认 5 位小数截断厘米级 RTK 精度的隐患。

---

### 2.1 P0-1：ZED-F9P `navRatesHz()` / `outputPeriodsMs()` 与频率变更闭环设计

#### 2.1.1 现网根因定位 `[verified-in-source]`

1. `components/gnss/src/Receiver.cpp:13-22` 基类默认实现 `navRatesHz()`、`outputPeriodsMs()` 均返回空 `span`。
2. `components/gnss/include/gnss/drivers/UbloxReceiver.hpp`（**冻结文件**，`docs/frozen.sha256:36`）未声明这两个虚函数覆盖。
3. `main/modules/gnss/GnssModuleProbe.cpp:360-380` `publishCaps()` 写入空 `navRatesHz`（`navRateCount=0`）与 `outPeriods[i].supported=false`；探测完成后 `caps.valid=true`，`components/settings/src/Service.cpp:99-112` 因 `dom.supported==false` 且 `caps.valid==true` 不再走预探测静态放行，直接返回 `Error::kUnsupported`；网页 `WebLogic.cpp:229-231` 过滤掉所有 `supported==false` 的键，导致 `gnss.rate` 与 `gnss.out.*` 全部消失。

#### 2.1.2 ZED-F9P RTK 最大定位频率与 460800 波特率带宽核算

##### A. u-blox 官方手册频率上限 `[from-doc]`

- **`ZED-F9P-01B Data sheet`（`UBX-17051259 - R09`，适用 `ZED-F9P-01B-01` / `HPG 1.12`，即当前实板固件），§1.2 Performance Table 1（p. 4）**：
  | 星座组合 | Max nav rate (RTK) | Max nav rate (PVT) | Max nav rate (RAW) | Moving Base RTK (Table 4, p. 5) |
  |---|---|---|---|---|
  | `GPS+GLO+GAL+BDS`（4 星座，本工程默认） | **8 Hz** | 10 Hz | 20 Hz | 5 Hz |
  | `GPS+GLO+GAL`（3 星座） | **10 Hz** | 12 Hz | 20 Hz | 5 Hz |
  | `GPS+GAL` / `GPS+GLO` / `GPS+BDS`（2 星座） | **15 Hz** | 20–25 Hz | 25 Hz | 8 Hz |
  | `GPS`（单星座） | **20 Hz** | 25 Hz | 25 Hz | 10 Hz |

- **`ZED-F9P-04B Data sheet`（`UBX-21044850 - R05`，适用 `ZED-F9P-04B-01` / `HPG 1.32`），§1.2 Performance Table 2（p. 4，脚注 4：GPS 含 QZSS+SBAS；脚注 7：仅开主输出、关闭 NAV2）**：
  | 星座组合 | Max nav rate (RTK) | Max nav rate (PVT) | Max nav rate (RAW) | Moving Base RTK (Table 6, p. 5) |
  |---|---|---|---|---|
  | `GPS+GLO+GAL+BDS`（4 星座） | **7 Hz** | 9 Hz | 15 Hz | 5 Hz |
  | `GPS+GLO+GAL`（3 星座） | **10 Hz** | 10 Hz | 18 Hz | 5 Hz |
  | `GPS+GAL` / `GPS+GLO` / `GPS+BDS`（2 星座） | **15 / 14 / 13 Hz** | 20 / 20 / 16 Hz | 25 Hz | 5 Hz |
  | `GPS`（单星座） | **20 Hz** | 25 Hz | 25 Hz | 8 Hz |

- **`u-blox F9 HPG 1.32 Interface Description`（`UBX-22008968 - R01`），§6.9.18 `CFG-RATE` Table 37（p. 261–262）**：
  - `CFG-RATE-MEAS`（`0x30210001`，U2，单位 ms）：最小值 **25 ms**（40 Hz 为协议硬底线，非 RTK 解算能力上限）。
  - `CFG-RATE-NAV`（`0x30210002`，U2）：1..127，默认 1（导航周期 = `CFG-RATE-MEAS × CFG-RATE-NAV`）。
- **未核实项声明**：`HPG 1.13` / `HPG 1.50` / `HPG 1.51` 的 4 星座 RTK 最大频率表本次未直接下载核实；实板 `P2 v1.41 + HPG 1.12` 在室外满星 4 星座 RTK 下跑 10 Hz 的实际掉历元/丢固定解行为**未上板实测**（U1 HIL 在室内 0 星完成）。

##### B. 460800 波特率下 UART1 带宽核算 `[assumption]`（基于典型 NMEA/RTCM 帧长）

460800 bps（8N1，10 bit/B）单向有效吞吐 = **46,080 B/s**（全双工，TX/RX 互不抢占）：
- **TX（ESP32 → F9P，RTCM3）**：4 星座 1 Hz MSM4 约 0.8–1.5 KB/s，MSM7 约 2.0–3.5 KB/s（即便 ADR-0010 §4 40 KB/s 突发测试也低于 46 KB/s）。
- **RX（F9P → ESP32，本工程 5 类 NMEA）**：
  - 单历元常规句估算：`GGA`（高精度 ~85 B）+ `RMC`（~75 B）+ `GST`（~55 B）+ `GSA`（4–5 系统 × ~65 B ≈ 325 B）≈ **540 B/历元**（保守按 **600 B/历元** 计）。
  - `GSV` 卫星表：4 星座约 40 颗星 ≈ 10–12 句 × 70 B ≈ **850 B/次**（默认 5000 ms = 0.2 Hz 仅占 170 B/s；1 Hz 占 850 B/s）。
  - 总 RX 占用：
    - **5 Hz**（GSV 1 Hz）：$5 \times 600 + 850 = 3{,}850\text{ B/s}$（占 460800 波特率 **8.4%**）
    - **10 Hz**（GSV 1 Hz）：$10 \times 600 + 850 = 6{,}850\text{ B/s}$（占 **14.9%**）
    - **20 Hz**（GSV 1 Hz）：$20 \times 600 + 850 = 12{,}850\text{ B/s}$（占 **27.9%**）
- **结论**：460800 波特率下 **UART1 带宽绝非瓶颈**；真正的硬瓶颈是 **ZED-F9P 内部 4 星座 RTK 解算 CPU 上限（HPG 1.12 为 8 Hz、HPG 1.32 为 7 Hz）**，以及蓝牙 SPP 下行 `gnssAppRing`（4096 B，`ADR-0010 §2.9`）在高频多句订阅下的承载能力（RTK Everywhere `GNSS_ZED.ino:2561` 亦特意限制 GSV ≤ 1 Hz 防刷爆 SPP）。

#### 2.1.3 建议的精确值集（`navRatesHz` / `outputPeriodsMs`）

注意本工程全局离散频率集为 `{1, 2, 5, 10, 20}` Hz（`GnssModulePrivate.hpp:77`、`Service.cpp:38`、`docs/web/ws-protocol.md:48`、`Registry.cpp:979-1020` `periodMsToInterval` 仅编码 `0/50/100/200/500/1000×k` ms `[verified-in-source]`），7 Hz（142.9 ms）与 8 Hz（125 ms）均不在编码域内。

1. **`navRatesHz(Model::kZedF9p)` 推荐值集：`{1, 2, 5}` Hz**
   - **理由**：`Receiver::navRatesHz(Model)`（冻结签名，`Receiver.hpp:174`）不感知当前启用星座数与 HPG 版本。在默认 4 星座 RTK 下，**5 Hz 是 `{1,2,5,10,20}` 中唯一同时满足 HPG 1.12（≤8 Hz）与 HPG 1.32（≤7 Hz）官方 RTK 指标的最大频率**。
   - **权衡说明**：若产品要求开放 `10 Hz`（对齐 `Cm680Receiver` 的 `{1,2,5,10}`，供用户关闭一个星座跑 3 星座 10 Hz RTK 或单点 PVT），则必须在室外 4 星座 RTK 条件下对 P2 v1.41 + HPG 1.12 实测 10 Hz 是否丢历元/失锁，并在 UI/文档明确 10 Hz 仅限 ≤3 星座 RTK。**任何情况下都不应对 4 星座 RTK 开放 20 Hz**（同时建议后续复核 `UbloxReceiver::caps()` 中的 `cap::kRate20Hz` — 注意 `GoldenUblox.inc` 固定校验 `caps`，解冻前不可改 `caps()` 返回值 `[verified-in-source]`）。
   - 非 `Model::kZedF9p` 返回空 `{}`（与 `Cm680Receiver::navRatesHz` 一致，`UnicoreReceiver.cpp:359`）。

2. **`outputPeriodsMs(OutputMsg msg, Model::kZedF9p)` 推荐值集**（对比 `Cm680Receiver` `UnicoreReceiver.cpp:363-384`）：
   - 先确立 **u-blox 历元分频铁律**（`UBX-22008968 §6.9.11` `[from-doc]`）：
     - `CFG-MSGOUT-*`（U1，0..255）含义为“每 `n` 个导航历元输出 1 次”，`0` 为关闭。
     - 因此物理可精确实现的输出周期必须同时满足：
       $$\text{periodMs} = 0 \quad \text{或} \quad \begin{cases} \text{periodMs} \ge T_{\text{nav}} = \dfrac{1000}{\text{rateHz}}, \\ \text{periodMs} \bmod T_{\text{nav}} = 0, \\ 1 \le \dfrac{\text{periodMs}}{T_{\text{nav}}} \le 255. \end{cases}$$
   - **关键陷阱——`rateHz = 5`（$T_{\text{nav}} = 200\text{ ms}$）下的 `500 ms`**：
     - $500 \div 200 = 2.5$，**不是整数倍**！
     - 现有 `UbloxReceiver::buildOutput()`（`UbloxReceiver.cpp:381-382`）用四舍五入 `(500*5+500)/1000 = 3` 静默输出 **600 ms**（1.67 Hz），与用户请求的 `500 ms` 不符！
     - 现有 `GnssModuleProbe.cpp:374` 与 `GnssModuleCmds.cpp:255` **只检查了 `period >= minPeriod`，未检查 `(period % minPeriod) == 0`**！
   - 各报文静态候选表（升序，均在 `periodMsToInterval` 合法编码集内，≤ `kMaxDomainValues=12`，经 `publishCaps()` 按当前 `rateHz` 过滤后产出实际可用集合）：
     - `kGga` / `kGst` / `kRmc`：`{0, 200, 500, 1000}`（若开放 10 Hz 则加 `100` → `{0, 100, 200, 500, 1000}`）
     - `kGsa`：`{0, 200, 500, 1000}`（F9P 支持每历元输出 GSA，无 CM680 亚秒限制；且 `GnssOutConfig::defaults()` 使 GSA 默认 `followsFixRate`，若要控 BT 流量也可保守取 `{0, 1000}` 并在 `makePlan` 固定 1 s，与 CM680 行为一致；二者二选一，推荐保守取 `{0, 1000}` 与 CM680 对齐，减少 4–5 系统多句 GSA 在 5 Hz 下的 25 句/秒流量）
     - `kGsv`：`{0, 1000, 5000}`（与 `Cm680Receiver` 一致，覆盖默认 `5000 ms`、常用 `1000 ms` 和关闭 `0`，避免高频率 GSV 洪泛 SPP；1000/5000 ms 对 1/2/5/10 Hz 全为整倍历元：$n \in \{1,2,5,10,25,50\} \le 255$）
     - `kSatsInfo` / `kHeading` / 其他：返回 `{}`（F9P 不支持 Unicore 私有报文；`GnssModuleProbe.cpp:429-438` 已把 `kSatsInfo` 能力查询映射至 `kGsv` `[verified-in-source]`）。
   - **各 `rateHz` 下经整倍历元过滤后的实际 `caps` 取值域（按 `{1,2,5}` + 推荐 GSA `{0,1000}`）**：
     | `gnss.rate` | $T_{\text{nav}}$ | `gnss.out.{gga,gst,rmc}`（对应 `n`） | `gnss.out.gsa`（对应 `n`） | `gnss.out.gsv`（对应 `n`） |
     |---|---|---|---|---|
     | `1` Hz | 1000 ms | `[0, 1000]` (`0, 1`) | `[0, 1000]` (`0, 1`) | `[0, 1000, 5000]` (`0, 1, 5`) |
     | `2` Hz | 500 ms | `[0, 500, 1000]` (`0, 1, 2`) | `[0, 1000]` (`0, 2`) | `[0, 1000, 5000]` (`0, 2, 10`) |
     | `5` Hz | 200 ms | `[0, 200, 1000]` (`0, 1, 5`) — **`500` 被整除检查自动滤除！** | `[0, 1000]` (`0, 5`) | `[0, 1000, 5000]` (`0, 5, 25`) |

#### 2.1.4 `gnss.rate` 下发流程、输出周期重算与必须修复的 4 个隐藏缺陷

追踪完整控制面路径 `settings::Service::set("gnss.rate", ...)` → `GnssModule::cmdSetting()` → `cmdSetRate()` → `beginPendingApply()` → `Controller` → `tickPendingApply()` → `publishCaps()` → `WebModule` `[verified-in-source]`：

1. **如何下发 `CFG-RATE-MEAS`**：
   - `beginPendingApply()`（`GnssModuleMaintenance.cpp:417-430`）在 `PendingApply::kRate` 时先调用 `controller().setOutputPlan(makePlan())`，再调用 `controller().requestReconfigure(nowMs)`。
   - `Controller` 进入 `kConfiguring`，执行 `UbloxReceiver::roverStep(0)` → `cmdConfigurePort(periodMs, port)`（`UbloxUbx.cpp:114-128`），在 **RAM 层** 发送 `CFG-VALSET` 写入 `CFG-RATE-MEAS = 1000 / rateHz`（`0x30210001`, U2）+ `INPROT-RTCM3X=1` + `OUTPROT-NMEA=1`；随后 `roverStep(1)` 静默 12 条标准 NMEA（`cfg.portBaud == 0`，无步 2）。
   - `kConfiguring` 结束自动调用 `enterOutput(self, false)`（`ControllerConfigure.cpp:82`），按新的 `OutputPlan`（含新 `plan.rateHz` 与新 `plan.periodMs[]`）逐句调用 `UbloxReceiver::buildOutput()`，发送 `CFG-MSGOUT-* = periodMs * rateHz / 1000`。
   - 完成后 `stats_.outputSeq` 自增，`tickPendingApply()`（`GnssModuleProbe.cpp:229-256`）落盘 NVS `gnss/rate`、置 `pending_apply_ = kNone`、调 `publishStatus(nowMs)` → `publishCaps()` 刷新 `DataCenter::gnssCaps`；`WebModule.cpp:657-666` 感知版本号变化，自动推送新 `{"t":"caps",...}` 与 `{"t":"config",...}` 给浏览器！

2. **率变更后输出周期的重算规则与必须补齐的 4 个代码缺陷 `[verified-in-source]`**：
   - **缺陷 1（`publishCaps()` / `cmdPatchOutput()` 缺整倍历元过滤）**：
     - 现状：`GnssModuleProbe.cpp:374` 和 `GnssModuleCmds.cpp:255` 仅判断 `period >= minPeriod`，未判断 `(period % minPeriod) == 0`。
     - 修复：对 `Model::kZedF9p`（按历元分频接收机）增加 `(period % minPeriod) == 0U` 且 `(period / minPeriod) <= 255U` 校验，防止 `5 Hz` 下暴露或受理 `500 ms`。
     - 同时在 `UbloxReceiver::buildOutput()`（`UbloxReceiver.cpp:379-389`）收紧校验：若 `periodMs != 0` 且 `(periodMs * rate) % 1000 != 0`（在 `plan.useConfiguredPort == true` 路径下，或全路径核对黄金测试后统一收紧 — 已核实 `PlanCheck.hpp:38,48` 与 `TestGnssUbloxReceiver.cpp:247-277` 全为整倍历元），直接返回 `fw::Status::kInvalidArg` 而非静默四舍五入为错误周期。
   - **缺陷 2（`makePlan()` / `SettingsBackend.cpp` 周期钳位与回显只写了 `kCm680`，漏掉 `kZedF9p`）**：
     - 现状：`GnssModuleProbe.cpp:484-496` `makePlan()` 仅在 `model == kCm680` 时把显式周期钳位到 `>= minPeriod`（且把默认 `followsFixRate` 的 GSA 钳到 `1000 ms`）；`main/app/SettingsBackend.cpp:117-125` `currentPeriod()` 也仅在 `status.model == kCm680` 时读 `status.rxPeriodMs[index]`！
     - 后果：若用户先在 5 Hz 下显式设 `gnss.out.gga = 200`，再降频到 1 Hz，或从 2 Hz（显式 `gga = 500`）切到 5 Hz：
       - 若不在 `makePlan()` 中对 `kZedF9p` 做“对齐到不快于原周期的最小合法整倍历元周期（`next_valid = ceil_to_allowed_multiple(period, T_nav)`，若超表尾取表尾）”，`out_cfg_` 与 `config` 推给网页的值会掉出新 `caps["gnss.out.gga"]`，导致前端下拉框空白/前后端失步！
     - 修复：
       1. 在 `makePlan()` 中为 `kZedF9p` 增加周期对齐逻辑：非 0 周期取 `outputPeriodsMs(msg, kZedF9p)` 中满足 `p >= period && p >= minPeriod && (p % minPeriod) == 0` 的最小值（找不到则取满足整倍条件的最大合法值）；若采用保守 GSA `{0,1000}`，同步处理 `followsFixRate(GSA)` → `1000 ms`。
       2. 在 `SettingsBackend.cpp:117` 将 `status.model == gnss::Model::kCm680` 扩展为 `(status.model == gnss::Model::kCm680) || (status.model == gnss::Model::kZedF9p)`，保证 `config get gnss.out.*` 与网页 `config` 帧始终反映与 `caps` 一致的真实生效周期。
       3. `GnssModuleProbe.cpp:440-471` `restoreOutputPeriods()` 在启动按型号恢复 NVS `gnss/out` 时，对 `kZedF9p` 同样遵循 `p >= minPeriod && (p % minPeriod) == 0` 对齐。
   - **缺陷 3（`PendingApply::kRate` 失败回滚未恢复 F9P `CFG-RATE-MEAS`）**：
     - 现状：`GnssModuleProbe.cpp:248-250` 在 `tickPendingApply()` 失败/超时回滚分支，无论 `PendingApply::kRate` 还是 `kOut`，均仅调用 `controller().requestOutput(...)`！
     - 后果：若 `gnss.rate` 在 `roverStep(0)` 已成功改写了 F9P 的 `CFG-RATE-MEAS`，但后续 `kOutput` 某句超时失败，回滚分支只重发 `MSGOUT` 而不重发 `CFG-RATE-MEAS`，导致主机回滚到旧 `rateHz`、F9P 仍运行在新 `CFG-RATE-MEAS`！
     - 修复：在 `restorePendingApply()` 前记录 `const bool wasRate = (pending_apply_ == PendingApply::kRate);`；若 `wasRate` 为真，回滚调用 `controller().requestReconfigure(nowMs)`（重配旧 `CFG-RATE-MEAS` + 旧 `OutputPlan`），而非仅调 `requestOutput()`。
   - **缺陷 4（`requestReconfigure` 触发不必要的 `sat_replay_` 600 ms 星座重置抖动）**：
     - 现状：`GnssModuleProbe.cpp:145-148` 只要状态进入 `kConfiguring` 就置 `sat_replay_ = true`。用户仅改 `gnss.rate` 时，`requestReconfigure()` 会顺带重放 `CFG-SIGNAL-*` 导致 F9P 600 ms 星座重新稳定与瞬时断锁。
     - 修复（低成本优化）：仅当从 `kProbing` 进入 `kConfiguring`（冷启动/重探后，即 `prev == kProbing`）时置 `sat_replay_ = true` / `ppp_replay_ = true`，避免运行期调频误触发星座重配（基站/流动站模式切换同理不受影响）。

#### 2.1.5 冻结文件约束（`docs/frozen.sha256`）与实施路径

核对 `docs/frozen.sha256` `[verified-in-source]`：
- `components/gnss/include/gnss/drivers/UbloxReceiver.hpp` 位于 `docs/frozen.sha256:36`（**冻结**）。
- `components/gnss/include/gnss/Receiver.hpp` 位于 `docs/frozen.sha256:33`（**冻结**）。
- `components/gnss/src/Receiver.cpp`、`components/gnss/src/drivers/UbloxReceiver.cpp`、`components/gnss/src/drivers/UbloxUbx.cpp`、`main/modules/gnss/*`、`main/app/SettingsBackend.cpp`、`test/gnss/*`、`tools/main_host_check/*` 均**非冻结**（`test/host/**` 冻结，但 `test/gnss/**` 不在 `docs/frozen.sha256` 中）。

因此存在两条清晰路径（强烈推荐 **方案 A**）：
- **方案 A（推荐，Architect 解冻 1 个公共头）**：
  - 由 Architect 在 `components/gnss/include/gnss/drivers/UbloxReceiver.hpp` 的 `UbloxReceiver` 公有段追加两行声明（与 `UnicoreReceiver.hpp:161-163` 对称），并重算 `docs/frozen.sha256`：
    ```cpp
    [[nodiscard]] std::span<const uint8_t> navRatesHz(Model model) const noexcept override;
    [[nodiscard]] std::span<const uint16_t> outputPeriodsMs(OutputMsg msg,
                                                            Model model) const noexcept override;
    ```
  - 实现直接写在 `components/gnss/src/drivers/UbloxReceiver.cpp`。架构整洁、与 ADR-0012 §2.2/§2.8 完全一致、无黑魔法。
- **方案 B（零冻结文件应急方案，不推荐长期留存）**：
  - 若任务卡暂不开放解冻窗口，可临时在非冻结的 `components/gnss/src/Receiver.cpp:13-22` 的基类默认体中按 `if (model == Model::kZedF9p)` 分发返回 F9P 静态表（需加明确注释标明待 Architect 解冻 `UbloxReceiver.hpp` 后迁入派生类）。缺点是基类感知具体型号，违背 ADR-0012 §2.2 厂家隔离原则，仅作应急备选。

---

### 2.2 P0-2：启用 NMEA 高精度模式（`CFG-NMEA-HIGHPREC = 1` + `CFG-NMEA-SVNUMBERING = 1`）

- **必要性**：`UBX-18010802 - R16 §3.1.4.5` `[from-doc]` 明确警告 `CFG-NMEA-HIGHPREC` 出厂默认为 `0`（经纬度 5 位小数，赤道量化误差 ~1.8 cm），**无法表达厘米级 RTK 精度**；开启后经纬度输出 **7 位小数**（~0.18 mm）、高程 **3 位小数**（1 mm）。我方 `NmeaParserNumeric.cpp:40,55` `[verified-in-source]` 已原生支持 7 位小数经纬度与毫米高程。RTK Everywhere `GNSS_ZED.ino:442-443` `[verified-in-source]` 无条件开启 `HIGHPREC=1` 与 `SVNUMBERING=1`。
- **协议键**（`UBX-22008968 - R01 §6.9.15 Table 28` `[from-doc]`；`u-blox_config_keys.h:1066-1067` `[verified-in-source]`）：
  - `CFG-NMEA-HIGHPREC` = `0x10930006U`（L，1 B），值 `1U`（注意与 `COMPAT`/`LIMIT82` 互斥，出厂默认两者均为 0）。
  - `CFG-NMEA-SVNUMBERING` = `0x20930007U`（E1，1 B），值 `1U`（EXTENDED，保证多系统卫星号完整输出）。
- **下发位置（严格保护黄金向量）**：
  - 在 `UbloxWire.hpp` 新增上述两键常量。
  - 仅追加到 `UbloxReceiver::cmdConfigurePort()`（`UbloxUbx.cpp:114-128`，实板路径 `cfg.useConfiguredPort == true`，`GnssModule.cpp:103` `[verified-in-source]`）：载荷仅从 `4+6+5+5=20 B` 增至 `30 B`（远小于 `kValsetPayloadMax=80 B`），无需增加步骤数。
  - **不动 `cmdConfigureReceiver()`**（`cfg.useConfiguredPort == false` 路径），从而保证 `test/gnss/golden/GoldenUblox.inc` 与 `testGoldenOutputPlan` **零破坏** `[verified-in-source]`。
  - **验证要点**：在 P2 v1.41 + `HPG 1.12` 上 HIL 确认该 5 键 VALSET 返回 ACK（若 `HPG 1.12` 拒绝其中任一键，需拆分确认；按 `UBX-18010854` HPG 1.12 文档与 RTK Everywhere 实测均支持 `[from-doc]` `[verified-in-source]`），并抓取实板 GGA 验证经纬度 7 位小数。

---

### 2.3 需要修改的文件与测试清单（P0-1 + P0-2）

| 文件 | 冻结状态 | 变更内容 |
|---|---|---|
| `components/gnss/include/gnss/drivers/UbloxReceiver.hpp` | **冻结**（`docs/frozen.sha256:36`，需 Architect 改并重算哈希） | 声明 `navRatesHz(Model)`、`outputPeriodsMs(OutputMsg, Model)` 覆盖 |
| `components/gnss/src/drivers/UbloxWire.hpp` | 非冻结 | 增加 `kKeyNmeaHighprec = 0x10930006U`、`kKeyNmeaSvNumbering = 0x20930007U` |
| `components/gnss/src/drivers/UbloxReceiver.cpp` | 非冻结 | 实现 `UbloxReceiver::navRatesHz`（`{1,2,5}`）与 `outputPeriodsMs`（GGA/GST/RMC `{0,200,500,1000}`，GSA `{0,1000}`，GSV `{0,1000,5000}`）；`buildOutput` 在 `periodMs != 0` 且 `(periodMs*rate)%1000 != 0` 时返回 `kInvalidArg` |
| `components/gnss/src/drivers/UbloxUbx.cpp` | 非冻结 | `cmdConfigurePort` 追加 `CFG-NMEA-HIGHPREC=1`、`CFG-NMEA-SVNUMBERING=1` |
| `main/modules/gnss/GnssModuleProbe.cpp` | 非冻结 | ① `publishCaps()` 对 F9P 增加 `(period % minPeriod) == 0` 过滤；② `makePlan()` 与 `restoreOutputPeriods()` 对 F9P 增加周期整倍历元对齐（含 GSA 默认 1 s 规则）；③ `tickPendingApply()` 回滚 `PendingApply::kRate` 时改调 `requestReconfigure(nowMs)`；④ 仅 `prev == kProbing` 时置 `sat_replay_`/`ppp_replay_` |
| `main/modules/gnss/GnssModuleCmds.cpp` | 非冻结 | `cmdPatchOutput()` 对 F9P 增加 `(period % fixPeriod) == 0` 校验 |
| `main/app/SettingsBackend.cpp` | 非冻结 | `currentPeriod()` 扩展 `status.model == gnss::Model::kZedF9p` 走 `status.rxPeriodMs[index]` 与 `gsv` 域判空 |
| `test/gnss/TestGnssUbloxReceiver.cpp` | 非冻结 | 新增用例：`testF9pNavRatesAndOutputPeriods`（校验率/周期值集、非 F9P 返回空）；`buildOutput` 拒绝非整倍历元（如 5 Hz + 500 ms → `kInvalidArg`）；更新 `testConfiguredOutputPort` 断言含 `0x10930006=1` 与 `0x10930007=1`；保持 `testGoldenOutputPlan` PASS |
| `tools/main_host_check/GnssResetCheck.cpp` | 非冻结 | 新增 F9P 集成用例：探测后 `gnssCaps` 包含 `gnss.rate` 与 5 条 `gnss.out.*`；`config set gnss.out.gsv 0` 成功并下发 `0x209100C5=0`；`gnss.rate` 1→5 Hz 重发 `CFG-RATE-MEAS=200` 且重算 `GSV(5000 ms)` 为 `epochs=25`、新 `caps["gnss.out.gga"]` 不含 `500`；`gnss.rate` 失败回滚时恢复旧 `CFG-RATE-MEAS`；调频不触发 `sat_replay_` |
| `test/settings/TestSettingsRegistry.cpp` / `TestSettingsService.cpp` | 非冻结 | 视需补充 5 Hz 下 `[0,200,1000]` 域拒绝 `500` 的断言（无破坏性改动） |

---

## 3. SparkFun 健壮性模式对比与取舍评估

| # | SparkFun 模式（v3 3.1.15 / RTK Everywhere） | 代码依据 `[verified-in-source]` | 本工程现状 `[verified-in-source]` | 是否需要引入？ |
|---|---|---|---|---|
| 1 | **包长度边界与溢出保护**（处理 `len==0`/`1`、ACK 强制 `len==2`、按消息类 `autoLookup` 动态分配 `payloadAuto`、越界复位状态机） | `u-blox_GNSS.cpp:1842-1912, 3267-3300, 3479-3510` | `stream_demux` 的 `Ubx.cpp:103-121` 读完 6 B 头即计算 `total = 6 + payloadLen + 2`，超 `kMaxFrame=4096`（`Ubx.hpp:14`）立即早拒绝，全程静态缓冲零堆分配 | **不需要**。本工程 `stream_demux` 设计更严格且无堆分配风险；唯一建议是未来若引入 `VALGET` 读回，确保单次查询响应总长远低于 4096 B。 |
| 2 | **Fletcher-8 滚动校验与坏帧处理**（滚动累加；坏校验时若 cls/id 碰巧匹配仍标 `NOT_VALID` 提前终止等待，注释承认坏字节可能就在 cls/id） | `u-blox_GNSS.cpp:3305-3306, 3335, 3418-3434, 5295-5325` | `demux::ubx` 帧尾整帧校验不通过直接 `kReject` 丢弃并重同步（`Ubx.cpp:124-142`）；`UbloxExt` 二次校验。仅 `UbloxReceiver::respHasValidAck`（`UbloxUbx.cpp:198-208`）未二次算 CRC | **不需要照搬**（我方丢弃坏帧等重发/超时更安全）；可选在 `respHasValidAck` 补调 `validUbxChecksum` 提升单元测试防御纵深。 |
| 3 | **通用 `class/id` ACK 匹配 + 四缓冲隔离**（`packetCfg`/`packetAck`/`packetBuf`/`packetAuto` 分离防覆盖；按发送帧 `(cls,id)` 匹配 `ACK.payload[0..1]`） | `u-blox_GNSS.cpp:1718-1922, 3342-3366, 5753-5856` | `stream_demux` 整帧交付无字节覆写问题；我方还有 SparkFun 缺乏的 **300 ms 迟到 ACK 重同步窗**（`UbloxExt.cpp:500-546`）。**但 `UbloxReceiver::classifyReply()`（`UbloxReceiver.cpp:311-315`）硬编码只认 `CFG-CFG` 与 `CFG-VALSET`** | **部分需要（P1）**：不需要四缓冲机制，但应将 `classifyReply()` 泛化为直接读取 `sent[kClsOffset]`/`sent[kIdOffset]`（空 `sent` 保持兼容 `0x06/0x8A`），为 `VALGET`/其他 `CFG-*` 铺路。 |
| 4 | **`maxWait` 阻塞轮询与动态加长超时**（默认 `kUBLOXGNSSDefaultMaxWait=1100` ms；RTK Everywhere 总线忙时 `1100→5000 ms` 渐进递增 + 分批 3–5 次重试） | `u-blox_external_typedefs.h:59`, `u-blox_GNSS.cpp:5764-5904`, `GNSS_ZED.ino:1971-1981,2221,2283-2292` | 非阻塞状态机，`VALSET` 默认 `500 ms`（`UbloxWire.hpp:64`）+ `stepRetries` 重试 + `300 ms` 重同步窗；`CFG-CFG` 保存 `1800 ms`；`SatMaskSequence` `1000 ms` + `600 ms` 稳定期 | **不需要阻塞 `delay(1)` 轮询**（我方非阻塞状态机远优）；但建议在满载 RTCM3 + 5 Hz RTK 室外 HIL 中观察 `500 ms` VALSET 超时是否偶发超时，必要时放宽至 `800–1000 ms`（与 `GnssModuleProbe.cpp:38` 同步）。 |
| 5 | **Auto-PVT / `implicitUpdate` 与三层回退**（`setAutoMsgRateVal` 在 `setVal8` 失败后调 `getVal8` 读真值） | `u-blox_GNSS.cpp:10768-10797, 11584-11664` | 单任务 `kIo` 事件流直接解析 NMEA，不轮询 `getPVT`；`Controller` 失败即按重试+回滚处理 | **不需要 `auto-PVT`**（与 ADR-0010/0012 架构冲突）；`VALGET` 读真值仅作为 P1 `gnss config` 诊断工具即可，不必引入复杂三层猜测逻辑。 |
| 6 | **VALSET 64 键上限与自动分包**（`CFG_VALSET_MAX_KEYS=64`、`autoSendCfgValsetAtSpaceRemaining`、按 Class 分批） | `u-blox_config_keys.h:55`, `u-blox_GNSS.cpp:10333-10363`, `GNSS_ZED.ino:153,2205,2392` | `kValsetPayloadMax=80 B`、`kMaxCommand=160 B`（最多约 15 个 U1 键，天然远低于 64 键），`ValsetWriter` 越界返回 `false` 拒绝截断 | **当前不需要自动分包**；保持静态缓冲 + 单元测试覆写载荷上限即可（P0-2 追加 2 键后 `cmdConfigurePort` 仅 30 B）。 |
| 7 | **固件版本能力表 + 易 NAK 键隔离拆包**（每键标注最低 HPG 版本；不支持键单独发 `setVal8` 防整包 VALSET 被 NAK 拖死） | `GNSS_ZED.h:28-38,61-278`, `u-blox_GNSS.cpp:8270-8275` | U1 已吸收此模式：`f9pSupportsSbas`（HPG≥1.13）、`uart2FactoryUbxInput`（HPG≥1.32）、静默步 NAK 非致命跳过 | **需要持续坚持**：后续新增任何 HPG 版本敏感键（如 `NAV2`、`RXM-COR`、`MON-SPAN`、NMEA 4.11）均应查表门控或独立拆步，绝不与核心键混包。 |
| 8 | **Survey-In 重置序列与 `VALGET` 层编码差异**（切 Fixed 或重启 Survey-In 前先清旧 `NAV-SVIN` 态防 RTCM 被抑制；`VALGET` 层号 0/1/2/7 ≠ `VALSET` 位图 1/2/4） | `GNSS_ZED.ino:761-770, 2831-2907`; `u-blox_GNSS.cpp:10578-10595` | `UbloxUbx.cpp:361-404` `modeStep` 未清旧 Survey-In 态；尚未实现 `VALGET` | **强需要（P1）**：基站 HIL 前必须吸收 Survey-In 清除逻辑；实现 `VALGET` 读回时必须严格区分层编码。 |
