# HB（H1-heap-budget）交叉评审（OMP，只读）

基线 `8c28078`，任务卡提交 `e2e4db1`，评审对象为 worktree 内未提交改动（`git diff HEAD` + 未跟踪 `tools/hil/web_load.py`）与实现方报告 `docs/reports/HB.md`。本评审不改源码、不提交、不烧录；构建前缀 `builds/hbr-*` 未使用（见 §1 说明）。

## 结论摘要

- **H1a 代码正确**：门槛选择逻辑、两种配置的边界测试、输出格式均符合卡片；并用两个 IDF 镜像的反汇编独立确认了编译期选择（Wi-Fi 镜像 40,960，Wi-Fi 关闭镜像 81,920）。
- **候选项安全**：8 项逐条核对 SDK Kconfig 与 IDF 源码，未发现会破坏 WPA2-PSK SoftAP 或 httpd 启动的改动；`LWIP_MAX_SOCKETS=6` 恰好等于 IDF 的硬性下限，没有余量，且与卡片的"双客户端"验收存在耦合风险。
- **报告有 2 个 P1**：把"静态 DRAM 减少"当成违规排除（Architect 已指出的取舍错误）；Wi-Fi 关闭构建的 81,920 B 门槛按现有证据很可能仍然 FAIL，而报告没有按卡片要求报告这处矛盾。
- **脚本基本合格**：`--selftest` PASS，协议字段与设备端逐一核对一致；`missing` 字段是常量而非测量值，不能单独支撑"0 缺帧"。

## 1. 已独立核实（通过）

| 检查 | 命令/证据 | 结果 |
| --- | --- | --- |
| 编译期门槛选择（Wi-Fi 镜像） | `xtensa-esp-elf-objdump -d builds/h1-idf/rtk_main.elf`，`cmdSysMem` 内 `l32r a13, ... (a000)` = 40,960 | PASS |
| 编译期门槛选择（`G3-idf-wifi-off`） | 同上，`builds/h1-wifi-off/rtk_main.elf` 内 `l32r a13, ... (14000)` = 81,920 | PASS |
| `#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)` 语义 | `builds/h1-wifi-off/config/sdkconfig.h` 无该宏；`builds/h1-idf/config/sdkconfig.h:291` 为 `#define ... 1` | PASS |
| host 边界测试 | `cmake --build builds/h1-main_host_check && ctest` | 77/77 PASS，boot 用例 5/5 |
| 两种配置各覆盖 =门槛 PASS / min−1 FAIL / largest 32767 FAIL | `tools/main_host_check/BootAllocCheck.cpp:480-501`，6 组 `BudgetCase` 并检查来源串 | PASS |
| 格式门禁 | `clang-format --style=file --dry-run --Werror` 三个改动文件 | PASS（exit 0） |
| 函数注释门禁 | `python3 -I tools/doc_check/doc_check.py --base 8c28078` | 改动函数 5 个，不合格 0 |
| 冻结校验 | `sha256sum -c docs/frozen.sha256` | 仅 `sdkconfig.defaults` 偏离（卡片例外） |
| 候选项 A/B 可控性 | `diff <(grep -v '^#' sdkconfig) <(grep -v '^#' builds/h1-static-rx/sdkconfig)` | 仅 6 行差异（`STATIC_RX_BUFFER_NUM` 及其两张旧别名），其余候选同理 |
| 静态 DRAM | `builds/h1-logs/*.size.log` | 默认镜像 0 B（flash +40 B）；wpa −288；sockets −240；其余 0 |
| 脚本自检 | `python3 -I tools/hil/web_load.py --selftest` | `{"selftest": "PASS"}` |
| 依赖可用性 | IDF 环境 Python `websockets 17.3`，`connect(...)` 支持 `proxy=None`、`compression=None` | PASS |
| 1 Hz 状态帧必带 `dev` | `WebModule.cpp:786` 每秒采样；`WebLogic.cpp:451-452` 按组变化发送，`dev.uptimeS` 每秒递增（`WebModule.cpp:206`）→ `dev` 每秒都在 | PASS |
| WS ping 不会被设备误判关连接 | `handle_ws_control_frames=false`；`httpd_ws.c:518-539` 由 httpd 自动回 PONG | PASS（600 s 会话不会被客户端 20 s ping 打断） |

