# W7：Wi-Fi 配置模式强制门户（Captive Portal）实施报告

- **任务卡**：`docs/tasks/W7-captive-portal.md`（含两轮交叉审查修复 `F1–F11`）
- **基线**：`main` `b15075b`，分支 `feat/w7-captive-portal`，工作树 `RTK_Main-w7`
- **构建前缀**：`builds/w7fix-*`（初轮 `builds/w7-*`）
- **工具链**：ESP-IDF v6.1（`/home/gtc/esp/esp-idf`）、Xtensa GCC 15.2、Python 3.14
- **冻结文件**：未修改任何冻结文件（`G1-frozen` PASS）。

---

## 1. 改动文件

### 新增文件

| 文件 | 用途 |
| --- | --- |
| `main/modules/web/WebDns.hpp` / `main/modules/web/WebDns.cpp` | 纯 C++20 强制门户 DNS 报文编解码器（RFC 1035 + RFC 6891 EDNS0 OPT，无堆、逐字节大端读写）：校验长度（17–512 B）、标准查询标志（QR=0/OPCODE=0/TC=0/QDCOUNT=1/ANCOUNT=0/NSCOUNT=0）、QNAME 标签边界（255 B 上限）与拒绝压缩指针（`0xC0`/`0x40`/`0x80`）；严格校验附加段（`ARCOUNT=0` 无尾字节，或 `ARCOUNT=1` 完整根名 OPT 且 `RDLENGTH` 精确匹配剩余字节）；A 查询（IN/ANY）回 `192.168.10.12`（TTL 60 s，NOERROR），AAAA 及其他类型回 NOERROR 空应答（ANCOUNT=0），合法 OPT 请求回带最小 OPT（根名、type 41、UDP 512、flags 0、RDLEN 0、`ARCOUNT=1`） |
| `tools/main_host_check/web_shim/lwip/sockets.h` | `web_module_check` 专用 lwIP socket 替身头（含 `lwip_recvmsg`，隔离宿主 UDP 53 权限与 libc `close`） |

### 修改文件

