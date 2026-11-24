# P2 旧固件（`P2_V3.1_IDF`）与新固件（`RTK_main`）功能与硬件差距分析报告

- **分析对象**：
  - 旧固件（只读参考）：`/home/gtc/Desktop/workspace/ESP32_PROJ/P2_V3.1_IDF`（C，ESP-IDF 5.5.1，`Equipment_type="P2"`，`Hardware_version="V1.3"`，`Software_version="V3.0.1"`，`main/main.c:7-11`；支持 u-blox ZED-F9P / UM980 / UM981 / UM982 / CM680 / UM960，无自定义 `components/` 目录，`main/idf_component.yml` 仅声明未使用的 `sd_card` 示例组件）。
  - 新固件：`/home/gtc/Desktop/workspace/ESP32_PROJ/RTK_Main/RTK_main`（C++20 / C11，ESP-IDF v6.1，`main` 分支，截至 2026-11-23）。
- **状态图例**：
  - `implemented`：功能已在新固件中等价或更完善地实现。
  - `partial`：仅实现部分能力、仅占位存储/未接硬件、或对 F9P / P2 存在明确缺口或未过 HIL。
  - `missing`：新固件尚未实现，且未在 ADR 中声明永久放弃（部分标记为“后续”）。
  - `intentionally dropped`：ADR 或用户决策明确放弃/替换为不同架构。
- **未知项约定**：源码与原理图未明确记载的事实一律标注 `UNKNOWN`。

---

## 1. 旧固件全量功能清单与 `RTK_main` 对照

### 1.1 蓝牙 SPP / BLE / 无线模式切换

| 功能 | 旧固件依据（`P2_V3.1_IDF/`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| Classic BT BR/EDR SPP（释放 BLE 内存，无 BLE） | `main/src/xyz_bluetooth.c`（`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`，单会话 `_spp_client`） | `implemented` | `components/spp_server/SppServerInitBt.c:62`（同样释放 BLE，扩展为双会话 `SPP_SERVER_MAX_SESSIONS=2`，支持手机 + 报警器，`components/spp_server/include/SppServer.h:21`）。 |
| 蓝牙设备名 `P2-<SN>` | `main/src/xyz_bluetooth.c`（`P2-<Equipment_SN>`） | `implemented` | `main/modules/system/SystemModule.cpp:36-51,100`、`main/board/BoardProfile.cpp:25`（`<product>-<SN>`，识别 P2 后生成 `P2-<SN>`）。 |
| 蓝牙配对方式与 CoD | `main/src/xyz_bluetooth.c`（`ESP_BT_IO_CAP_NONE` 无交互自动配对；CoD=`MISC\|POSITIONING\|OBJ_TRANSFER`） | `partial` | `components/spp_server/SppServerInitBt.c:115-138`、`SppServerSession.c:454-538` 改用 `ESP_BT_IO_CAP_IO`（手机需配对窗口/控制台确认，NoIO 报警器走 Just Works）且未显式设置旧 CoD（ADR-0014 配对窗口策略，属安全收紧但改变手机首次直连体验）。 |
| 蓝牙拥塞 3 s 自动断连 | `main/main.c:91-99`（检测发送拥塞持续 3 s 主动断开） | `implemented` | `components/spp_server/SppServerTx.c`、`SppServerTxRing.c` 改为非阻塞 TX Ring + 背压/预留机制（ADR-0011），避免阻塞任务而非粗暴断连。 |
| BT / Wi-Fi 互斥切换 | `main/src/xyz_webserver.c:17`（`BT_WIFI_AUTO_SWITCH=1`，OLED 菜单运行期热切 `bluetooth_stop()` / `wifi_init_softap()`） | `partial` | `docs/adr/ADR-0017-wifi-web-config.md:164-169`、`main/app/RadioMode.hpp`、`main/modules/bt_link/BtLinkModule.cpp:607-613` 实现启动期互斥（切换须重启）；入口改为手机 `0x28` 与控制台 `radio wifi/bt`，P2 OLED 菜单入口延后（ADR-0017 §7）。 |
| 手机流 Raw RTCM3 提取并转发 GNSS UART2 | `main/user_src/user_bt_message.c:885`、`main/user_src/user_tools.c:48-140`（`Analyze_RTCM_Frame`，仅 `RTK_enable < 1` 时转发） | `implemented` | `components/stream_demux`（`demux::rtcm3::kDecoder`）+ `main/modules/bt_link/BtLinkSessions.cpp:51-120`（ADR-0005/0006/0011 无锁直通，F9P 实板 RTCM 注入待 HIL）。 |
| 手机连接直推 Raw NMEA（非 Nav App） | `main/user_src/user_gnss.c:375-388`（推送 GGA/GSA/GST/RMC/GSV/ZDA/VTG/GLL） | `partial` | `main/modules/bt_link/BtLinkGnss.cpp:36-40,238-281` 连接 PHONE 即推 `GGA/GST/RMC/GSA/GSV`；**缺少 `ZDA/VTG/GLL`**；新增 Q1 GGA 质量门控（`components/gnss/private_include/GgaQualityGate.hpp:1`）及基站 RTCM 下行至手机（`BtLinkGnss.cpp:283-318`）。 |

### 1.2 蓝牙私有二进制协议（帧头 `0xAA 0x44 0x18`，CRC32）

旧固件按 `sender == 0x00` 区分 `Connect_Nav_APP` 与 `Connect_Survey_APP`（`main/user_src/user_bt_message.c:137-1006`）；`RTK_main` 通过 `components/device_proto` + `components/link_core/services/`（`DeviceService`、`GnssService`、`ConfigService`、`PhoneCompatService`）实现兼容。

