# W2：Wi-Fi 配置模式网页服务（合并 ADR-0017 W2 + W5 + S2）

- 依据：ADR-0017 §2.2–§2.6、`docs/web/ws-protocol.md` v1；用户 2026-11-19 要求先让网页上的 GGA 门控（Q1）在设备上可用。
- 基线：main `c404e96`（W1 射频模式、W3 `web_proto`、W4 页面、Q1 门控均已合入）。
- 分支 `feat/w2-web-server`，工作树 `RTK_Main-w2`，构建前缀 `builds/w2-*`。
- 执行者不提交、不烧录、不连串口；HIL 由 Architect 做。

## 1 范围

1. **SoftAP**（仅 `RadioMode::kWifiConfig`）：SSID = `systemStatus.deviceName`（`P2-<SN>`），WPA2-PSK，口令 Kconfig
   `RTK_WIFI_AP_PASSWORD`（默认 `12345678`，编译期 `static_assert` 8–63 字符），信道 `RTK_WIFI_AP_CHANNEL`（默认 1，1–11），
   最多 2 个终端，AP IP `192.168.10.12/24`（网关同址）+ DHCP（用户 2026-11-19 指定，不用 IDF 默认 192.168.4.1，需 `esp_netif_dhcps_stop`→`esp_netif_set_ip_info`→`esp_netif_dhcps_start`）。新 Kconfig 放 `main/Kconfig.projbuild` 的 `RTK_WIFI_CONFIG_ENABLE` 下。
2. **空闲回落**：`RTK_WIFI_IDLE_EXIT_S`（默认 300，60–3600）：无终端连上满该时长、或最后一个终端断开满该时长 →
   推 `bye{reason:"idle"}` 后复用 W1 的请求接口切回 `kBluetooth` 并重启。
3. **HTTP/WS**：`esp_http_server`，1 个监听任务固定 core 0，最多 3 个套接字；`GET /` 返回构建期 gzip 的 `main/web/ui/` 单页
   （CMake 调 Python 标准库 gzip，`EMBED_FILES`，gzip 后 ≤ 24 KB，超限构建失败）；`GET /ws` 升级；其余 404。不做 DNS 劫持。
4. **新模块 `web`**：`module_list.inc` 登记 `kWifiOnly`、执行器 `kLink`。httpd 回调线程**不得**直接读写模块状态：
   收到的帧拷进静态 SPSC/邮箱（≤ 1 KB/帧，满则回 `busy`），由 `web` 模块 `poll()` 在 kLink 解析处理；发送用
   `httpd_ws_send_frame_async`，帧缓冲静态。本工程代码不分配堆（IDF/lwIP 内部分配记入 G7b 白名单，同 Bluedroid）。
5. **协议**：严格按 `ws-protocol.md` v1：`hello`、`config`（连接后与每次 `set` 成功后）、`status` 每 1 s 只发变化组、
   `ack`、`bye`；一次只处理一条请求。解析/键表/输出全部复用 `components/web_proto`（不复制）。
6. **set → 命令总线**：每个键转成 `fw::Command` 发给拥有者模块，与控制台同一校验与保存路径，网页不直接写 NVS：
   - `gnss.rate` → `GnssVerb::kSetRate`；`gnss.out.*` → `GnssSubVerb::kSetOut`；
   - **`gnss.gga_gate.enabled` / `gnss.gga_gate.min_sats` → `GnssSubVerb::kSetGgaGate`（Q1，必须可用）**；`config.gga_gate` 取自
     `DataCenter::ggaGateStatus`；
   - `gnss.const.*`：若 W0 已确认 CM680 命令并有现成动词则接通，否则回 `unsupported`（报告写明）；
   - `factory.sn`（需解锁）、`cmd save|exit|freset`（`freset` 需解锁）、`unlock`（口令同控制台 `factory unlock`，断开即失效）。
7. **S2（Wi-Fi 模式下的蓝牙设置）**：`bt_link` 在 Wi-Fi 模式不运行，所以：
   - `bt.wl.add` / `bt.wl.del`：新增由 `storage` 模块执行的白名单写动词（写同一 NVS 记录，格式与 `bt_link` 读出的一致），
     `status.bt.whitelist` 从同一记录读；
   - `bt.pair_next`：新 NVS 键（`storage` 拥有），下次 BT 启动时 `bt_link` 读到即开配对窗口并清除该键。
   - 不改 `components/*/include/**`；确需改冻结头时先停下在报告中说明。
8. **页面**：W4 页面已就绪，只修集成发现的问题；`tools/web_mock/web_mock.py --selftest` 保持 PASS。

## 2 测试（host，含 ASan）

- 帧邮箱：满/超长/并发投递；`set` 每个键 → 正确的 `fw::Command`（含 `gga_gate` 两键、越界 `bad_value`）；
- `config`/`status` 输出与 `ws-protocol.md` 示例一致（含 `gga_gate` 嵌套对象）；status 只发变化组；
- 空闲回落计时（无终端、最后终端断开、期间重连取消）；
- S2：Wi-Fi 模式写白名单后 BT 启动读到同一内容；`pair_next` 只生效一次；
- 门禁全 PASS，含 `G3-idf-wifi-off`（关 `RTK_WIFI_CONFIG_ENABLE` 不链入 Wi-Fi）。

## 3 交付

- 报告 `docs/reports/W2-web-server.md`：改动表、heap/IRAM/镜像大小、gzip 页面大小、偏离与 §文档待改。
- Architect HIL（P2）：`radio wifi` → 手机/电脑连 `P2-<SN>` → 打开 `http://192.168.10.12/` → 状态 1 Hz、空闲堆 ≥ 40 KB；
  页面打开 GGA 门控并改 `min_sats` → 控制台 `gnss status` 显示一致 → “退出”回 BT 后设置仍在；白名单增删 BT 模式下生效；
  无终端 300 s 自动回 BT。