说明：报告中的 35 PASS/1 FAIL 我未重跑整套 `run_gates.sh`（IDF 目标构建耗时），改为逐项复核日志 + 对关键项独立复跑（host、format、doc_check、frozen、size、反汇编）。报告数值与日志一致。

## 2. 发现（按严重度）

### P1-1 报告把"静态 DRAM 减少"当成违规排除（`docs/reports/HB.md:72-73`）

问题：`wpa`（−288 B）与 `sockets`（−240 B）的取舍写作"排除，绝对变化量超 100 B"。卡片的"BT 模式静态 DRAM 变化 ≤ 100 B"是**防增长**的单边约束，减少不构成违反（Architect 2026-11-20 已明确）。同一报告的 `HB.md:77` 又写的是正确的运行时规则"每项达到至少 1,024 B 动态堆收益"，两处自相矛盾；`HB.md:59` 的"整图校验绝对变化量 0 B ≤ 100 B"也沿用了"绝对"措辞。

影响：若 Architect 按报告执行，会以一条不存在的约束否掉净收益；`wpa` 除 +288 B 堆总量外还省 **61,492 B flash code**（`1051588` vs `1133080`），报告未把这一点放进决策表。

建议：
- 把两行的理由改成"静态 DRAM 为减少，满足防增长约束；但单项 < 1,024 B，按卡片规则暂不单独启用，留待 P2 组合评估"。
- `wpa` 行补 flash 收益（`builds/h1-logs/wpa.size.log`：Flash Code −61,452 B，`libwpa_supplicant.a` −40,659 B）。
- 全文把"绝对变化量 ≤ 100 B"改为"增长 ≤ 100 B（减少不受限）"。

### P1-2 Wi-Fi 关闭构建的 81,920 B 门槛按现有证据仍会 FAIL，报告未报告该矛盾（`main/modules/system/SystemMemory.hpp:38`，`docs/reports/HB.md` H1a）

证据链：
- 卡片自带的实测表：W2 之前 `7c06e5d`（无 Wi-Fi 库、无 web 模块）`min = 81,048 B`，已经低于 81,920 B；
- 当前默认镜像 `min = 58,624 B`，而 `builds/h1-wifi-off` 相对 `builds/h1-idf` 的静态段为 `.data −6,220 B`、`.bss −16,016 B`，合计 **+22,236 B 堆**；
- 58,624 + 22,236 = **80,860 B < 81,920 B**。

也就是说去掉 Wi-Fi 后 `internal_budget` 仍会大概率显示 FAIL（差约 1 KB）。卡片 §1 明确要求"关闭：保持 81,920 B"，同时 AGENTS.md §7 要求"发现矛盾或无法实现时停下，在报告中写明证据"——实现方按卡片实现没有错，但报告缺少这条风险说明。

建议：报告补一节"Wi-Fi 关闭门槛风险"，并把它列为 P2 上板项：烧 `builds/h1-wifi-off` 镜像，启动 6 s 后 `sys heap` 记录 INTERNAL|8BIT 的 min。若实测 < 81,920，请 Architect 决定是把 ADR-0011 Q7 改为"实测基线 + 余量"（例如 80,896 B），还是接受该构建长期 FAIL；否则本卡"门槛对齐"在 G3-idf-wifi-off 上并未达成。

### P2-1 `web_load.py` 的 `missing`/`disconnects` 是常量，不能支撑"0 缺帧"（`tools/hil/web_load.py:69-89`）