| Msg ID | 旧固件功能与位置（`P2_V3.1_IDF/main/user_src/user_bt_message.c`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| `0x01` | 设备版本信息：产品/SN/HW(`V1.3`)/SW(`V3.0.1`)/GNSS 型号/GNSS SN/GNSS FW（`:155,895-911`） | `partial` | `components/link_core/services/DeviceService.cpp:105-123,215`、`main/modules/bt_link/BtLinkModule.cpp:798-801`、`BtLinkGnss.cpp:98-105`：产品名已填 `P2`，SW 已填 `appVersion()`，板卡型号/FW 可更新；但 **`hwVer` 硬编码 `"V1.1.0"`**（未随 P2 v1.41 更新），且 **F9P 未提取 `boardSn`**（保持空）。 |
| `0x02` | 外设能力位图（GNSS/IMU/4G/Battery/WiFi/BT/Display/PPP，`:158,914-930`） | `partial` | `DeviceService.cpp:94,136-142` 仅固定 14 B 模板并按 `gnssRunning` 更新 offset 2（`0x81`/`0x01`），不动态反映 P2 4G/OLED/IMU/电池状态。 |
| `0x03` | 电池电量百分比 + 充电标志（`:161,933-943`） | `partial` | `DeviceService.cpp:151-164`、`components/link_core/include/core/Reply.hpp:68-70` 路由已通，但无 ADC 采样与充电同步，**恒返回 `onBattery=1, batteryPct=0xFF (kValueUnknown), temp=0`**。 |
| `0x04` | GGA 查询与订阅应答（`:164-183`） | `implemented` | `components/link_core/services/GnssService.cpp:23,39,216-244`（含 Q1 GGA 门控联动）。 |
| `0x05` | 可见卫星信息（`:184-190`，旧版亦为简版/区分 Nav 与 Survey） | `implemented` | `GnssService.cpp:24,40,245`（P1 Rev A 兼容：按 App 类型回 2 B 全 0 并维护订阅状态）。 |
| `0x06` | 卫星星座开关（GPS/BDS/GLO/GAL）+ 截止角（`:191-240`；注意旧版此处误直接调 `UM980_unmask_init/mask_init` 未分支 F9P） | `partial` | `DeviceService.cpp:266-275`、`PhoneCompatService.cpp:745-800`：Survey App 对 UM980/CM680 走异步配置；**对 F9P，`supportsSatMask(Model::kZedF9p)` 返回 `false`（`components/gnss/src/PppSequence.cpp:42-56`），0x06 SET 直接失败**；Nav App 仅回占位应答（`DeviceService.cpp:171-178`）。 |
| `0x07` | 工作模式 Rover/Base 查询与设置（`:241-243,948-970`，旧版 SET 仅改内存 `GNSS_Mode`） | `partial` | `components/link_core/services/ConfigService.cpp:87,97-110` 仅读写内存占位字节，未驱动 `GnssModule` 的真实 Rover/Base 模式切换（真实模式切换仅在控制台 `gnss mode` 实现，`main/modules/gnss/GnssWorkMode.cpp`）。 |
| `0x08` | 设备 SN / PN 查询与写入（Nav App，`:244-277`） | `implemented` | `PhoneCompatService.cpp:292-324`、`BtLinkModule.cpp:419-465`（经 `settings::Service` 持久化 `factory.sn` 与 `rtk/pn`）。 |
| `0x09` | 4G 模组信息查询 & APN/用户名/密码/使能设置（`:278-280,975-1005`） | `partial` | `PhoneCompatService.cpp:332-342` 仅协议占位（query 96 B 全 0，set 回 `01 01`）；4G 未立项（`docs/PROGRESS.md` §2）。 |
| `0x0A` | GSV 缓存读取（Nav App，`:281-298`） | `implemented` | `PhoneCompatService.cpp:351-367`（返回最新缓存 GSV 原文，无缓存回 `02 02`）。 |
| `0x11` | 经 4G 获取 NTRIP 挂载点列表（`:299-316`） | `partial` | `PhoneCompatService.cpp:375-386` 仅占位返回空列表 `{1,0,0,0}`（4G 未立项）。 |
| `0x12` | 主机 4G NTRIP RTK 配置与开关（`:317-367`） | `partial` | `PhoneCompatService.cpp:396-416`、`main/modules/storage/StoreKeys.hpp:46-52` 支持 112 B RAM+NVS 持久化读写，但**不连接 4G/NTRIP**。 |
| `0x13` | 保留占位 132 B（`:368-372`） | `implemented` | `PhoneCompatService.cpp:424-429`（等价回 132 B 全 0）。 |
| `0x14` | 电台配置占位（`:373-392`） | `implemented` | `ConfigService.cpp:88,119-134`（等价内存占位）。 |
| `0x15` | 静态记录配置占位（`:393-412`） | `implemented` | `ConfigService.cpp:89,142-155`（等价内存占位）。 |
| `0x1E` | 主机控制：恢复出厂（`eraseNVSFull`+`EEPROM_deinit`+`freset`）与重启（`:413-435`） | `implemented` | `PhoneCompatService.cpp:453-474`、`BtLinkModule.cpp:591-620`（先回 `01 01`，延迟 >=200 ms 执行擦除用户 NVS / 重启；注意 `freset` 保留出厂分区 `nvs_fac` 且不自动发接收机 `freset`，见 `FactoryReset.cpp:76-80`）。 |
| `0x21`/`0x22`/`0x23` | GST / RMC / GSA 查询与订阅（`:436-482`） | `implemented` | `GnssService.cpp:26-28,42-44,216-244`。 |
| `0x24` | 倾斜测量天线高读写（`:483-504`） | `partial` | `PhoneCompatService.cpp:523-552`、`StoreKeys.hpp:16-22` 支持 8 B `binary64 LE` RAM+NVS 存储，但不下发给 GNSS/IMU 倾角引擎。 |
| `0x25` | 惯导 NAVI 数据占位（`:505-509`） | `implemented` | `PhoneCompatService.cpp:560-563,814`（回 `set-res 00 00`）。 |
| `0x26` | 杆对准/惯导开关（`:510-531`，旧版调用 `user_slant.c` UM981 指令） | `partial` | `PhoneCompatService.cpp:571-581` 仅协议占位（query 4 B 全 0，set 回 `01 01`），无倾角指令下发。 |
| `0x27` | 手簿网络开关（`:532-551`） | `partial` | `PhoneCompatService.cpp:560-563,816` 占位回 `00 00`。 |
| `0x29` | `BASEINFO` 占位（`:552-558`） | `implemented` | `PhoneCompatService.cpp:589-593`（回 `01 01`）。 |
| `0x32` | 自定义 TCP JSON 遥测服务器配置（Nav App，`:559-595`） | `partial` | `PhoneCompatService.cpp:603-626`、`StoreKeys.hpp:53-59` 支持 42 B RAM+NVS 存储，但无 4G/TCP 遥测发送端。 |
| `0x33` | Nav: ThingsBoard MQTT 配置 / Survey: PPP 开关（`:596-686`） | `partial` | `PhoneCompatService.cpp:636-702`：Nav 侧 86 B 仅 RAM+NVS 存储（`StoreKeys.hpp:60-66`），无 MQTT 客户端；Survey 侧 PPP 对 UM980/981/982 真实下发，对 F9P 正确返回不支持（`PppSequence.cpp:26-40`）。 |
| `0x60`/`0x61`/`0x62` | 本地电子围栏列表、详情/导入、新建/加点/完成/启停/删除（Nav App，`:687-853`） | `missing` | `PhoneCompatService.cpp:710-742` 仅返回空列表/不支持占位（`0x60` 回 `{0}`，`0x61`/`0x62` 回 `02 02`）；无围栏引擎与存储。 |
| *新固件新增* | `0x0D` COM 查询、`0x1D` OTA 占位、`0x1F` HEADING、`0x28` Wi-Fi 配置切换、`0x40..0x44` 报警器协议 | `implemented` | `DeviceService.cpp:187`、`PhoneCompatService.cpp:437,486`、`GnssService.cpp:25`、`AlarmService.cpp:282-287`、`PairService.cpp:161`。 |

### 1.3 Web 配置页面、端点与 OTA