| 文件 | 改动摘要 |
| --- | --- |
| `main/Kconfig.projbuild` | 在 `RTK_WIFI_CONFIG_ENABLE` 下新增 `RTK_WIFI_CAPTIVE_PORTAL`（`bool`，默认 `y`，依赖 `RTK_WIFI_CONFIG_ENABLE`） |
| `main/CMakeLists.txt` | 在 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL=n` 时过滤 `modules/web/WebDns.cpp`，避免门户关闭时编译或残留符号 |
| `main/def/SystemDef.hpp` | `WebTrace` 新增 `kDnsErr = 10`（实时路径 DNS 套接字错误追踪事件） |
| `main/modules/debug/TraceText.cpp` | 登记 `WebTrace::kDnsErr` 文本渲染条目，并以 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL` 守卫（关闭时零格式串残留） |
| `main/modules/web/WebModule.hpp` | 在 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL` 开启时声明 `startDns()`、`closeDnsSocket()`、`pollDns(nowMs)`、`handleCaptiveRedirect()`、`handleNotFound()` 及全零初值（`.bss`）DNS 套接字、重试计时与 512 B 静态收发缓冲；拆分 `resetRuntimeState()` 与 `startNetworkStack()` |
| `main/modules/web/WebServer.cpp` | 调整启动顺序为 `initSoftApNetif` → `startHttpd`（含全部 URI/404 注册）→ `startDns`（失败仅降级 DNS 不拆 Wi-Fi）→ `startSoftApWifi`；SoftAP 启动前设静态存储的 DHCP option 114（`http://192.168.10.12/`）；注册 7 个系统探测路径与 `HTTPD_404_NOT_FOUND` 处理器（GET 回 302 空体 `Cache-Control: no-store`，非 GET 发 404 并返回 `ESP_FAIL` 保持关连接语义）；`GET /` 接受 `192.168.10.12` 与 `192.168.10.12:80`，其余或缺失 302；`pollDns` 用 `lwip_recvmsg` + `MSG_TRUNC` 丢弃超长 UDP 报文（计入 4 包额度），非 `EAGAIN`/`EWOULDBLOCK` 错误关套接字并按 ≥1 s 间隔重试；`teardownServer` 无论 `httpd_stop` 结果均关闭 DNS 套接字并校验 `lwip_close` 返回值 |
| `main/modules/web/WebModule.cpp` | `WebModule::poll(nowMs)` 在 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL` 开启时调用 `pollDns(nowMs)`，并将 `dnsMore` 纳入返回值 |
| `tools/web_mock/web_mock.py` | 增加 7 个系统探测路径与未知 GET 路径的 302 重定向、`/favicon.ico` 204、`GET /` 的 `Host` 校验；新增 `--canonical-host` 与 `resolve_canonical_host()`（按绑定后实际端口解析、`0.0.0.0` 回退 `127.0.0.1`），`--selftest` 共用同一逻辑 |
| `tools/main_host_check/web_shim/esp_netif.h` / `esp_http_server.h` / `sdkconfig.h` | 补齐 DHCP option 114、404 处理器、Host 读取与 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL` 替身定义 |
| `tools/main_host_check/WebServerCheck.cpp` | 扩充 `testDnsCodec()`：覆盖 A/AAAA/HTTPS/TXT、合法 OPT 应答（含带 RDATA）、255/256 B 域名边界、尾部垃圾、ARCOUNT 缺记录/超限、OPT 截断、非根名/非 OPT、RDLENGTH 越界、压缩指针、超长报文与缓冲不足 |
| `tools/main_host_check/WebModuleCheck.cpp` | 扩充 `testCaptivePortal()` 与中文 Doxygen 桩：覆盖 `dhcps_stop < option114 < dhcps_start < httpd_start < URI/404 注册 < dns_bind < wifi_start` 有序断言、静态 URI 读回、7 探测路径 302、404 GET/POST、Host 含 `:80`、512 B 合法 / 513 B `MSG_TRUNC`、DNS 启动失败降级（socket/fcntl/bind 注入）、运行期接收错误关闭与 ≥1 s 重试、`httpd_stop` 失败仍关 DNS、`lwip_close` 失败保句柄、关停后重新 `init`，以及 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL=0` 全路径 |
| `tools/main_host_check/CMakeLists.txt` | `web_module_portal_off_check` 不再编译 `WebDns.cpp`，验证门户关闭零引用 |

说明：`main/web/ui/app.js:667` 已使用 `location.host` 拼接 `/ws` 且无外部资源，按任务卡 §2.1 确认无须修改。

---

## 2. 执行方式选择与理由

选择任务卡 §1.2 方案 **(a)：非阻塞 UDP 53 套接字由 `WebModule::poll()` 在 `kLink` 轮询处理，每轮最多 4 包**。

理由：
1. **零额外任务栈**：无需新建 FreeRTOS 静态任务，省去约 3–4 KB 任务栈，也不需要修改 `ADR-0011` BootArena 栈台账表。
2. **`kLink` 在 Wi-Fi 模式下负载很轻**：`kWifiConfig` 模式下 `bt_link` 不启用，`kLink` 仅执行 `WebModule::poll()`，无蓝牙竞争。
3. **运行期开销与阻塞边界**：
   - 自有代码零堆分配（512 B 收发缓冲静态驻留 `.bss`），非阻塞 `lwip_recvmsg` 无包时立即 `EAGAIN` 返回；
   - **lwIP SDK 内部开销**：每次 `lwip_sendto()` 会在 lwIP 内部从堆/内存池分配瞬时 `pbuf`（约 64–128 B，发完即释放）并通过 `tcpip_api_call` 同步等待 `tcpip_thread` 完成 UDP 投递（典型单包数十至数百微秒）。每轮上限 4 包将单轮最坏耗时约束在毫秒级以内，不触发 `kLink` 执行器超时。
4. **启动顺序核查（IDF 源码）**：`esp_http_server/src/httpd_main.c:413` 绑定 `INADDR_ANY:80`、`startDns` 绑定 `INADDR_ANY:53`，两者均仅依赖 `esp_netif_init()` 启动的 lwIP 协议栈，无需等待 SoftAP 发射即可完成监听与全部 URI/404 注册；因此将 `startSoftApWifi()` 置后可彻底消除 `httpd_uri.c:150-158` 注册表写入与客户端请求的竞态。

---

## 3. 资源与镜像大小差值（二轮修复后 `builds/w7fix-*`）

### 3.1 固件构建大小（`idf.py size` 与 `stat -c %s`）

| 构建目录 | IRAM (B) | IRAM 余量 (B) | 静态 DRAM (B) | DRAM `.data` / `.bss` (B) | DRAM 余量 (B) | Flash Code (B) | Flash Data (B) | 链接镜像 (B) | `rtk_main.bin` (B) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `builds/w7fix-idf`（DEV，门户开） | 114,715 | 16,357 | 96,931 | 52,379 / 44,552 | 27,649 | 1,044,136 | 246,092 | 1,457,362 | 1,457,472 |
| `builds/w7fix-portal-off`（DEV，门户关） | 114,715 | 16,357 | 95,867 | 51,323 / 44,544 | 28,713 | 1,040,976 | 245,604 | 1,452,658 | 1,452,768 |
| **W7 差值（门户开 − 门户关）** | **0** | **0** | **+1,064** | **+1,056 / +8** | **-1,064** | **+3,160** | **+488** | **+4,704** | **+4,704** |
| `builds/w7fix-wifi-off`（`RTK_WIFI_CONFIG_ENABLE=n`） | 86,287 | 44,785 | 75,487 | 45,119 / 30,368 | 49,093 | 632,080 | 164,632 | 928,158 | 928,192 |

### 3.2 堆与裁剪核查

- **IRAM**：`+0 B`，余量 `16,357 B`（门槛 ≥ 8 KB，PASS）。
- **静态 DRAM / 运行期堆**：静态增量 `+1,064 B`（主要为 `WebModule` 内置的两个 512 B 静态 DNS 收发缓冲 `dnsRxBuf_`/`dnsTxBuf_`、重试状态与静态 URI 串）。自有代码零堆分配（`G7b-runtime` PASS：131 个自有目标文件，0 个未白名单堆符号，0 个 `__cxa_guard`，6/6 白名单异常/RTTI 符号）；运行期 lwIP 侧常驻 1 个 UDP PCB/套接字（约数百字节），每次 `lwip_sendto()` 瞬时申请并释放一个 pbuf（每轮 ≤ 4 包，同步交由 `tcpip_thread` 发出）。Wi-Fi 模式实际空闲堆待 Architect P2 HIL 实测确认 ≥ 40 KB。
- **F8 门户关闭裁剪验证（`builds/w7fix-portal-off/rtk_main.elf`）**：
  - `xtensa-esp32-elf-nm` 检出 `buildDnsReply`、`handleCaptiveRedirect`、`handleNotFound`、`startDns`、`pollDns`、`closeDnsSocket`：**`NONE_FOUND`**（PASS）。
  - `strings` 检出 `dns socket error`、`captive DNS start failed`、`generate_204`、`hotspot-detect`：**`NONE_FOUND`**（PASS）。

---

## 4. 二轮修复（F1–F11）对照

| 编号 | 审查项 | 修复内容 | 验证 |
| --- | --- | --- | --- |
| F1 | 启动顺序竞态 | `WebModule::init()` 拆为 `resetRuntimeState()` + `startNetworkStack()`，按 `initSoftApNetif` → `startHttpd`（含全部 URI/404 注册）→ `startDns` → `startSoftApWifi` 顺序执行 | `web_module_check` 有序调用序列断言 PASS |
| F2 | DNS 启动失败不拆 Wi-Fi | `startDns()` 失败仅 `traceWeb` + 一次 `FW_LOGW` + 置重试标志，不拆掉 httpd/SoftAP/302/option 114，`init()` 返回 `kOk` | `web_module_check` 注入 socket/fcntl/bind 三类失败 PASS |
| F3 | 超长 UDP 报文 | `pollDns()` 改用 `lwip_recvmsg` 检查 `MSG_TRUNC`，>512 B 静默丢弃并仍计入每轮 4 包额度 | `web_module_check` 512 B 合法 + 513 B `MSG_TRUNC` 接收层用例 PASS |
| F4 | 附加段 / EDNS0 | `WebDns.cpp` 新增 `validateAdditional()`：严格匹配 `ARCOUNT=0` 无尾字节或 `ARCOUNT=1` 完整根名 OPT（type 41、RDLEN 不越界无尾垃圾）；合法 OPT 应答附带最小 OPT（UDP 512、RDLEN 0、`ARCOUNT=1`）并计入 512 B 输出空间 | `web_server_check` 覆盖 OPT 应答、截断、RDLEN 越界、ARCOUNT 缺记录、尾部垃圾、255/256 B 域名边界 PASS |
| F5 | DNS socket 错误与关闭 | 接收非 `EAGAIN`/`EWOULDBLOCK` 错误关套接字、trace 一次、按 ≥1 s 间隔在 `pollDns` 重试；`teardownServer` 无论 `httpd_stop` 结果先关 DNS；`closeDnsSocket` 查 `lwip_close` 返回并维护 `dnsOpen_` | `web_module_check` 接收错误重连、`httpd_stop` 失败关 DNS、`lwip_close` 失败保状态、关停后重 `init` PASS |
| F6 | Host 校验 | `isCanonicalApHost()` 接受 `192.168.10.12` 与 `192.168.10.12:80`，避免带默认端口自环 302 | `web_module_check` PASS |
| F7 | 未知非 GET | `handleNotFound()` 对非 GET 调用 `httpd_resp_send_err` 后返回 `ESP_FAIL`（或发送错误），保持 IDF 原生关连接语义 | `web_module_check` PASS |
| F8 | Kconfig 关闭残留 | `TraceText.cpp` 用 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL` 守卫 `kDnsErr` 条目与格式串；`main/CMakeLists.txt` 与 `tools/main_host_check/CMakeLists.txt` 在门户关闭时过滤 `WebDns.cpp` | `builds/w7fix-portal-off` `nm` 与 `strings` 均 `NONE_FOUND` |
| F9 | `web_mock.py` | 新增 `resolve_canonical_host()` 与 `--canonical-host`，使用绑定后实际端口，`0.0.0.0` 回退 `127.0.0.1`，`--selftest` 共用同一函数，新增注释改为中文 | `web_mock.py --selftest` PASS |
| F10 | 测试桩与注释 | DHCP/httpd/DNS/Wi-Fi 桩记录有序序列、断言 option 114 在 STOP/START 之间且 Wi-Fi start 最后、读回静态 URI 指针；全部新增 C/C++ 桩补中文 Doxygen | `web_module_check` + `G6-doc_check`（23/23）PASS |
| F11 | 报告修正 | 补充本节、修正 §2/§3.2 lwIP 发送堆/同步说明、更新 §5 偏离与冻结注释待改项 | 本报告 |

