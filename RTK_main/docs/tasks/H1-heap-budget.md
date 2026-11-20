# H1 堆门槛对齐 + Wi-Fi 配置模式运行期内存调优

依据：用户 2026-11-20 决定（保持单镜像；门槛对齐 ADR-0017；Wi-Fi/lwIP 可调优，前提是不明显影响性能）；ADR-0017 §3；ADR-0011 Q7；docs/reports/W0.md。
基线：main 8c28078。分支 `feat/h1-heap-budget`（独立 worktree）。实现：Codex；交叉评审：OMP；Architect（Claude）验收、改冻结文档、提交、上板。

## 0 背景（Architect 2026-11-20 P2 实测，每个镜像重新烧录，启动约 6 s 后 `sys heap`）

| 镜像 | 堆 total | free | min | largest |
|---|---|---|---|---|
| W2 之前 `7c06e5d` | 223,700 | 85,740 | 81,048 | 77,824 |
| main `8c28078` | 201,220 | 63,372 | 58,624 | 57,344 |
| SV1 `1105c35` | 201,188 | 63,348 | 58,600 | 57,344 |

- 堆 total 少 22,480 B，与静态 DRAM 增量 22,476 B 一致：libnet80211 +10,704、libpp +4,211、lwIP +2,482、main +2,360、wpa_supplicant +1,715、其他约 +830。这些是 Wi-Fi 预编译库的静态数据，只要链入 Wi-Fi 就占用，与是否启动 Wi-Fi 无关。W0 已证实缓冲调参对这部分的影响 ≤ 80 B。
- 用户 2026-11-19 已接受这 21 KB，保持单镜像（ADR-0017 §3），**本卡不做关 Wi-Fi 的第二镜像**。
- 问题：`sys mem`/`sys heap` 的 `internal_budget` 仍按 ADR-0011 Q7（min ≥ 81,920 B、largest ≥ 32,768 B）判定，三个镜像（含 W2 之前）都显示 FAIL，和 ADR-0017 的负载门槛（min ≥ 40 KB）矛盾。
- 网页 `index_html_gz` 在 flash（`.rodata.embedded`，0x3f425a64），不占 RAM，不在本卡范围内。

## 1 H1a 门槛对齐（代码）

1. `main/modules/system/SystemMemory.hpp` `printMemoryReport()`：`kInternalMinBudget` 按编译开关选择：
   - `CONFIG_RTK_WIFI_CONFIG_ENABLE=y`：min ≥ 40,960 B（ADR-0017 §3 的 40 KB）；
   - 关闭：保持 81,920 B（ADR-0011 Q7，`G3-idf-wifi-off` 构建）。
   - `kInternalLargestBudget` 两种情况都保持 32,768 B（ADR-0011 为以后 16 KiB 记录环留的余量）。
   - 输出格式不变：`internal_budget PASS|FAIL (min>=N B, largest>=M B)`；额外打印门槛来源（`adr0017`/`adr0011`），便于读日志。
   - 常量放在一个具名 `constexpr` 函数或结构里，不要散落在多处。