问题：`status_load()` 返回 `"missing": 0, "disconnects": 0` 是字面量；真正测量的只有 `intervals`、`max_gap_ms` 与每帧 1.8 s 期限。`uptime_s` 只校验"严格递增"（`:81-83`），不校验 Δ==1。设备推送周期为 1000 ms（`WebModule.cpp:786`、`WebModule.hpp:40`），单帧丢失会产生约 2 s 间隔，通常能被 1.8 s 期限抓到；但 `missing` 字段本身不是测量值，且开头两帧（`:71-72`）的 Δ 被丢弃。

影响：报告的 JSON 摘要里 `"missing": 0` 看起来是测量值，实际不是；卡片 H1c 的"0 缺帧"验收依赖它。

建议：按 uptime 增量累计 `missing += max(0, Δ−1)`，并在 Δ>1 时直接判 FAIL（或写入 `gaps` 列表）；`disconnects` 要么删除（连接关闭本来就会抛异常），要么注明"由异常路径保证"。修完重跑 `--selftest`（补一条"丢一帧 → missing>0"的用例）。

### P2-2 `LWIP_MAX_ACTIVE_TCP` 16→6 没有静态/稳态收益，只收紧上限（`sdkconfig.defaults:50`，`docs/reports/HB.md:73`）

证据：`components/lwip/port/include/lwipopts.h:105` 为 `MEMP_MEM_MALLOC 1`，lwIP 的 tcp_pcb 走 `mem_malloc`（系统堆）按需分配，不占 `.bss`；`h1-sockets` 的 −240 B 与 `NUM_SOCKETS=6` 少 4 个 `lwip_sock`（约 60 B/个）吻合，MAX_ACTIVE_TCP 部分对静态段贡献 0。httpd 上限 3 个会话，峰值 pcb 约 4 个，16→6 在真实负载下省不到一个 pcb。

影响：这一半改动收益为零，却把"同时活跃 TCP"压到 6（TIME_WAIT 靠 `tcp.c:1856-1876` 杀最旧连接兜底，不是硬故障，但属纯下行）。

建议：候选只保留 `LWIP_MAX_SOCKETS`，`LWIP_MAX_ACTIVE_TCP` 维持 16（或 8）；报告相应改为"本项收益全部来自 socket 表"。

### P2-3 `LWIP_MAX_SOCKETS=6` 是零余量，且与卡片"双客户端"验收耦合（`sdkconfig.defaults:48-49`）

证据：
- IDF 硬性下限：`components/esp_http_server/src/httpd_main.c:526` 要求 `LWIP_MAX_SOCKETS >= max_open_sockets + 3`；`max_open_sockets=3`（`main/modules/web/WebServer.cpp:51,574`）→ 恰好 6，`httpd_start` 的 `6 < 6` 为假，能启动；
- 3 个内部 socket = 监听 TCP + 两个 UDP 控制（`httpd_main.c:360-437`），与报告描述一致；
- 应用内没有其他 socket 使用者（无 mdns/sntp/esp_http_client/`socket()`；DHCP 服务器用 `udp_new()` 原始 API，`dhcpserver.c:1537`），所以 6 在单浏览器下够用；
- 但 `GET /` 未设置 `Connection: close`（`WebServer.cpp:302-318`），浏览器会把页面连接留在连接池，WS 再用一条连接：**2 个浏览器最多 4 个会话 > 3**，`lru_purge_enable=true`（`WebServer.cpp:576`）会静默踢掉最久未活动的会话，可能正是某个 WS。卡片 H1b/H1c 的"两台设备同时打开页面 5 min 不断线"正落在这个风险上。

建议：
- sdkconfig 注释里写明"6 = 3+3，是 IDF 下限，没有余量；若 httpd 的 `max_open_sockets` 提到 4，本值必须同步 ≥ 7，否则 `httpd_start` 直接返回 `ESP_ERR_INVALID_ARG`"；
- 由于该候选本来也 < 1 KB，建议直接给 8（多 2×约 60 B），把余量留出来，或在 P2 先跑双客户端再决定；
- 报告的双客户端步骤里加一条"若出现重连/断线，检查 httpd 是否触发 LRU purge"。

