# ADR-0017：Wi-Fi 网页配置与蓝牙/Wi-Fi 射频切换

状态：**已批准**（2026-11-19，用户答复 §7 Q1–Q4）。由 W 系列任务卡实施（见 §6）。
关联：ADR-0011（启动步骤）、ADR-0013 §2.4（spp_server 线性启动/拆除）、ADR-0014（运行期配置键）、
ADR-0015（P2 板级识别）、ADR-0016 Q1（量产需要控制台以外的工厂写入通道）。

> 命名：W = Wi-Fi / Web。

## 1 背景

- ADR-0016 Q1：P2 量产固件用 PRODUCTION 控制台，但 UART0 不对用户引出；SN、白名单、配对窗口的写入需要别的通道。
  用户（2026-11-19）选网页：设备开 Wi-Fi 热点，浏览器打开配置页，查看控制台能看到的状态并修改配置。
- 参考旧 P2 V2.7 `AP-Config/`：深色单页（背景 `#272b30`、卡片 `#262626`、强调色 `#83ca98`、最大宽 600 px、
  可折叠分组按钮），GNSS 星座勾选、NMEA 各语句频率下拉、保存/退出、固件上传；WebSocket `ws://192.168.4.1/ws`，
  报文为 `id,value,` 平铺 CSV。页面依赖 bootstrap + jQuery（未压缩约 380 KB）。
- 旧固件的射频切换教训（`P2_V2.7/Bluetooth.ino`、`screen.ino`）：
  1. 运行中 `SerialBT.end()` 之后再 `begin()`，手机一连就 `assert failed: hash_map_set hash_map.c:129`，
     结论是“拆过的蓝牙只能整机重启恢复”。
  2. 另一种做法 `btSuspendForWifi()` 保留协议栈、只停广播，前提是 BT 与 Wi-Fi 同时常驻时堆够用；
     旧代码在空闲堆 < 30 KB 时打印 `LOW HEAP`，因为 Bluedroid 堆不足时直接 assert 而不是返回错误。
- 本工程现状：
  - `spp_server` 的拆除路径（`components/spp_server/SppServerInit.c`）注释写明“本接口之后仅允许重启”，与上面第 1 条一致。
  - 只开 BR/EDR（`CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y`，BLE 内存启动时已释放）；Bluedroid 固定在 core 0。
  - P2 DEV 档启动后空闲堆 82.3 KB（PRODUCTION 94.0 KB，NONE 110.9 KB，ADR-0016 §3）。
  - 镜像 870 KB，app 槽 3 MB（ADR-0014 §2.1），flash 余量充足；IRAM/DRAM 是瓶颈。
  - 模块之间已有命令总线：`fw::Command`（verb + args）→ 各模块 `onCommand` → `fw::CommandResult`，控制台命令就是经它提交
    （例如 `main/modules/gnss/GnssCmds.cpp` 的 `submitGnssAndWait`）。状态在 DataCenter Slots 里（`main/data/AppData.hpp`）。

## 2 决定：射频互斥，按启动选择（需重启切换）

### 2.1 两种射频模式

| 模式 | 蓝牙 | Wi-Fi | 用途 | 默认 |
|---|---|---|---|---|
| `kBluetooth` | SPP 服务（现状） | 不初始化 | 正常工作：手机/报警器 | **是** |
| `kWifiConfig` | 不初始化，启动时 `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` 把控制器保留内存还给堆 | SoftAP + HTTP + WebSocket | 网页查看状态、改配置、工厂写入 | 否 |

- 同一次上电内**只初始化一种**射频，切换一律走 `esp_restart()`。理由：
  - 运行中拆蓝牙再建在旧固件上会崩，我们的 `spp_server` 也规定拆除后只能重启；
  - 两者常驻时，DEV 档按估算只剩约 20–30 KB 堆（W0 实测确认），已到旧固件出 assert 的区间；
  - `esp_bt_controller_mem_release` 不可逆，只有“本次上电不会再用蓝牙”时才能释放，这正好对应按启动选择。
- 代价：切换一次约 1–2 s 重启；Wi-Fi 模式下手机蓝牙断开，手机下发 RTCM 中断（配置时可接受）。
- GNSS 在两种模式下都正常运行（kIo 不受影响），网页能看到实时定位状态。

### 2.2 模式请求与回落

- 切换请求写进 `RTC_NOINIT` 内存的一次性记录 `{magic, mode, reason, crc}`，然后 `esp_restart()`；
  启动时新步骤 `radio`（在 `earlyConfig` 之后、`alloc` 之前）读取并**立即清除**它。