| 功能 | 旧固件依据（`P2_V3.1_IDF/`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| SoftAP 热点参数 | `main/src/xyz_webserver.c`：开放无密码，SSID=`P2-<SN>`，IP=`192.168.4.1`，无强制门户 DNS，无自动退出 | `implemented`（架构变更） | `main/modules/web/WebServer.cpp:68-81,700-722,1102-1111`、`main/modules/web/WebDns.cpp`、`docs/adr/ADR-0017-wifi-web-config.md:166-174`：SSID=`P2-<SN>`，**改为 WPA2-PSK 固定口令**，静态 IP 改为 **`192.168.10.12/24`**，新增强制门户 DNS + DHCP Option 114 + 空闲超时自动重启回 BT。 |
| HTTP 页面与静态资源端点 | `main/src/xyz_webserver.c`、`main/user_src/user_web_resourse.c`：`/`、`/favicon.ico`、`/src/*`（Bootstrap/jQuery/main.js/css/png/icomoon） | `implemented`（重构） | `main/modules/web/WebServer.cpp:381-390,757-783`、`main/web/ui/`：单页 gzip 打包（`/`、`/favicon.ico` 204、`/ws`、系统门户探测 302）。旧 `/src/*` 路径不兼容。 |
| WebSocket 协议与可配置字段 | `main/src/xyz_websocket.c`、`main/user_src/user_web_from.c:388`：`/ws` CSV 键值对（`Hardware_version`、`Software_version`、`ubxConstellations{GPS,BDS,GLONASS,Galileo,SBAS}`、`UBX_NMEA_{GGA,GSA,GSV,GST,RMC}`、`exitToRoverMode` 无操作） | `partial`（协议不兼容 + 星座缺失） | `docs/web/ws-protocol.md`、`main/modules/web/WebLogic.cpp`、`components/settings/src/Registry.cpp:776-826`：改为 JSON 协议，支持 `gnss.rate`、`gnss.out.{gga,gsa,gsv,gst,rmc}`、`gnss.gga_gate.{enabled,min_sats}`、`bt.wl.{add,del}`、`bt.pair_next`、`factory.sn`、`save/exit/freset`；**但 `gnss.const.{gps,bds,glo,gal}`（及 SBAS）因 `Registry.cpp:799-805` 未绑定动词且 F9P `supportsSatMask=false`，网页端不可配星座**。 |
| Web OTA 升级与回滚 | `main/user_src/user_web_from.c:145-380`、`main/main.c:170`：`POST /upload` multipart 上传；校验文件名含 `"P2"` 且版本号严格递增（禁止降级）；启动即无条件 `esp_ota_mark_app_valid_cancel_rollback()`（实质无回滚保护） | `implemented`（已增强） | `main/modules/web/WebOta.cpp:156-447`、`main/modules/ota/OtaRuntime.cpp:33-136`、`main/modules/ota/OtaCmds.cpp`、`docs/adr/ADR-0019-web-ota.md`：端点改为 `GET /ota/info` + `POST /ota`（原始二进制流）；实时校验 `0xE9`/`ESP_CHIP_ID_ESP32`/`ESP_APP_DESC_MAGIC_WORD`/`project_name`；允许降级并支持完整应用回滚（`RTK_OTA_VALID_DELAY_S` 默认 30 s 后确认，启动失败自动回滚）。注：`docs/PROGRESS.md` §2 仍标注 O1/O2 进行中，但代码已合入工作树。 |

### 1.4 GNSS 配置与驱动（重点对比 u-blox ZED-F9P 与多板卡）

| 功能 | 旧固件依据（`P2_V3.1_IDF/`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| 板卡识别机制 | `main/user_src/user_eeprom.c:19-29`：按 `Equipment_PN[4..5]` 查表静态指定（`15`=F9P、`17`=UM982、`18`=UM981、`19`=CM680、`20`=UM960） | `implemented`（更优） | `components/gnss/src/Registry.cpp` + `UbloxReceiver.cpp:189-211`：运行期主动协议探测（F9P 轮询 `UBX-MON-VER` 匹配 `MOD=ZED-F9P`，不依赖手写 PN）。支持 Unicore/u-blox/ComNav/Optisens。 |
| F9P 波特率探测与切速 | `main/user_src/user_f9p.c:277-355`、`main/main.c:147`：扫描 8 档 `{4800, 9600, 38400, 57600, 115200, 230400, 460800, 921600}`，用旧版 `UBX-CFG-PRT` 切至 **`921600`** | `partial` | `main/modules/gnss/GnssModule.cpp:462-483`、`main/Kconfig.projbuild:193-207`、`components/gnss/src/drivers/UbloxUbx.cpp:475-488`：扫描 `{preferred, 115200, 460800, 921600, 230400, 38400, 9600, 19200, 57600}`（**不含 `4800`**），用 `UBX-CFG-VALSET` 切至默认 **`460800`**（仅写 RAM 层，可由 `gnss baud 921600` 覆盖）。**尚未在 F9P 实板 HIL 验证**（`components/gnss/Kconfig:8`）。 |
| F9P 端口协议与双串口（UART1 + UART2）配置 | `main/user_src/user_gnss.c:70-73,90`：同时配置 F9P `UART1` 与 `UART2`（`UBX\|NMEA` 出，`UBX\|NMEA\|RTCM3` 入），并在 `COM2 (UART2)` 开 1 Hz GGA | `partial` | `UbloxUbx.cpp:294-309` 仅配置板级表指定的单个接线口（P2 为 `receiverPort=1` 即 UART1，`BoardProfile.cpp:28`），**不配置 F9P UART2**（若 P2 板上外接模块依赖 F9P UART2 需确认，目前电路连接状态对 F9P UART2 为 `UNKNOWN`）。 |
| F9P 定位频率（`CFG-RATE` / `CFG-RATE-MEAS`） | `main/user_src/user_f9p.c`：`UBX-CFG-RATE (0x06 0x08)` | `implemented`（未过 F9P HIL） | `UbloxUbx.cpp:305-308`、`UbloxReceiver.cpp:128-139,183-184`：`CFG-VALSET` 写 `kKeyRateMeas`，支持 1/2/5/10/20 Hz。 |
| F9P NMEA 输出配置（类型与频率） | `main/user_src/user_gnss.c:92-99`、`user_f9p.c:414`：试图配置 `GGA/GSA/GSV/GST/RMC/ZDA/VTG/GLL`（注意旧版存在枚举错位 bug：把 `user_gnss.h` 枚举值直接传入 `F9P_SETdateout`） | `partial` | `UbloxReceiver.cpp:287-356`：`CFG-VALSET` 仅支持 5 句（`GGA/GST/RMC/GSA/GSV`），**不支持配置或显式关闭 `ZDA/VTG/GLL`**（F9P 出厂默认会输出 `GLL`/`VTG`）；另 `UbloxUbx.cpp:94-98` 存在 1 参 `beginValset` 少 1 个保留字节的潜在 bug（仅 `useConfiguredPort=false` 路径触发，主程序已全部置 `true`：`GnssModule.cpp:73`、`GnssOutput.cpp:163`）。 |
| F9P 星座使能/屏蔽与截止角 | `main/user_src/user_f9p.c:465-490`、`user_gnss.c:77-87`（旧版写了 `CFG-GNSS` 辅助函数，但 `F9P_unmask_init/mask_init` 实际未调用它，且蓝牙 `0x06` 误调 UM980 函数） | `missing` | `components/gnss/src/PppSequence.cpp:42-56` 明确 `supportsSatMask(Model::kZedF9p) == false`，**F9P 星座掩码与高度截止角在蓝牙 `0x06` 和网页均不可用**。 |
| F9P Base / Rover / Survey-in / Fixed Base 与 RTCM3 输出 | `main/user_src/user_f9p.c:358`：`UBX-CFG-TMODE3 (0x06 0x71)` Rover=0 / Base Survey-in（60 s, 2.0 m） | `implemented`（未过 F9P HIL） | `UbloxUbx.cpp:499-594`、`main/modules/gnss/GnssWorkMode.cpp`：完整实现 Rover / Base Survey-in / Base Fixed (LLH 高精度) 及 RTCM3（1005/1230/MSM4/MSM7），但仅在 `gnss mode` 控制台暴露，未接蓝牙 `0x07`/网页，且**未在 F9P 实板验证**。 |
| F9P 保存配置与恢复出厂 | `main/user_src/user_f9p.c:277,493`：`UBX-CFG-CFG` 清除/保存 BBR+Flash | `implemented`（未过 F9P HIL） | `UbloxUbx.cpp:409-461`：实现 `saveStep`（`CFG-CFG` BBR+Flash）与 `factoryResetStep`（`CFG-CFG` clear+load，切回 38400）。注意普通配置默认只写 RAM，需显式 `gnss save` 或网页 `save`。 |
| UM980/UM981/CM680 配置与 PPP | `main/user_src/user_um980.c`、`user_gnss.c:103-130` | `implemented` | `components/gnss/src/drivers/Unicore*`、`PppSequence.cpp:12-40`（CM680 实板已验，但存在 `PROGRESS.md` §3 所列待查项）。 |

