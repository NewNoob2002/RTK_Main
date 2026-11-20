# W7：Wi-Fi 配置模式强制门户（Captive Portal）

- 依据：ADR-0017 §2.2–§2.6；用户 2026-11-20 要求：手机/电脑连上 `P2-<SN>` 热点后自动弹出配置页。
- 前置：W2 Revision A 合入 main 并通过 HIL（本卡基线 = 届时 main，Architect 派发时填写 commit）。
- 分支 `feat/w7-captive-portal`，工作树 `RTK_Main-w7`，构建前缀 `builds/w7-*`。
- 执行者不提交、不烧录、不连串口；HIL 由 Architect 做。
- 本卡取代 W2 卡 §1.3 “不做 DNS 劫持”的约定；ADR-0017 由 Architect 在合入时补记。

## 1 范围

1. **DHCP 选项 114（RFC 8910）**：SoftAP 启动前用 `esp_netif_dhcps_option(..., ESP_NETIF_CAPTIVEPORTAL_URI, ...)`
   通告 `http://192.168.10.12/`。IDF 只保存指针（`esp_netif.h:580`），URI 字符串必须是静态存储。
2. **DNS 劫持服务**：参考 `$IDF_PATH/examples/protocols/http_server/captive_portal/components/dns_server/dns_server.c`
   的报文处理，但按本工程规则重写（不得原样复制）：
   - UDP 53，所有 A 查询应答 `192.168.10.12`，TTL 60 s；AAAA/其他类型回 NOERROR 空应答（不回 NXDOMAIN，避免部分系统认定“无网”后断开）；
   - 不分配堆：静态收发缓冲（≤ 512 B，超长报文丢弃），报文解析做完整边界检查（QDCOUNT=1、标签长度、压缩指针不跟随、截断报文丢弃）；
   - 执行方式二选一并在报告说明理由：(a) 非阻塞 socket 由 `web` 模块 `poll()` 在 kLink 处理，每轮最多 4 包；(b) 静态任务固定 core 0、
     低优先级，栈大小写入 ADR-0011 预算表待改。优先 (a)。
   - 只在 `RadioMode::kWifiConfig` 且 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL=y`（新 Kconfig，默认 y，依赖 `RTK_WIFI_CONFIG_ENABLE`）时启用。
3. **系统探测 URL 重定向**（`esp_http_server`）：以下路径回 `302 Found`，`Location: http://192.168.10.12/`，空体，`Cache-Control: no-store`：
   - Android：`/generate_204`、`/gen_204`；
   - Apple：`/hotspot-detect.html`、`/library/test/success.html`；
   - Windows：`/connecttest.txt`、`/ncsi.txt`、`/redirect`；
   - 其余未匹配 GET（注册 404 错误处理器）同样 302 到根页，`/ws` 与 `/` 不受影响。`/favicon.ico` 仍 204。
   - 注意 httpd `max_uri_handlers`，按实际注册数调整并留 2 个余量。
4. **Host 头校验**：`Host` 不是 `192.168.10.12`（或缺失）的 `GET /` 也 302 到 `http://192.168.10.12/`，保证门户窗口地址统一。
5. **页面适配**：`main/web/ui/app.js` 的 WebSocket 地址一律用 `location.host`，在 iOS 门户小窗与普通浏览器都能连；
   不新增外部资源；`tools/web_mock/web_mock.py` 增加探测路径与 302 行为，`--selftest` 覆盖。
6. **不做**：mDNS、HTTPS、互联网转发。

## 2 测试（host，含 ASan/UBSan）

- DNS：正常 A 查询应答；AAAA 空应答；畸形报文（截断、标签越界、QDCOUNT≠1、压缩指针、超长）全部丢弃且无越界；
- HTTP：每个探测路径 302 + Location；未知路径 302；`/`、`/ws`、`/favicon.ico` 行为不变；错误 Host 302；
- Kconfig 关闭门户时不注册 DNS、不设 option 114、未知路径回 404；`G3-idf-wifi-off` 仍不链入 Wi-Fi；
- 门禁全 PASS（G1 不涉及冻结文件；若必须改冻结文件，先停下在报告说明）。

## 3 交付

- 报告 `docs/reports/W7-captive-portal.md`：改动表、heap/IRAM/镜像大小差值、所选执行方式与理由、偏离与 §文档待改（ADR-0017、ws-protocol.md、W2 卡）。
- Architect HIL（P2，`radio wifi` 后）：
  - Android：连 `P2-<SN>` 后出现“登录网络”通知，点开即配置页，状态 1 Hz 更新；
  - iPhone：自动弹出门户窗口，页面可改 GGA 门控并 ack；
  - Windows/Linux 笔记本：系统弹出浏览器或访问任意 http 网址被引到配置页；
  - `sys mem` Wi-Fi 模式空闲堆 ≥ 40 KB；300 s 无终端仍自动回 BT。