- 掉电、看门狗、panic 后记录无效 → 回到 `kBluetooth`。这样设备不会因为一次误操作永久停在 Wi-Fi 模式，也不写 NVS。
- Wi-Fi 模式自动回落：没有终端连上热点满 `RTK_WIFI_IDLE_EXIT_S`（默认 300 s），或最后一个终端断开后满同一时长，
  请求 `kBluetooth` 并重启。网页“退出”按钮做同样的事。
- 请求入口（§7 Q2）：
  - **手机协议新指令“进入网页配置”**：P2 的正式入口。设备先回 ACK，等 SPP 发送队列清空（最多 500 ms）再重启，
    手机端据此提示用户连接热点。需在冻结的 `components/device_proto/include/**` 增加指令号，由 Architect 在 W6 提交并重算
    `docs/frozen.sha256`；
  - 控制台 `radio wifi|bt`（DEV 档，台架与自动化测试用）；
  - P2 的 I2C OLED + 按键菜单以后再做（用户 2026-11-19：现在不做），届时只需再调用同一个请求接口。
- 模块按模式启用：`bt_link` 在 `kWifiConfig` 下不初始化、不轮询；新模块 `web` 只在 `kWifiConfig` 下启用。
  这是 ADR-0015 留下的“按特性跳过模块”的第一个用例，用 `module_list.inc` 增加“启用条件”列实现，不在模块内部判断。

### 2.3 Wi-Fi 与 HTTP

- SoftAP：SSID 与蓝牙名相同（`E1-Lite-XXXX`），信道 1–11 中固定一个，最多 2 个终端，IP `192.168.4.1/24`，DHCP 服务开启。
  WPA2-PSK，**所有设备同一个口令**（§7 Q3），由 Kconfig `RTK_WIFI_AP_PASSWORD` 给出（8–63 个字符，编译期
  `static_assert` 检查长度）；默认值 `12345678`（用户 2026-11-19 指定的量产口令）。
  工厂分组另需解锁口令（与控制台 `factory unlock` 相同）。
- `esp_http_server`（IDF 自带，开 `CONFIG_HTTPD_WS_SUPPORT`），1 个监听任务，固定在 core 0，最多 3 个套接字。
  它和 lwIP 在 IDF 内部分配堆，与 Bluedroid 一样记入 G7b 白名单；本工程代码不分配堆。
- 路由：`GET /` 返回 gzip 压缩的单页；`GET /ws` 升级为 WebSocket；其余 404。可选 DNS 劫持做强制门户（W2 视资源决定）。

### 2.4 网页（参考旧样式重写）

- 保留旧页面的视觉：深色、`#83ca98` 强调色、600 px 单列、可折叠分组按钮、行内红/绿提示。
- **不用 bootstrap/jQuery**：手写 HTML + CSS + 原生 JS，打成一个文件，构建时 gzip 后用 `EMBED_FILES` 嵌入。
  预算：gzip 后 ≤ 24 KB。图标字体换成 Unicode/内联 SVG。
- 资源放 `main/web/ui/`（源文件）；构建用 CMake 调 Python 标准库 `gzip`，不引入 npm。
  浏览器端可用 `tools/web_mock/`（Python 标准库实现的假 WebSocket 服务）离线调页面。

### 2.5 WebSocket 协议

- 文本帧 JSON，扁平、键固定，便于无堆解析：
  - 设备 → 浏览器：`{"t":"status",...}` 每 1 s 推一次（只发变化的组）；`{"t":"config",...}` 连接时与修改后推送；
    `{"t":"ack","id":N,"ok":true|false,"err":"..."}`。
  - 浏览器 → 设备：`{"t":"set","id":N,"key":"gnss.rate","value":"10"}`、`{"t":"cmd","id":N,"name":"reboot_bt"}`、
    `{"t":"unlock","id":N,"token":"..."}`。
- 解析与键表放新的纯组件 `components/web_proto`（不含 IDF 头，host 测试 + ASan）：固定长度 JSON 分词器、输出器、
  键 → (模块, verb, 参数编码, 校验范围) 表。
- **修改走现有命令总线**：`set` 转成 `fw::Command` 提交给拥有该配置的模块，与控制台同一条校验和保存路径，
  网页不直接写 NVS。这样控制台、网页、以后的手机指令共用一套规则。
- 单帧 ≤ 1 KB，超长拒收；一次只处理一条请求，结果回 `ack` 后再接下一条。

### 2.6 页面内容（第一版）

| 分组 | 显示（只读） | 可改 | 对应控制台 |
|---|---|---|---|
| 设备 | 板型、固件版本、SN、蓝牙名、射频模式、运行时间、空闲堆、复位原因 | — | `status`、`sys` |
| GNSS | 状态、型号、固件、定位类型、卫星数、hAcc、各语句实际频率 | 定位频率 1/2/5/10/20 Hz、GGA/GSA/GSV/GST/RMC 频率、星座勾选（W0 先在 CM680 上验证命令）、保存 | `gnss status`、`gnss rate`、`config gnss` |
| 蓝牙 | 白名单、策略、上次连接 | 白名单增删、下次蓝牙启动时打开配对窗口 | `bt`、`config link` |
| 工厂（解锁后显示） | — | SN、恢复出厂 | `factory`、`freset` |
| 系统 | 构建信息（含控制台档位） | 退出到蓝牙模式（重启） | `config build` |