### 1.5 按键、OLED 显示、声光振外设与本地电子围栏

| 功能 | 旧固件依据（`P2_V3.1_IDF/`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| 电源/功能单键（GPIO18）长按关机与充电待机开机 | `main/user_src/user_key.c`、`main/main.c:119-132`：长按 >2 s 关机（有 USB 进充电页）；充电待机按住约 2.5 s（`5 × 500 ms`）开机 | `implemented` | `main/board/PowerLogic.hpp/.cpp`、`main/modules/power/PowerModule.cpp`：长按 2.5 s（`RTK_POWER_OFF_PRESS_MS`）触发四阶段关机（保存→静默→断电），充电待机长按 2.5 s 保持并重启开机（P2 v1.41 CM680 主路径 HIL PASS，`docs/reports/B2-power-key.md` §9）。 |
| 单键短按 OLED 菜单（Rover / WiFi / Net / Ntrip / Device Info） | `main/user_src/user_key.c`、`main/user_src/user_oled.c`：短按翻页，2 s 无操作自动进入 | `missing` | `PowerLogic.hpp:37` 已识别 `KeyEvent::kShort`，但无任何消费方；ADR-0017 §7 与 `docs/PROGRESS.md` §2 明确 P2 OLED + 按键菜单延后。 |
| SH1106 128×64 OLED 显示驱动（I2C `0x3C`，`OLED_POWER=GPIO12`） | `main/src/xyz_oled.c:33-70,333-334`、`main/user_src/user_oled.c`（开机画面、定位/卫星/电量/充电/关机/OTA 进度/菜单） | `missing` | `main/board/BoardProfile.cpp:16` 仅登记 `Feature::kOled` 掩码，未驱动 `GPIO12` 与 `0x3C`（ADR-0015 §2.5/§5 标为后续）。 |
| 蜂鸣器（GPIO13 低有效）、震动马达（GPIO15 高有效，开机震 500 ms）、LED（GPIO23 高有效） | `main/user_src/user_gpio.c:25-27,40-49` | `missing` | `BoardProfile.cpp:10-12` 仅在启动识别后置安全关闭电平（13=H, 15=L, 23=L），无开机震动及任何运行期驱动（ADR-0015 §2.5/§5 标为后续）。 |
| 本地电子围栏（5 区块 × 20 顶点，射线法实时告警驱动 LED/蜂鸣器/马达） | `main/user_src/user_geogence.c`、`main/user_src/user_bt_message.c:687-853`、`main/log_version:27,32` | `missing` | 无围栏计算模块与存储；`PhoneCompatService.cpp:710-742` 返回空/不支持；`AlarmService` 仅用于外部蓝牙报警器协议，非本机围栏。 |

### 1.6 电池、充电芯片与电源管理序列

| 功能 | 旧固件依据（`P2_V3.1_IDF/`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| 电池电压 ADC 采集（GPIO34 `BAT_V_ADC`，×2 分压，20 次去极值均值 + 3.34–4.15 V 查表电量） | `main/src/xyz_gpio.c:157,175-190`、`main/user_src/user_battery.c:135-191` | `missing` | `RTK_main` 未读取 GPIO34（ADR-0015 §3 标为后续）；电量无法监测。 |
| 低电量自动关机（`< 10%`） | `main/main.c:207-214`、`main/user_src/user_battery.c:22-47` | `missing` | `main/board/PowerLogic.hpp:46` 仅预留 `ShutdownReason` 枚举值，无采样触发器。 |
| SGM41513 I2C 充电芯片初始化（`0x1A`）与参数配置 | `main/user_src/user_tools.c:163-201`：探测 `0x1A`，无应答静默跳过；存在则写 `REG02[5:0]=0x34 (1980 mA)`、`REG03=0xAA (120/120 mA)`；**不读 `REG08`** | `partial` | `main/board/Charger.cpp:136-261`、`BoardProfile.cpp:42-47`：初始化 I2C0 `0x1A`，写 V2.7 参数 `REG02[5:0]=51 (1860 mA)`、`REG03=0xF8 (240/80 mA)`，并轮询 `REG08`。**在 F9P P2 实板上 I2C 读写报 `io-error`**（详见 §2.3）。 |
| USB 插入/充电状态检测 | `main/user_include/user_gpio.h:21`、`main/main.c:119`、`user_battery.c:56`：读 **`VBUS_DET_ESP32` (GPIO27 高有效)**；`PWR_CHG` (GPIO2) 已初始化但被注释未用 | `partial` | `BoardProfile.cpp:36-41`、`BoardStartup.cpp:511-525`：禁用 GPIO39，**未使用 GPIO27**，依赖 SGM41513 I2C `REG08`，I2C 失败时仅退化到 GPIO2 `STAT`（充满时 `STAT` 拉高会漏检 USB，见 `BoardStartup.cpp:512`）。 |
| 开机电源保持与关机断电脉冲序列 | `main/main.c:122`（充电待机 GPIO4=LOW，**注意 `P2_V3.1_IDF` 未执行 V2.7 的开机 GPIO4/GPIO25 保持循环**）；关机：`user_key.c:145` + `user_oled.c:377-383`（GPIO4 高/低各 3.1 s 循环，低电关机先高 2 s 再交替 3.1 s） | `implemented` | `main/board/BoardStartup.cpp:256-291,502-549`、`main/modules/power/PowerModule.cpp`：完整实现 V2.7 风格的 `kKeyEmulate` 开机保持（GPIO4 拉高 + ADC2 采 GPIO25 >= 1600 mV 连续 3 次，5 s 超时保护）及四阶段安全关机与 U3 断电脉冲。 |

### 1.7 4G / EC600 蜂窝网络、倾角/IMU、电台、存储、控制台与出厂模式