### P3-1 报告引用错误的自身文件名（`docs/reports/HB.md:23`）

表格里写"`docs/reports/H1.md`：构建取证、调优取舍和 P2 验收步骤"，但 `H1.md` 是分流引擎旧任务的报告（`# H1 分流引擎 C++ 执行报告`）。本卡报告是 `docs/reports/HB.md`，应改为 `HB.md`，避免 Architect 把 P2 结果写错文件。

### P3-2 `sdkconfig.defaults` 的候选行全部是注释，缺"实测收益"（`sdkconfig.defaults:32-53`）

卡片要求"每项旁边写一行中文注释说明原因和实测收益"，当前 12 行候选全部以 `# CONFIG_...` 形式停用，收益一律"待测"。在无板条件下这是合理的（`HB.md:77` 也写明由 Architect 在 P2 后启用），但报告应明确：本卡 H1c 在 `sdkconfig.defaults` 上只完成了"候选清单 + 构建取证"，启用与收益回填是 P2 之后对冻结文件的第二次修改，需要 Architect 重算 `frozen.sha256`。当前 `HB.md` 的 H1c 表述容易被读成已完成。

### P3-3 `ampdu` 候选未说明保留 TX AMPDU 的理由（`sdkconfig.defaults:41-42`，`docs/reports/HB.md:71`）

卡片给的是"关闭，或保留 RX 并把窗口调小"；实现选择"TX/RX 都保留，只把 RX_BA_WIN 6→4"。这是更保守的选择（上传方向不受影响），但报告没写为什么不动 TX AMPDU，也没写"RX_BA_WIN 4 仍 ≥ 2，且 static-rx 8 ≥ 4，两个候选可组合"这条约束核对。补一句即可。

### P3-4 `tools/hil/README.md` 未记录 `web_load.py`

同目录的 `spp_rtcm_mix.py` 有完整使用说明（环境、常用命令、参数表），新脚本没有对应章节。P2 由 Architect 执行，缺少"用哪个 Python、需要 websockets、如何解读 JSON"的说明会降低可复现性。

### P3-5 host 测试的两个小缺口（`tools/main_host_check/BootAllocCheck.cpp:480-501`）

- 未覆盖 `heaps.empty()`：此时 `printInternalHeapBudget(0,0,...)` 输出 FAIL，属于原有行为，但本次重构后没有用例钉住；
- `internalHeapBudget()` 没有直接单测，只通过格式化输出间接覆盖（可接受）。补 1 条即可。

## 3. 候选项安全核对（对应评审要点 2）

| 候选 | 事实核查 | 结论 |
| --- | --- | --- |
| `dynamic-rx` 32→16 | `esp_wifi/Kconfig:41-44` range 0..128；动态 RX 缓冲按需分配，空闲不占堆，只降峰值 | 安全；报告"空闲收益未必可见"的判断正确，需峰值/min 证据 |
| `dynamic-tx` 32→16 | 同上（TX 动态缓冲） | 安全；首页 gzip 发送与双客户端是重点 |
| `static-rx` 10→8 | `Kconfig:25-39` 建议 `STATIC >= RX_BA_WIN`；8 ≥ 6（BA=4 时也 ≥）；每缓冲约 1.6 KB → 约 3.2 KB | 安全，收益最直接；报告"仍容纳 BA 窗口 6"与 Kconfig 措辞一致 |
| `mgmt` 32→16 | `Kconfig:263-266` range 6..32；16 合法；帮助文本称缓冲按报文长度动态分配 | 安全；空闲堆收益需实测（可能为 0） |
| `ampdu` RX_BA_WIN 6→4 | `Kconfig:199-206` range 2..32，默认 6，文档明说非特殊原因不要改；保留 TX/RX AMPDU | 功能安全，吞吐需测；见 P3-3 |
| `wpa` 关 SAE/OWE | SoftAP 固定 `WIFI_AUTH_WPA2_PSK`、`pmf capable=true/required=false`（`WebServer.cpp:550-552`）；应用代码无 SAE/OWE 引用；`CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y` 保留 | **WPA2-PSK SoftAP 不受影响**；DRAM −288 B、flash −61,492 B；见 P1-1 |
| `sockets` | 见 P2-3：6 = 3 + 3，恰好等于 IDF 下限；无其他 socket 使用者 | 能启动，零余量；MAX_ACTIVE_TCP 部分无收益（P2-2） |
| `recvmbox` 32→16 | TCP/IP 邮箱由 `sys_mbox_new` 动态创建，16 个指针 × 4 B = 64 B | < 1 KB，报告判断正确 |
| 保持 `SND_BUF/WND=5760` | 1440 B MSS × 4 | 符合卡片"保持"选项 |