---

## 5. 实际执行的命令与结果

| 项 | 命令 | 结果 | 备注 |
| --- | --- | --- | --- |
| 全量 CI 门禁 | `unset IDF_PATH; . $HOME/esp/esp-idf/export.sh; bash tools/ci/run_gates.sh --prefix w7fix` | **PASS（40/40）** | 基线 `b15075b`，0 失败 |
| G1 冻结校验 | `G1-frozen`（`sha256sum -c --quiet docs/frozen.sha256`） | **PASS** | 全部冻结文件无改动 |
| G2 / G2b Host + ASan/UBSan | 13 组 × 普通 + ASan/UBSan（26 项，含 `main_host_check` 82/82：`web_server_check`、`web_module_check`、`web_module_portal_off_check`） | **PASS（26/26）** | 0 失败、0 ASan/UBSan 报告 |
| G5 代码格式 | `G5-format`（`clang-format --style=file --dry-run --Werror`） | **PASS** | 全仓格式合规 |
| G6 Doxygen 文档 | `G6-doc_check`（`doc_check.py --base b15075b`） | **PASS** | 改动函数 23 个，不合格 0 个 |
| G3 / G3b ESP-IDF 构建 | `w7fix-idf`、`w7fix-trace-off`、`w7fix-wifi-off`、`w7fix-production`、`w7fix-console-none`、`w7fix-none-wifi`、`w7fix-rtk_pure-esp32`、`w7fix-rtk_pure-esp32c3` | **PASS（8/8）** | 全部 0 warning |
| G5b 静态分析 | `G5b-tidy`（`clang-tidy` 142 个文件） | **PASS** | 142 文件 0 发现 |
| G7b / G7c | `G7b-runtime`、`G7c-no-cli-deps` | **PASS（2/2）** | 0 堆违规、0 CLI 违规依赖 |
| 门户关闭独立构建与裁剪 | `idf.py -B builds/w7fix-portal-off ... build` + `xtensa-esp32-elf-nm` + `strings` | **PASS** | `NONE_FOUND`（符号与格式串均零残留） |
| 前端模拟自检 | `python3 tools/web_mock/web_mock.py --selftest` | **PASS** | 覆盖 7 探测路径 + 未知路径 302、`/favicon.ico` 204、错误 Host 302、`resolve_canonical_host` 与 WS 全量自检 |
| P2 台架 HIL | 烧录 `builds/w7fix-idf` + 串口 + 实机门户验证 | **PASS** | 2026-11-21 Architect 执行，见 §7 |