| 功能 | 旧固件依据（`P2_V3.1_IDF/`） | `RTK_main` 状态 | `RTK_main` 依据与差异说明 |
| --- | --- | --- | --- |
| EC600 4G 模组驱动与状态监测（UART1 GPIO33/32，电源 GPIO26，CSQ/IMEI/SIM/CREG/CGREG） | `main/user_src/user_ec600.c`、`user_gsm.c`、`user_gsm_task.c`、`user_gsm_event.c` | `missing` | `BoardProfile.cpp:11,16` 仅将 `GPIO26` 置低关断 4G 并登记 `Feature::kCellular`；`docs/PROGRESS.md` §2 明确 4G 尚未立项（需新 ADR）。 |
| 4G NTRIP 客户端（发 GGA、收 RTCM 注入 UART2、拉挂载点） | `main/user_src/user_gsm.c:47-67`、`user_gsm_task.c`（`Socket_RTK=1`） | `missing` | 同上（`0x12` 仅做 NVS 占位存储）。 |
| 4G TCP JSON 遥测上传（`Socket_JSON=2`）与 ThingsBoard MQTT（GGA 遥测 + RPC） | `main/user_src/user_gsm_task.c`、`user_gsm_event.c`（`AT+QMTOPEN/CONN/SUB/PUB`） | `missing` | 同上（`0x32`/`0x33` 仅做 NVS 占位存储）。 |
| 倾角 / IMU / Slant（UM981 内部惯导指令） | `main/user_src/user_slant.c:7-46`（UM981 `CONFIG IMUTOANT` 等串口指令；外置 IMU614 代码已注释） | `missing` | 无 IMU/Slant 驱动，仅 `Feature::kImu` 占位与 `0x24` 天线高存储（`0x25`/`0x26` 占位）。 |
| 电台 / LoRa / UHF | 无实际硬件驱动（仅蓝牙 `0x14` 占位与 `0x01` 字段） | `implemented`（同等占位） | 无电台驱动，`ConfigService.cpp:119` 同等占位。 |
| 持久化存储（NVS / 分区表） | `partitions.csv`：`nvs`(0x5000)、`otadata`(0x2000)、`app0/app1`(各 0x1E0000)、`spiffs`(0x20000，未用)、`coredump`(0x10000)。`main/src/xyz_eeprom.c`、`user_eeprom.c`：单块 2560 B blob 存 NVS `eeprom/eeprom` | `implemented`（不兼容旧格式） | `partitions.csv`：`nvs`(0x9000)、`phy_init`、`otadata`、`ota_0/ota_1`(各 0x1E0000)、`nvs_fac`(0x6000)、`coredump`(0x10000)。改为分键存储：`fac/sn`、`rtk/*`（`StoreKeys.hpp`、`NvsStore.c`）、`gnss/*`（`GnssKeys.hpp`）、`dbg/*`。**无法直接读取旧固件 `eeprom/eeprom` 中的 SN/PN/配置**。 |
| 串口控制台 / AT 命令 | `main/main.c:108`：UART0 460800 仅 TX 日志打印，无 RX 命令行或 AT 解析 | `implemented`（大幅增强） | `main/app/ConsoleSetup.cpp:245-335`、`docs/adr/ADR-0016-console-profiles.md`：UART0 2 Mbaud，三档位（DEV/PRODUCTION/NONE），完整命令组（`status`/`log`/`sys`/`bt`/`alarm`/`config`/`rtcm`/`gnss`/`debug`/`factory`/`freset`/`power`/`radio`/`ota`）。 |
| 出厂 / 测试模式 | 无专用控制台出厂模式；通过蓝牙 `0x08` 写 SN/PN、`0x1E` 恢复出厂；`user_key.c` 含未接入主菜单的 motor/beep/LED 测试页 | `implemented`（形式不同） | `main/modules/system/FactoryCmds.cpp`、`FactoryReset.cpp`：提供带口令解锁的 `factory` 命令组（写 `nvs_fac` 出厂 SN、报警器白名单、`factory erase yes`）、`freset yes` 及网页解锁入口；保留 `0x08`/`0x1E` 手机路径。无 motor/beep/LED 自测命令（因驱动未实现）。 |
| SD 卡 / SPIFFS | `main/idf_component.yml` 引用 `sd_card`、`partitions.csv` 划 `spiffs`，源码零调用 | `intentionally dropped` | `main/Kconfig.projbuild:229-234` 明确静态观测/SD/SPI 仅作关闭占位，不划 `spiffs`。 |

---

## 2. 分硬件版本引脚与外设全量对比及充电芯片异常根因分析

### 2.1 硬件版本分支机制说明

- **旧固件（`P2_V3.1_IDF`）**：
  - 全仓库 `main/` 下**无任何硬件版本条件编译分支**（仅有 `BT_WIFI_AUTO_SWITCH` 功能开关，见 `main/src/xyz_webserver.c:17`）。
  - `Hardware_version = "V1.3"`（`main/main.c:8`）仅用于蓝牙 `0x01`、OLED 设备信息页和 WebSocket 展示字符串（`user_bt_message.c:903`、`user_oled.c:303`、`user_web_from.c:388`）。
  - 所有板级引脚在编译期写死，不读 `GPIO35` 分压。
- **新固件（`RTK_main`）**：
  - 按 `docs/adr/ADR-0015-board-profiles.md` 启动第 1 步执行 `identify()`（`main/board/BoardStartup.cpp:442-479`），通过 ADC1 采样 `GPIO35 (DEVICE_SENSE)` 匹配 `kProfiles`（`main/board/BoardProfile.cpp:21-56`）：
    - `P2 v1.41`：`idMinMv=1500, idMaxMv=1800`（R134/R135 = 100k/100k，标称 1650 mV）。
    - `E1 DevKit`：`idMinMv=0, idMaxMv=0`（不参与自动识别，仅台架 `CONFIG_RTK_BOARD_FORCE_E1_DEVKIT` 强制）。
  - ADR-0015 §1 明确声明“P2 v1.41 之前的板已停产，不兼容”。

### 2.2 引脚与外设逐项对比表