## 4. 脚本度量覆盖（对应评审要点 4）

| 卡片指标 | 脚本实现 | 评审 |
| --- | --- | --- |
| 首页冷加载中位数/P90 | `home_load()`：每轮新 curl 进程，`--compressed --write-out %{http_code} %{time_total}`，20 次 | 达标；`--noproxy "*"` 避免代理干扰，200 校验合理 |
| WS 600 s 1 Hz 无缺帧/断线 | `status_load()`：`dev.uptime_s` 周期帧，每帧 1.8 s 期限，断开/bye/uptime 回退即抛错 | 超时与异常路径可靠；`missing` 字段是常量（P2-1） |
| 配置往返 20 次中位数 | `ack_load()`：`{"t":"set","id","key","value"}` → 匹配同 id 的 `{"t":"ack","ok":true}`；异常时恢复原值 | 字段与设备端一致（`web_proto/src/Request.cpp:310-318`、`WebLogic.cpp:474-479`、`KeyTable.hpp:34-35` 的 0..40 范围）；测的是设备侧 set→ack，页面 JS 段不在内，报告的"手动交叉核对"可接受 |
| 2 客户端 5 min | 脚本无此模式，报告列为人工步骤 | 可接受，但见 P2-3：3 会话上限使该步骤最可能失败，应写进步骤预期 |

其他核对：`SET_KEY` 的字符串长度 22，与 `gnss.gga_gate.min_sats` 一致（内容由占位符替换，未逐字节核对）；`connect(..., proxy=None, compression=None)` 在 websockets 17.3 上可用；客户端默认 20 s ping 由 httpd 自动 PONG，不会误关连接。

## 5. 未验证项

- H1b/H1c 的板载指标（free/min/largest、首页中位数/P90、WS 缺帧、ack 中位数、双客户端）——无板，全部 NOT_RUN；本评审只验证脚本与门槛逻辑。
- `G3-idf-wifi-off` 的实际 `sys heap` —— 见 P1-2，需 P2 测量。
- 报告提到的"首轮门禁在沙箱内 LeakSanitizer 失败"属于实现方环境记录，无法复核，不影响结论。

## 6. 建议的合并前动作

1. 报告修正 P1-1（取舍理由与 flash 收益）与 P3-1（文件名）。
2. 报告补 P1-2 风险节，并把"关闭 Wi-Fi 构建 min 实测"加入 P2 步骤。
3. `web_load.py` 的 `missing` 改为实算（P2-1），补一条自检用例。
4. `sockets` 候选拆分为 `LWIP_MAX_SOCKETS=6`（或 8）与保持 `LWIP_MAX_ACTIVE_TCP=16`（P2-2），注释写明零余量约束（P2-3）。
5. P2 先跑双客户端，再决定 `max_open_sockets` 是否需要提到 4。