固件上传（OTA）不在第一版（§7 Q4），以后另立卡。

## 3 资源预算（W0 实测后填写）

| 构建 / 模式 | IRAM | DRAM 静态 | 镜像 | 启动空闲堆 |
|---|---|---|---|---|
| 现状 DEV（仅蓝牙） | | 73,647 B | 869,680 B | 82.3 KB |
| 链入 Wi-Fi，`kBluetooth` 模式 | | | | |
| 链入 Wi-Fi，`kWifiConfig` 模式（BT 内存已释放） | | | | |
| 对照：BT + Wi-Fi 同时常驻（只测不用） | | | | |

验收门槛（W0 结束时决定能否继续）：`kBluetooth` 模式空闲堆不比现状少 8 KB 以上；`kWifiConfig` 模式一个浏览器连上并
打开页面后空闲堆 ≥ 40 KB；IRAM 有余量且 G3 全部构建通过。不满足就先调 Wi-Fi 缓冲（静态 RX 缓冲个数、AMPDU 关闭）再评估。

## 4 构建与冻结文件

- 打开 Wi-Fi / httpd / WS 需改 `sdkconfig.defaults`（冻结），由 Architect 在 W0/W1 提交并重算 `docs/frozen.sha256`。
- 新 Kconfig：`RTK_WIFI_CONFIG_ENABLE`（默认 y）、`RTK_WIFI_IDLE_EXIT_S`、`RTK_WIFI_AP_CHANNEL`、`RTK_WIFI_AP_PASSWORD`。
  关掉 `RTK_WIFI_CONFIG_ENABLE` 时不链入 Wi-Fi，镜像与现状一致（新增门禁 `G3-idf-wifi-off`）。
- 分区表不改；OTA 槽已存在但本版不用。

## 5 不做的事

- 不做 Wi-Fi STA（连路由器）、NTRIP、MQTT；这些以后另立 ADR。
- 不做 BT 与 Wi-Fi 同时常驻的运行模式。
- 不改蓝牙名规则（AGENTS.md）。

## 6 任务拆分（分支 + 并行，按 2026-11-19 的协作规则）

| 卡 | 内容 | 依赖 | 建议执行者 | 分支 |
|---|---|---|---|---|
| W0 | 资源探测：链入 Wi-Fi 的各模式 IRAM/DRAM/堆实测，CM680 星座命令验证，填 §3 | 无 | Architect + Codex | `spike/w0-wifi-budget` |
| W1 | 射频模式框架：`radio` 启动步骤、RTC 一次性请求、按模式启用模块、BT 内存释放、控制台 `radio`、空闲回落 | W0 | Codex | `feat/w1-radio-mode` |
| W2 | SoftAP + httpd + WS 骨架，嵌入页面，1 Hz 状态推送 | W1 | Codex | `feat/w2-web-server` |
| W3 | `web_proto` 纯组件：JSON 分词/输出、键表、`set`→`fw::Command`，host 测试 | 只依赖 §2.5 定稿 | OMP（与 W1 并行） | `feat/w3-web-proto` |
| W4 | 网页前端（旧样式重写，≤ 24 KB gz）+ `tools/web_mock` | 只依赖 §2.5 定稿 | AGY（与 W1 并行） | `feat/w4-web-ui` |
| W5 | 集成：W2+W3+W4 合并，全部配置项接通，P2 台架验收 | W2、W3、W4 | Codex | `feat/w5-web-integrate` |
| W6 | 手机协议“进入网页配置”指令（含冻结头改动） | W1 | OMP | `feat/w6-radio-entry` |

每张卡由另一个代理交叉评审，Architect 复跑门禁并做 P2 台架验收后合入 main。

## 7 用户决定（2026-11-19）

- **Q1 切换方式**：互斥，按启动选择，切换即重启（§2.1）。“共存”只在 W0 作对照测量。
- **Q2 入口**：手机 App 指令（W6）；P2 的 OLED + 按键菜单以后再做。
- **Q3 热点安全**：所有设备同一个 WPA2 口令（`RTK_WIFI_AP_PASSWORD`）。
- **Q4 OTA**：不进第一版。

### 补充（2026-11-19）

- 量产热点口令 `12345678`（`RTK_WIFI_AP_PASSWORD` 默认值）。
- 手机 App 由用户团队修改；W6 只做设备端指令、协议文档与测试桩。