| 功能 / 外设 | 旧固件（`P2_V3.1_IDF`，标注 `V1.3`，无版本分支） | `RTK_main`（`P2 v1.41` 配置表） | `RTK_main`（`E1 DevKit` 配置表） | 一致性（Legacy vs P2 v1.41） |
| --- | --- | --- | --- | --- |
| 板卡版本识别 ADC | 未使用（硬编码 `"V1.3"`） | `GPIO35`（`DEVICE_SENSE`，ADC1_CH7，12 dB，1500–1800 mV，`BoardStartup.cpp:23-26,442`） | 仅读并打印 mV，靠 Kconfig 强制 | **不适用 / 新增**（旧板无 R134/R135 分压时 `identify()` 会因未知 ID 降级 console-only） |
| 控制台 UART0 | `UART0`，TX=`GPIO1`，RX=`GPIO3`，**460800** 8N1，仅 TX（`main/main.c:108`） | `UART0`，TX=`GPIO1`，RX=`GPIO3`，**2000000** 8N1，完整 REPL（`docs/adr/ADR-0009-trace-console.md`） | 同左 | **波特率不同**（460800 vs 2 Mbaud），引脚一致 |
| GNSS UART | `UART2`，RX=`GPIO16`，TX=`GPIO17`；先 115200 后扫 8 档切 **921600**（`user_gnss.c:56`、`main.c:147`） | `UART2`，RX=`GPIO16`，TX=`GPIO17`，`receiverPort=1`，默认探 115200 后切 **460800**（`BoardProfile.cpp:28`、`Kconfig.projbuild:193-207`） | `UART2`，RX=`GPIO16`，TX=`GPIO17`，`powerGpio=25` 高有效 | **引脚一致；目标波特率不同**（921600 vs 460800） |
| 4G 模组 UART | `UART1`，TX=`GPIO33`，RX=`GPIO32`；115200 → 460800（`user_gsm.c:10,19`） | **未初始化**（ADR-0015 §3 标为后续） | 未使用 | **缺失（未实现）** |
| 4G 电源使能 `GSM_PWREN` | `GPIO26`，输出：启动早置 LOW，退出充电待机后置 HIGH（`user_ec600.h:8`、`user_gpio.c:19,28`） | `GPIO26`，输出：**始终置 LOW**（`kP2SafeOutputs`，`BoardProfile.cpp:11`） | `GPIO26` 用作 E1 电源保持高有效 | **引脚一致，电平行为不同**（新固件保持关闭 4G） |
| I2C 总线 | `I2C_NUM_0`，SDA=`GPIO21`，SCL=`GPIO22`，**400 kHz**，**开启内部上拉**（`GPIO_PULLUP_ENABLE`），超时 1000 ms（`xyz_iic.h:7-13`、`xyz_iic.c:11-18`） | `I2C_NUM_0`，SDA=`GPIO21`，SCL=`GPIO22`，**100 kHz**，**关闭内部上拉**（`enable_internal_pullup=0`），超时 10 ms（`BoardProfile.cpp:42-44`、`Charger.cpp:23-24,162`） | 未使用（`-1`） | **引脚一致；速率/上拉/超时不同**（关键排查项） |
| 充电芯片 SGM41513 | I2C0 7-bit **`0x1A`**；探测失败静默返回 0；`REG02[5:0]=0x34 (52→1980 mA)`，`REG03=0xAA (120/120 mA)`；不读 `REG08`（`user_tools.c:163-201`） | I2C0 7-bit **`0x1A`**；写 `REG02[5:0]=51 (1860 mA)`，`REG03=0xF8 (240/80 mA)`；启动及运行期 200 ms 读 `REG08`（`BoardProfile.cpp:45-47`、`Charger.cpp:222-261`） | 未使用 | **地址一致；充电参数不同，且新固件强依赖 I2C 读 `REG08`** |
| OLED 屏幕 SH1106 | I2C0 7-bit **`0x3C`**；电源 `OLED_POWER=GPIO12`（`GPIO_MODE_INPUT_OUTPUT` 置 HIGH，`xyz_oled.h:8`、`xyz_oled.c:333-334`） | **未驱动**（`GPIO12` 不触碰，ADR-0015 §2.5/§3 标为后续） | 未使用 | **缺失（未实现）** |
| 电源模拟按键 `TELL_MCU_KEY` | `GPIO4`，输出：充电待机 LOW；关机先 HIGH 再高/低 3.1 s 循环（`user_gpio.h:24`、`main.c:122`、`user_oled.c:377-383`） | `GPIO4`，输出：开机保持拉高至 GPIO25 达标后拉低；关机高/低脉冲断电（`BoardProfile.cpp:30`、`BoardStartup.cpp:270-282`、`PowerModule.cpp`） | 未使用 | **引脚一致；新固件补齐开机保持流程** |
| 电源通路检测 `ON_OFF_ADC` | `GPIO25` 定义在 `user_gpio.h:25`，但 `user_gpio.c:15` 已注释，**未实际读取** | `GPIO25`，ADC2_CH8（12 dB，校准 mV，阈值 `senseOnMv=1600 mV`），启动早期使用（`BoardProfile.cpp:31-32`、`BoardStartup.cpp:256-348`） | `GPIO25` 用作 E1 GNSS 电源使能 | **引脚一致；旧固件未启用，新固件启用** |
| 电源/菜单按键 `key_pin` | `GPIO18`，输入 + 内部上拉，低有效（`user_gpio.h:14`、`user_gpio.c:11`） | `GPIO18`，输入，低有效（`BoardProfile.cpp:35,39`、`PowerModule.cpp:44-55` 无内部上下拉，依赖外置电路） | 未使用（`-1`） | **引脚与有效电平一致；上拉配置不同**（旧开内部上拉，新关闭内部上拉） |
| USB 在位检测 `VBUS_DET_ESP32` | **`GPIO27`**，纯输入无上下拉，**高有效**，作为全部 USB/充电检测唯一依据（`user_gpio.h:21`、`main.c:119`、`user_battery.c:56`） | **未使用**（`usbDetectGpio=-1`，ADR-0015 §3 标“后续”） | 未使用（`-1`） | **关键不一致 / 缺失！**（旧固件用 GPIO27，新固件未接） |
| USB 检测 `USB_Detec`（SGM41513 nPG） | 未使用（代码无 GPIO39 引用） | `GPIO39` 已在实板测得插拔恒 0 V 且固件读高，**主动禁用**（`usbDetectGpio=-1`，`BoardProfile.cpp:36-37`） | 未使用 | 一致（均不用 GPIO39） |
| 充电状态 `PWR_CHG`（STAT） | `GPIO2`，输入+上拉（`user_gpio.c:17`），**被注释未读取**（`user_battery.c:56`） | `GPIO2`，纯输入无上下拉（板载 R237 10k 上拉），低有效，仅作 I2C 故障退化兜底（`BoardProfile.cpp:38,41`、`BoardStartup.cpp:523`） | 未使用（`-1`） | **引脚一致；使用方式不同** |
| 电池电压 ADC `BAT_V_ADC` | **`GPIO34`**，ADC1_CH6，11 dB，12 bit，×2 分压（`user_gpio.h:22`、`xyz_gpio.c:157,175-190`、`user_battery.c:177-191`） | **未使用**（ADR-0015 §3 标“后续”） | 未使用 | **缺失（未实现）** |
| 蜂鸣器 `BEEP_PWREN` | `GPIO13`，输出，低有效（ON=LOW, OFF=HIGH，`user_gpio.h:17,19-20`） | `GPIO13`，输出：仅启动置安全电平 HIGH（关闭，`BoardProfile.cpp:10`） | 未使用 | **引脚与极性一致；无运行期驱动** |
| 震动马达 `MOTOR_PWREN` | `GPIO15`，输出，高有效（`user_gpio.h:16`、`user_gpio.c:25-27,42-43`） | `GPIO15`，输出：仅启动置安全电平 LOW（关闭，`BoardProfile.cpp:10`） | 未使用 | **引脚与极性一致；无运行期驱动** |
| 指示灯 `LED_PWREN` | `GPIO23`，输出，高有效（`user_gpio.h:18`、`user_gpio.c:48-49`） | `GPIO23`，输出：仅启动置安全电平 LOW（关闭，`BoardProfile.cpp:11`） | 未使用 | **引脚与极性一致；无运行期驱动** |
| SPI / SD 卡 | 未使用（`idf_component.yml` 引用但无引脚/代码） | 未使用 | 未使用 | 一致（均无 SPI） |

---

### 2.3 实板日志根因分析：F9P 联调 P2 v1.41 报充电芯片 I2C 错误

实板现象（P2 v1.41 + F9P 运行 `RTK_main`）：
1. `"charger charge params apply failed (io-error)"`（对应 `main/board/BoardStartup.cpp:369`）
2. `"charger REG08 read failed, falling back to STAT pin"`（对应 `main/board/BoardStartup.cpp:376`）