---

## 6. 偏离与 §文档待改

### 6.1 偏离与待 HIL 确认事项

1. **DHCP 选项 114 与 RFC 8908 CAPPORT API 差异**：按任务卡 §1.1 保留 `ESP_NETIF_CAPTIVEPORTAL_URI` 指向 HTTP 配置页 `http://192.168.10.12/` 作为兼容提示；该值不符合 RFC 8908 定义的 CAPPORT API（要求 HTTPS JSON 端点），各终端操作系统（Android/iOS/Windows）是否忽略或采纳该选项待 Architect HIL 实测确认（DNS 劫持 + 探测 URL 302 作为主触发路径独立生效）。
2. **`sdkconfig.defaults:48` 套接字余量注释已过时**：当前 `CONFIG_LWIP_MAX_SOCKETS=8`，原注释写“httpd 要求 >= max_open_sockets(3)+3，取 8 留 2 个余量”。启用门户 UDP 53 套接字后实际占用 7/8（仅余 1 个套接字）；因 `sdkconfig.defaults` 为冻结文件未修改，待 Architect 合入时评估更新注释或容量并重算 `docs/frozen.sha256`。

### 6.2 §文档待改（供 Architect 合入时更新）

1. **`docs/adr/ADR-0017-wifi-web-config.md` §2.3 / §4**：
   - 补记强制门户方案：DHCP option 114（`http://192.168.10.12/`）、UDP 53 非阻塞 DNS 劫持（`kLink` 每轮最多 4 包，`lwip_recvmsg` + `MSG_TRUNC` 丢弃超长包，A 回 `192.168.10.12` TTL 60 s，AAAA/其他回 NOERROR 空应答，严格 EDNS0 OPT 校验与最小 OPT 应答）、系统探测路径与 404 GET 302 重定向、`GET /` Host 规范化校验（含 `:80`）；
   - 在 §4 Kconfig 清单补充 `RTK_WIFI_CAPTIVE_PORTAL`（默认 `y`，依赖 `RTK_WIFI_CONFIG_ENABLE`）。