2. `tools/main_host_check/BootAllocCheck.cpp` `testMemoryReport()`：覆盖两种开关下的边界（门槛值 PASS、门槛值 −1 FAIL、largest 32,767 FAIL）。host 构建如果只能编一种开关，就把选择逻辑做成可传参的纯函数，直接测这个函数。
3. 不改 ADR 和 docs/**（冻结）。需要改的文字列在报告“文档待改”一节，由 Architect 改：
   - ADR-0011 Q7 补一句：链入 Wi-Fi 的镜像改用 ADR-0017 §3 的门槛；
   - AGENTS.md 和 K 系列卡片里“负载下 heap min ≥ 81920”的引用改为按构建区分。

## 2 H1b 负载验收（Architect 上板）

W2 HIL 第 7 步（BT RTCM 负载）仍未做，在本卡一起完成：
- `kBluetooth` 模式：手机连接，持续下发 RTCM，GNSS 1 Hz 输出，运行 10 min，`sys mem` min ≥ 40,960 B、largest ≥ 32,768 B，任务余栈不持续下降，无 TWDT。
- `kWifiConfig` 模式：1 个浏览器打开页面，1 Hz 状态推送，运行 10 min，记录 free/min/largest（门槛同上）。
- 结果写进 docs/reports/H1.md 的“Architect 验收”一节。

## 3 H1c Wi-Fi 配置模式运行期调优（不明显影响性能）

目标：只降 `kWifiConfig` 模式启动 Wi-Fi 后的动态堆占用；`kBluetooth` 模式的静态 DRAM 变化须 ≤ 100 B（用 `esp_idf_size --archives --diff` 证明）。

**先测基线**（当前配置，P2，SoftAP + 1 个客户端），再逐项改、逐项测。每项单独记录：

| 指标 | 方法 | 允许回退 |
|---|---|---|
| Wi-Fi 模式 free / min / largest | 连接后、页面打开、1 Hz 推送 10 min，`sys mem` | 不允许变差 |
| 首页冷加载 | 主机连 SoftAP，`curl -w %{time_total}` GET `/` 20 次，取中位数/P90 | 中位数 +20% 以内 |
| WS 推送 | 10 min 内 1 Hz `status` 无缺帧、无断线 | 0 缺帧 |
| 配置往返 | 页面保存一项设置到收到 `ack`，20 次中位数 | +20% 以内 |
| 2 客户端 | 两台设备同时打开页面 5 min | 不断线 |

测量脚本放在 `tools/hil/web_load.py`（无板自检 `--selftest`，风格同 `tools/hil/spp_rtcm_mix.py`）。上板由 Architect 执行；执行方给出脚本和预期输出。

**候选项**（当前值来自 sdkconfig；每项先在报告里说明原理和风险，再测）：

| 项 | 当前 | 候选 | 备注 |
|---|---|---|---|
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 32 | 16 | 1–2 个客户端足够 |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 32 | 16 | |
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 10 | 6–8 | 每个约 1.6 KB，启动时分配 |
| `ESP_WIFI_MGMT_SBUF_NUM` | 32 | 16 | |
| `ESP_WIFI_AMPDU_TX/RX_ENABLED`、`RX_BA_WIN` | y / y / 6 | 关闭，或保留 RX 并把窗口调小 | 吞吐影响最大，必须测首页加载 |
| `ESP_WIFI_ENABLE_WPA3_SAE`、`SOFTAP_SAE_SUPPORT`、`WPA3_OWE_*` | y | n | SoftAP 只用 WPA2-PSK（ADR-0017 §2.3），确认手机仍能连接 |
| `LWIP_MAX_SOCKETS` / `LWIP_MAX_ACTIVE_TCP` | 10 / 16 | 按 httpd `max_open_sockets` + 3 计算 | 不得低于 httpd 实际配置 |
| `LWIP_TCPIP_RECVMBOX_SIZE` | 32 | 16 | |
| `LWIP_TCP_SND_BUF_DEFAULT` / `TCP_WND_DEFAULT` | 5760 / 5760 | 保持或 2×MSS | 只有首页加载不超出允许回退才改 |

不做：关 `ESP_WIFI_IRAM_OPT`/`RX_IRAM_OPT`（省的是 IRAM 不是堆，还会降性能）；改 BT 控制器内存；改分区表。

规则：
- 只保留“堆收益 ≥ 1 KB 且所有指标都在允许回退以内”的项。每项的取舍和数据写进报告。
- 改动写在 `sdkconfig.defaults`（冻结例外，本卡授权，仅限上表中的项），每项旁边写一行中文注释说明原因和实测收益；frozen.sha256 由 Architect 重算。
- IRAM 余量保持 ≥ 8 KB；G3 全部构建（含 `G3-idf-wifi-off`）通过。

## 4 测试与交付

- `tools/ci/run_gates.sh --base 8c28078 --prefix h1` 全部 PASS（G1-frozen 中 sdkconfig.defaults 的偏离属于预期，在报告中说明）。
- `esp_idf_size --archives --diff` 对比 8c28078：BT 模式静态 DRAM 变化 ≤ 100 B。
- 报告 docs/reports/H1.md：门槛改动、host 测试结果、调优候选项逐项表（原理/风险/堆收益/性能数据/取舍）、上板步骤、“文档待改”一节。
- 不提交、不烧录；打印 `H1 DONE`。OMP 交叉评审写 docs/reports/H1-review-omp.md。