#### 已核实代码与硬件事实

1. **驱动与总线已成功创建，失败发生在首笔 I2C 事务**：
   - `charger::init()`（`main/board/Charger.cpp:136-185`）未报错，说明 `i2c_new_master_bus(I2C0, SDA=21, SCL=22)` 与 `i2c_master_bus_add_device(0x1A)` 成功（ESP-IDF v6.1 添加设备不做总线探测）。
   - 紧接着 `applyChargeParams()` 执行首笔读操作 `readRegLocked(0x02)` → `i2c_master_transmit_receive(..., 10 ms)` 返回非 `ESP_OK`；随后 `refreshInputStatus()` 读 `0x08` 同样失败（`Charger.cpp:91-101,233,250`）。
2. **错误码 `io-error` 的精确含义**：
   - `components/rtos/include/rtos/EspStatus.hpp:19-24` 将 `ESP_ERR_TIMEOUT` 映射为 `kTimeout`（`"timeout"`），而将未显式映射的 **`ESP_FAIL`**（ESP-IDF `i2c_master` 在收到 **Address/Data NACK** 或总线仲裁/起始异常时返回 `ESP_FAIL`）映射为 **`kIoError`（`"io-error"`）**。
   - 因此，实板日志为 `(io-error)` 而非 `(timeout)`，说明 **I2C 事务发出了但地址 `0x1A` 未应答（NACK）或总线电平异常**，而非时钟线被长期拉死超时。
3. **同份代码在 CM680 P2 v1.41 上已实测通过**：
   - `docs/reports/B2-power-key.md:228` 记录在 CM680 的 P2 v1.41 板上 `RTK_main` 成功读出 `charger reg08=0x54`（`usb=1 src=i2c`），证明 `RTK_main` 的 I2C0 引脚（21/22）与 SGM41513 7-bit 地址 `0x1A` 驱动本身无硬性代码缺陷。
4. **旧固件为何在同块 F9P 板上“不报错且 USB 检测正常”**：
   - 旧固件 `SGM41513_init()`（`P2_V3.1_IDF/main/user_src/user_tools.c:194-201`）在读写前先执行 `i2c_check_device_connected(0x1A)`，**若 `0x1A` 无应答直接静默 `return 0`，不打任何日志**。
   - 旧固件**从不通过 I2C 读 `REG08`**，全部 USB/充电判定走独立 GPIO **`VBUS_DET_ESP32`（GPIO27 高有效）**（`main/main.c:119`、`user_battery.c:56`）。

#### 推断的可能根因（按可能性排序）

1. **【最可能】该 F9P 实板的充电芯片 BOM / 型号或 I2C 地址不是 `0x1A`（或未接 I2C / 未贴 SGM41513）**：
   - `docs/hardware/P2/schematic/p2v1.41.pdf` 修订记录显示 P2 在 2025 年至少两次更换充电芯片：`2025.4.27`（“更换充电芯片及配套电路，删除充电检测改为读充电芯片io，适配快充芯片”，第 756–758 行）与 `7.25`（“1、更换充电芯片及配套电路；2、删除esp32下载口转usb电路，改为接F9P的USB口”，第 749–751 行）；且 `P2_V3.1_IDF/main/log_version:18` 在 `2025.7.15` 才加入 SGM41513 快充支持，并刻意对 `0x1A` 探测失败静默兼容旧板。
   - 虽然原理图 `p2v1.41.pdf` 第 986 行标为 `SGM41513YTQF24G/TR`（地址 `0x1A`），但这块带 F9P 的样板可能使用了不同批次/替代料充电芯片（如 BQ25601/BQ25601D 地址 `0x6B`，或非 I2C 充电芯片，或 SDA/SCL 串联电阻未贴/虚焊）。**该块 F9P 实板实际贴装的充电芯片型号与 I2C 地址为 `UNKNOWN`**（需 I2C 0x08–0x77 扫描或目视板面丝印核实）。
2. **【次要可能】I2C 上拉配置与总线电平差异**：
   - 旧固件 `xyz_iic.c:15-16` 显式开启 ESP32 内部上拉（`GPIO_PULLUP_ENABLE`），而 `RTK_main` `Charger.cpp:162` 显式关闭内部上拉（`enable_internal_pullup = 0U`），假定板上 `R66/R68`（4.7 kΩ）已贴且接 `VCC_3V3`。若该 F9P 板上 `R66/R68` 未贴、供电轨未上电、或被同总线器件（如断电态 SH1106 OLED 钳位；注：旧固件也是在 `OLED_init()` 之前调用 `SGM41513_init()`，`main.c:116-117`，但旧固件开了内部上拉），也会引发 NACK/`ESP_FAIL`。
3. **【叠加隐患】参数差异与退化缺陷**：
   - 即使换回 `0x1A` 芯片，`RTK_main` 写入的 `REG02=51 / REG03=0xF8`（源自 V2.7）与 `P2_V3.1_IDF` 的 `REG02=0x34(52) / REG03=0xAA` 不一致，需核对电池规格。
   - 一旦 I2C 失败，`RTK_main` 因未接入 `VBUS_DET_ESP32 (GPIO27)`，仅能退化到 `PWR_CHG (GPIO2 STAT)`，在电池充满（STAT 拉高）时无法识别 USB 在位（`BoardStartup.cpp:512`），将导致充电待机/插 USB 关机逻辑错判。

---

## 3. 按优先级排列的差距清单

### P0（阻断 P2 + F9P 硬件正常工作或引发严重硬件/电源故障）

1. **充电芯片 I2C 失败 + 缺失 `VBUS_DET_ESP32`（GPIO27）USB 检测**
   - **证据**：F9P P2 实板实测报 `charger charge params apply failed (io-error)` 与 `charger REG08 read failed, falling back to STAT pin`（`BoardStartup.cpp:369,376`）；`RTK_main` 将 `usbDetectGpio` 置 `-1` 且未读 GPIO27（`BoardProfile.cpp:37`），而旧固件始终使用 GPIO27（`user_gpio.h:21`、`main.c:119`）。
   - **影响**：无法配置快充电流；电池充满时 STAT 为高，关机插 USB 会误全功能开机，运行中插 USB 关机行为异常（`BoardStartup.cpp:512`）。
   - **待核实**：需确认该 F9P 实板充电 IC 丝印/I2C 地址（`UNKNOWN`）及 `R66/R68` 上拉与 `GPIO27` 连通性；建议同时评估恢复内部上拉或兼容备选地址，并核实 GPIO27 作为独立 VBUS 辅助检测。

2. **u-blox ZED-F9P 驱动从未在实板 HIL 验证 + 星座/截止角完全不可用 + 若干协议缺口**
   - **证据**：
     - `components/gnss/Kconfig:8`、`UbloxReceiver.cpp:3` 明确标注 `UbloxReceiver` 未经实板验证。
     - `components/gnss/src/PppSequence.cpp:42-56`：`supportsSatMask(Model::kZedF9p)` 返回 `false`，导致 F9P 无法通过蓝牙 `0x06` 或网页配置 GPS/BDS/GLO/GAL/SBAS 与截止角。
     - `UbloxReceiver.cpp:287-356`：仅支持 5 句 NMEA（无 `ZDA/VTG/GLL`，也未显式关闭 F9P 出厂默认 `GLL/VTG`）；未配置 F9P `UART2`；`readMonVer` 未提取 `boardSn`；`UbloxUbx.cpp:94-98` 保留了 3 字节头（缺第 2 个保留字节）的 1 参 `beginValset` 隐患。
   - **影响**：P2 + F9P 量产前存在探测/切速/输出计划/RTCM 注入/基站模式全链路未实测风险，且用户无法控制 F9P 卫星系统与截止角。