2. **`docs/web/ws-protocol.md`**：
   - 在 HTTP 路由概述中补充门户探测路径与 404 GET 的 302 行为及 `GET /` 非规范 Host 的 302 规范化说明（`/ws` 与 `/` 既有协议不变）。
3. **`docs/tasks/W2-web-server.md` §1.3**：
   - 标注原“不做 DNS 劫持”约定已由 `docs/tasks/W7-captive-portal.md` 取代。
4. **`sdkconfig.defaults:48`**（冻结，由 Architect 处理）：
   - 同步更新 `CONFIG_LWIP_MAX_SOCKETS=8` 旁关于套接字余量的注释（计入 DNS UDP 53 后为 7/8）。

---

## 7. 台架 HIL（2026-11-21，Architect，P2 v1.41，SN 20261121，`builds/w7fix-idf`）

| 项 | 结果 | 记录 |
| --- | --- | --- |
| 笔记本 DNS/HTTP | **PASS** | DHCP 网关/DNS 192.168.10.12；A→192.168.10.12、AAAA 空；EDNS OPT udp 512，无 EDNS 无 OPT；畸形/600 B 包丢弃；突发 30 包回 6（`CONFIG_LWIP_UDP_RECVMBOX_SIZE=6`）；7 探测路径 + 随机路径 302，`captive.apple.com` Host 302，`:80` 200，favicon 204，未知 POST 404；`web_check.py` 12/12；Wi-Fi 模式空闲堆 85 KB / 最低 71 KB |
| Windows 门户弹窗 | **PASS** | 自动弹出并显示配置页 |
| Android 门户弹窗 | **PASS** | 自动弹出并显示配置页 |
| iPhone 门户弹窗 | **PASS** | 自动弹出并显示配置页 |
| 空闲 300 s 回蓝牙 | **PASS** | 最后终端 18:13:57 断开 → 18:18:57 软复位，`radio=bt reason=idle` |
| 蓝牙 RTCM 10 min | **PASS** | `spp_rtcm_mix.py --epoch-hz 1 --burst --product cycle --duration 600`：板端 frames 3600 / bytes 637178 / crc32 0xA23BF31E 与发送端一致，rx_dropped 0；产品 12000/12000，RTT p50 10.5 ms / p99 56.7 ms / max 179 ms；30 s 采样空闲堆 56.8–60.5 KB，heap_min 54.7 KB（门槛 ≥ 40 KB） |

- 已知日志：客户端关闭门户窗口时 IDF 打印 `httpd_sock_err: error in send : 104` / `uri handler execution failed`（对端 RST），无功能影响。
- 连上后各系统仍显示“需要登录/验证”：探测 URL 恒 302，与 SparkFun 相同；“确认后放行”（W7b）暂不做，理由见 ADR-0017 §2.3。
- §6.1 第 1 项：三系统均由 DNS 劫持 + 探测 302 触发弹窗；option 114 是否被采纳未单独区分，保留。§6.1 第 2 项与 §6.2 已在合入时处理（`sdkconfig.defaults` 注释更新并重算 `docs/frozen.sha256`）。