3. **电池电压采集（GPIO34 `BAT_V_ADC`）与低电量自动关机（`<10%`）完全缺失，蓝牙 `0x03` 恒报未知电量**
   - **证据**：旧固件 `user_battery.c:22-47,135-191`、`main.c:207-214` 依赖 GPIO34 实现电量换算与 `<10%` 自动断电保护；`RTK_main` 未采样 GPIO34（ADR-0015 §3 标后续），`DeviceService.cpp:151-164` / `Reply.hpp:68-69` 恒上报 `batteryPct=0xFF`、`onBattery=1`。
   - **影响**：电池过放无软件保护，且手机 App 无法显示 P2 电量与充电状态。

4. **设备信息 `0x01` 硬件版本号硬编码与充电电流参数不一致**
   - **证据**：`DeviceService.cpp:215` 硬编码 `hwVer="V1.1.0"`（未从 `BoardProfile` 更新 P2 版本）；`BoardProfile.cpp:46-47` 充电参数为 `51 (1860 mA) / 0xF8 (240/80 mA)`，而 `P2_V3.1_IDF/main/user_src/user_tools.c:166,189` 为 `52 (1980 mA) / 0xAA (120/120 mA)`。
   - **影响**：上位机/App 读取到错误硬件版本与空 GNSS SN；充电截止/预充电流需与电池包规格对齐（以硬件规格书为准，当前最优值 `UNKNOWN`）。

---

### P1（用户可见功能缺失或显著行为不兼容）

1. **SH1106 OLED 显示（`0x3C`、`GPIO12`）与单键短按菜单（GPIO18）缺失**
   - **证据**：旧固件提供完整状态屏（定位/卫星/电量/充电/OTA 进度）与 5 项单键菜单（含 Wi-Fi 切换入口，无需手机 App），见 `user_oled.c`、`user_key.c`；`RTK_main` 仅消费长按关机/开机，短按与 OLED 延后（ADR-0017 §7、`PROGRESS.md` §2）。
   - **影响**：手持机脱离手机时成为“盲盒”，无法查看定位/电量/充电状态，也无法不借助手机或串口进入 Wi-Fi 配置/OTA 模式。

2. **EC600 4G 模组与内置网络业务（NTRIP 客户端、TCP JSON 遥测、ThingsBoard MQTT）全部缺失**
   - **证据**：旧固件通过 `UART1 (GPIO33/32)` + `GPIO26` 实现 4G 联网、主机内置 NTRIP RTK（`0x11`/`0x12`）、JSON 遥测（`0x32`）和 ThingsBoard MQTT（`0x33`）（`user_ec600.c`、`user_gsm*.c`）；`RTK_main` 仅在 `PhoneCompatService.cpp` 做 NVS 存储占位，4G 未立项（`PROGRESS.md` §2）。
   - **影响**：依赖主机内置 4G 联网获取差分或上报定位的场景不可用（仅能依赖手机蓝牙转发 RTCM）。

3. **声光振反馈（蜂鸣器 GPIO13、马达 GPIO15、LED GPIO23）缺失**
   - **证据**：旧固件开机马达震动 500 ms 提示，运行期驱动声光振告警（`user_gpio.c:25-27,40-49`）；`RTK_main` 仅将其锁定在安全关闭态（`BoardProfile.cpp:10-12`）。
   - **影响**：无开机触觉反馈与状态灯光提示（在无 OLED 阶段尤为明显）。

4. **本地电子围栏（5 区 × 20 点 + 越界声光振告警 + `0x60`/`0x61`/`0x62`）缺失**
   - **证据**：旧固件 `user_geogence.c`、`user_bt_message.c:687-853`；`RTK_main` `PhoneCompatService.cpp:710-742` 仅回空/失败占位。
   - **影响**：Nav App 的本地电子围栏功能无法使用。

5. **蓝牙协议与连接体验不兼容项**
   - **证据**：
     - 蓝牙配对从 `ESP_BT_IO_CAP_NONE` 改为 `ESP_BT_IO_CAP_IO`（`SppServerInitBt.c:116`），旧手机 App 若未适配配对窗口流程可能首连受阻。
     - `0x07` 工作模式在 `ConfigService.cpp:97-110` 仅写内存不真正切换 Rover/Base。
     - Raw NMEA 直推缺少 `ZDA/VTG/GLL`（`BtLinkGnss.cpp:36-40`）。
     - 网页星座配置 `gnss.const.*` 未接通（`Registry.cpp:799-805`）。
   - **影响**：旧 App 部分配置页静默失效或交互变化。

6. **Web 配置入口与 NVS 存储格式与旧固件不兼容**
   - **证据**：SoftAP IP 由 `192.168.4.1` 无密码改为 `192.168.10.12` WPA2-PSK，WebSocket 由 CSV 改为 JSON，OTA 由 `/upload` 改为 `/ota`；NVS 由 `eeprom/eeprom` 改为 `nvs_fac` + `rtk/*` + `gnss/*`。
   - **影响**：旧设备直接刷入 `RTK_main` 后**原有 SN/PN/配置不会自动迁移**，需重新通过产线 `factory sn` 或 `0x08` 写入；用户手册与运维脚本需同步新热点地址与口令。

---

### P2（锦上添花 / 低优先级差异）

1. **UM981 内部倾角/Slant 串口指令（`0x24`/`0x26` 联动）**
   - **证据**：旧固件 `user_slant.c:7-46` 仅服务于 UM981，不适用于 F9P；`RTK_main` 已保留 `0x24` 天线高持久化（`PhoneCompatService.cpp:523`），`0x26` 占位。
2. **F9P 波特率细节（`4800` 候选与 `921600` 默认目标速率）**
   - **证据**：旧固件探测含 `4800` 且默认切 `921600`（`user_f9p.c:279`、`main.c:147`）；`RTK_main` 默认探测不含 `4800` 且切 `460800`（`GnssModule.cpp:464`、`Kconfig.projbuild:203`）。460800 对 20 Hz NMEA+RTCM 通常足够，必要时可通过 `gnss baud 921600` 切换。
3. **硬件按键内部上拉防御性配置**
   - **证据**：旧固件 `GPIO18` 使能内部上拉（`user_gpio.c:11`），`RTK_main` 关闭内部上拉（`PowerModule.cpp:51`）。若硬件已有外置上拉则无影响；若个别板卡漏贴外置上拉则存在悬空误触发风险（当前 P2 v1.41 实测正常，`B2-power-key.md` §9）。
4. **旧固件无效/冗余残留（无需移植）**
   - **证据**：`P2_V3.1_IDF` 中未使用的 `sd_card` 依赖、`spiffs` 分区、`exitToRoverMode` 空操作、注释掉的外置 IMU614 代码、以及 `0x13`/`0x14`/`0x15`/`0x25`/`0x29` 协议占位（其中协议占位在 `RTK_main` 中已等价兼容）。
