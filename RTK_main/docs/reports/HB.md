# HB（任务卡 H1-heap-budget）堆门槛与 Wi-Fi 配置模式调优

分支 `feat/h1-heap-budget`，基线 `8c28078`，任务卡提交 `e2e4db1`。构建目标为 ESP32，工具链为 ESP-IDF v6.1、Xtensa GCC 15.2，Python 环境为 `idf6.1_py3.13_env`。

## H1a 堆门槛

`sys mem` 和 `sys heap` 按镜像的 `CONFIG_RTK_WIFI_CONFIG_ENABLE` 选择门槛。链入 Wi-Fi 的单镜像在 BT 和 Wi-Fi 两种启动模式都用 40,960 B。Wi-Fi 关闭的构建用 81,920 B。两类构建的最大连续内部 8 位堆块均须达到 32,768 B。

```text
internal_budget PASS (min>=40960 B, largest>=32768 B)
internal_budget_source adr0017
```

Wi-Fi 关闭时来源为 `adr0011`。`sys mem` 的预算字段及值格式沿用原格式，`sys heap` 原先只显示堆数值，本次也调用共用的预算输出。

### Wi-Fi 关闭门槛风险

任务卡记录的 W2 之前镜像 `7c06e5d` 启动约 6 s 的 min 为 81,048 B，低于 81,920 B。对比本次 map，`h1-wifi-off` 相对 `h1-idf` 的静态 DRAM 减少 22,236 B（.data −6,220 B，.bss −16,016 B）。只按静态差值估算，将 22,236 B 加到任务卡主线镜像实测的 58,624 B 得 80,860 B，仍低 1,060 B。运行期分配差异需上板确认。门槛按任务卡保留 81,920 B，见下方关闭 Wi-Fi 镜像验收步骤。若实测不足，由用户在 Architect 提交实测后决定后续门槛。

| 文件 | 改动 |
| --- | --- |
| `main/modules/system/SystemMemory.hpp` | 具名 `constexpr` 门槛、统一的比较和来源输出 |
| `main/modules/system/SystemCmds.cpp` | 两个控制台入口传入编译配置 |
| `tools/main_host_check/BootAllocCheck.cpp` | 两种配置各覆盖等于门槛通过、min 差 1 B 失败、largest 为 32,767 B 失败。另验证空堆视图判失败，直接检查 constexpr 门槛 |
| `tools/hil/web_load.py` | 首页、周期状态和配置往返采样，输出 JSON，提供离线自检 |
| `tools/hil/README.md` | Wi-Fi 负载脚本的环境、命令、参数和结果判读 |
| `sdkconfig.defaults` | 中文注释记录表内候选，通过独立 defaults 文件构建；实测达标后再启用对应项 |
| `docs/reports/HB.md` | 构建取证、调优取舍和 P2 验收步骤 |

## 本地验证

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 初版 `tools/ci/run_gates.sh --base 8c28078 --prefix h1` | 35 PASS，1 预期 FAIL | `builds/h1-logs/summary.log` |
| G1 冻结校验 | FAIL（任务卡例外） | 仅 `sdkconfig.defaults` 校验和偏离，`builds/h1-logs/G1-frozen.log` |
| G2、G2b 含 GNSS 全驱动 | PASS | 普通与 ASan/UBSan/LSan 各 12 组。两次 `main_host_check` 各 77 个测试通过，含 4 个 boot alloc 变体 |
| G3、G3b | PASS | 默认、关闭 trace、关闭 Wi-Fi、production、console-none，以及 rtk_pure ESP32、ESP32-C3 |
| G5、G5b、G6、G7b | PASS | clang-format、clang-tidy、函数注释、运行期分配门禁日志在 `builds/h1-logs/` |
| `python3 -I tools/hil/web_load.py --selftest` | PASS | 20 次首页计时及百分位、状态采样、超时、ack ID 匹配和配置回原值 |
| 八组候选独立 IDF 构建 | PASS | `builds/h1-logs/<候选>.rebuild.log`，检查生成的 `sdkconfig` 与 map |
| `python -m esp_idf_size --archives --diff` | PASS（默认镜像） | 与指定 BT 基线比较为 0 B，分库结果见下节 |
| OMP 交叉评审 | 已完成 | `docs/reports/HB-review-omp.md` |
| P2 堆与性能、H1b | 完整验收待执行 | P2 基线状态采样已试跑，修正判据后的结果待 Architect 复测 |

首轮门禁在沙箱内运行，LeakSanitizer 因 ptrace 限制退出，G6 的 `uvx --offline` 因缓存只读无法加锁，G5b 报出两处字面量需具名。将门槛具名后，用本机已缓存的 lizard 1.26.1 配合 IDF Python 执行 G6，并在沙箱外按原参数复跑整套门禁，保留泄漏检查。首轮汇总见 `builds/h1-logs/summary-first.log`。

OMP 评审修订后的复验如下。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| `cmake --build` 与 `ctest --output-on-failure`，构建目录 `builds/h1-main_host_check` 和 `builds/h1-main_host_check-asan` | PASS | 普通和 ASan/UBSan/LSan 各 77/77。日志见 `builds/hb-logs/host.log` 和 `host-asan.log` |
| `clang-format --style=file --dry-run --Werror`，三个改动的 C++ 文件 | PASS | `SystemCmds.cpp`、`SystemMemory.hpp`、`BootAllocCheck.cpp` |
| `python3 -I tools/doc_check/doc_check.py --base 8c28078` | PASS | 5 个受检函数均合格 |
| `python3 -I tools/hil/web_load.py --selftest` 和模拟 WS 的 CLI 检查 | PASS | P2 误报修正后，uptime 4、5、7 且末两帧间隔 1.15 s 时缺帧数为 0，间隔 2.0 s 时为 1。缺帧累计到采样结束，超时判失败 |
| 新版 socket 候选 `idf.py build` 和 `python -m esp_idf_size --archives --diff` | PASS | `builds/hb-sockets/` 内生成配置为 8/16。DRAM 减少 120 B，IRAM 余量为 16,357 B |

### BT 模式静态 DRAM

在已激活的 IDF 环境执行以下命令。该环境用 Python 模块入口提供 `esp_idf_size`。

```sh
baseline=../../RTK_Main/RTK_main/builds/mainw2-idf/rtk_main.map
python -m esp_idf_size --archives --diff "$baseline" builds/h1-idf/rtk_main.map
python -m esp_idf_size --show-unchanged --diff "$baseline" builds/h1-idf/rtk_main.map
```

| 指标 | mainw2-idf | h1-idf | 增量 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 97,483 B | 97,483 B | 0 B |
| DRAM .data | 51,099 B | 51,099 B | 0 B |
| DRAM .bss | 46,384 B | 46,384 B | 0 B |
| IRAM 已用 | 114,715 B | 114,715 B | 0 B |
| IRAM 余量 | 16,357 B | 16,357 B | 0 B |

分库对比和整图的 DRAM 增量均为 0 B，满足静态 DRAM 增长 ≤ 100 B 的要求，减少不受限。单镜像的静态段随镜像链接确定，BT 模式适用同一份 map。IRAM 余量 16,357 B ≥ 8 KiB。结果见 `builds/h1-logs/size.archives.log` 和 `size.size.log`。

## H1c 候选构建与取舍

每组独立与同一个 mainw2-idf map 比较。下表的堆收益预期来自 SDK 源码或 Kconfig。P2 的 free/min/largest、首页、WS、ack 和双客户端完整验收数据待回填。全部八组的 IRAM 余量均为 16,357 B。

| 候选 | 基线 → 试值 | 原理、预期与逐项上板重点 | 静态 DRAM Δ | 取舍 |
| --- | --- | --- | ---: | --- |
| `static-rx` | `ESP_WIFI_STATIC_RX_BUFFER_NUM` 10 → 8 | `esp_wifi_init()` 少分配两个约 1.6 KB 的接收缓冲，理论约 3.2 KB。仍容纳 BA 窗口 6。先核对 AP 启动，核对连接、首页并发和接收突发时的 WS | 0 B | 优先测 |
| `dynamic-rx` | `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` 32 → 16 | 限制突发接收时的在途缓冲。保留静态 RX 10 所需余量。空闲收益未必可见。重点看两台终端并发请求和 ack 时的 min、掉帧 | 0 B | 待实测 |
| `dynamic-tx` | `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` 32 → 16 | 限制在途发送缓冲。首页 gzip 发送及双终端状态推送可能触及上限。重点比较首页中位数/P90、WS 间隔和 minimum | 0 B | 待实测 |
| `mgmt` | `ESP_WIFI_MGMT_SBUF_NUM` 32 → 16 | 缩小短管理帧池。优先核对两台终端连续加入，以及断开后一台重连，记录连接、DHCP 和堆水位 | 0 B | 待实测 |
| `ampdu` | `ESP_WIFI_RX_BA_WIN` 6 → 4，TX/RX AMPDU 保持 y | 缩小接收重排窗口，保留 TX 聚合以维持发送吞吐。窗口 4 ≥ IDF 下限 2，且 static-rx 候选的 8 ≥ 4，可组合复验。重点测 20 次首页加载、两类终端的 WS 和重传时延 | 0 B | 待实测 |
| `wpa` | `ESP_WIFI_ENABLE_WPA3_SAE`、`ESP_WIFI_SOFTAP_SAE_SUPPORT`、`ESP_WIFI_ENABLE_WPA3_OWE_STA`、`ESP_WIFI_ENABLE_WPA3_OWE_SOFTAP` y → n | AP 使用 WPA2-PSK，SoftAP SAE 随父选项失效。手机重新关联并验证 DHCP、页面和双终端。Flash Code 减少 61,452 B（约 61 KB）。分库统计中 `libwpa_supplicant.a` 各段合计减少 40,659 B | −288 B | 满足静态增长约束。已知静态差值低于 1,024 B，动态堆收益待 P2 单项测量及组合评估，暂缓单独启用 |
| `sockets` | `LWIP_MAX_SOCKETS` 10 → 8，`LWIP_MAX_ACTIVE_TCP` 保持 16 | IDF 要求 `LWIP_MAX_SOCKETS >= max_open_sockets + 3`，当前 3 个会话加监听与两个 UDP 控制 socket 的下限为 6。试值 8 留 2 个余量。TCP PCB 按需分配。检查 httpd 启动、两个浏览器加单次 GET 及断开后恢复 | −120 B | 满足静态增长约束，变化全部来自 `liblwip.a` 的 socket 表 .bss。已知静态差值低于 1,024 B，动态堆收益待 P2 单项测量及组合评估，暂缓单独启用 |
| `recvmbox` | `LWIP_TCPIP_RECVMBOX_SIZE` 32 → 16 | ESP32 的 TCP/IP 邮箱少存 16 个 4 B 指针，直接节省 64 B。若实测，重点用双客户端并发排查队列压力和载入尾延迟 | 0 B | 预计直接收益低于 1 KiB |
| 保持 TCP 默认值 | `LWIP_TCP_SND_BUF_DEFAULT` / `LWIP_TCP_WND_DEFAULT` 保持 5760 / 5760 B | 窗口会影响首页传输；SDK MSS 为 1440 B，保留 4×MSS。首页、WS 和 ack 基线采样继续使用此值 | 0 B（沿用默认） | 采用卡片的保持选项 |

分库和总量逐项存为 `builds/h1-logs/<候选>.archives.log` 和 `<候选>.size.log`。新版 socket 候选的结果见 `builds/hb-sockets/archives.log` 和 `size.log`，该镜像替代旧的 6/6 候选。本阶段完成 `sdkconfig.defaults` 候选清单和独立构建取证。P2 上逐项确认至少 1,024 B 动态堆收益、性能和静态门槛达标后，由 Architect 第二次修改冻结配置，启用通过项、在旁边补中文实测收益注释并重算 `docs/frozen.sha256`。组合配置再跑同一套门禁和板载步骤。

### 重建单项候选

候选镜像已在 `builds/h1-<候选>/rtk_main.bin`，新版 socket 镜像在 `builds/hb-sockets/rtk_main.bin`。以下流程以新前缀重建单项候选，`name` 替换为表中候选名。

```sh
export IDF_PATH="$HOME/esp/esp-idf-v6.1"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf6.1_py3.13_env"
export IDF_COMPONENT_MANAGER=0
. "$IDF_PATH/export.sh"
name=static-rx
out="builds/hb-$name"
mkdir -p "$out"
awk -v name="$name" '$0 ~ "^# " name "：" { active=1; next }
    active && /^# CONFIG_/ { print substr($0, 3); next }
    active { exit }' sdkconfig.defaults > "$out/candidate.defaults"
test -s "$out/candidate.defaults" &&
    idf.py -B "$out" -DSDKCONFIG="$PWD/$out/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$out/candidate.defaults" build
```

## Architect 验收

### H1b 负载

1. 用 P2 台架和默认 `builds/h1-idf` 镜像。保持现有用户数据和可回退镜像。观察启动进入 `kBluetooth`，用 `radio` 核对模式，设置并确认 GNSS 1 Hz 输出。
2. 配对手机并保持 SPP，持续下发 RTCM 10 min。开始、过程和结束采集 `rtcm stats`、`sys mem`、`sys tasks` 和复位日志，确认 RTCM 持续转发、GNSS 仍输出。预期 `internal_budget PASS`，min ≥ 40,960 B，largest ≥ 32,768 B，任务余栈无持续下降、无 TWDT。
3. 执行 `radio wifi`，连设备 SoftAP，开一个浏览器访问 `http://192.168.10.12/` 并前台运行 10 min。开始和结束执行 `sys mem`，记录 INTERNAL|8BIT 的 free/min/largest。预期 1 Hz 状态连续，两个堆门槛同 BT，来源 `adr0017`。
4. 在 P2 上烧录 `builds/h1-wifi-off`，使用与任务卡基线一致的启动条件。启动约 6 s 后执行 `sys heap`，记录内部 8 位堆 free/min/largest 和预算判定，确认来源 `adr0011`。min < 81,920 B 时保留失败日志，交用户决定门槛。
5. 回填下表，附日志和镜像标识。

| 项 | free B | min B | largest B | 结果 |
| --- | ---: | ---: | ---: | --- |
| BT + 10 min RTCM（手机 NTRIP，4,559 帧 / 619,123 B 全部转发，0 丢弃）/ GNSS 1 Hz | 58,112 | 54,956 | 57,344 | PASS（`h1-idf`，2026-11-20） |
| Wi-Fi + 单客户端 600 s（`web_load.py`，0 缺帧） | 74,484 | 55,932 | 73,728 | PASS（`h1-idf` 调优前） |
| Wi-Fi + 双客户端 300 s，0 缺帧 0 断线 | 73,820 | 55,932 | 69,632 | PASS（`h1-idf` 调优前） |
| Wi-Fi + 单客户端 60 s，手册内存档 | 86,836 | 70,180 | 86,016 | PASS（`hb-doc`，见下节） |
| Wi-Fi 关闭，启动约 6 s | — | 80,776 | — | FAIL（< 81,920，门槛待用户决定） |

BT 负载期间 min 从第 1 分钟起不再下降；任务余栈结束值 link 2,416、io 1,744、console 2,212、idle 976 B，无复位或 TWDT。GNSS 1 Hz 输出在本轮未单独核对。

### H1c 基线与逐项 A/B

1. 先用默认镜像建立对照。每轮重新启动进入 Wi-Fi 配置模式，用同一台 P2、同一主机和手机、相同信道与距离，保证闲置时间和 GNSS 设置一致。按上表次序每次只替换一组候选镜像，WPA 与 socket 候选同样按运行期收益和性能实测取舍。
2. 单浏览器访问设备首页，确认有 `hello`、`config` 和约 1 Hz 的 `status`。在页面刚打开和保持打开 10 min 时分别执行 `sys mem` 并记录 free/min/largest。候选三项均须不低于基线，且 H1b 堆门槛继续通过。
3. 关闭浏览器页，用同一主机运行下列脚本。脚本依序执行 20 次独立 curl GET、单个 WS 会话的 600 s 状态采样，以及 20 次配置往返。运行环境已确认有 curl 和 IDF Python 的 `websockets` 17.3。

   ```sh
   python3 -I tools/hil/web_load.py --selftest
   python -I tools/hil/web_load.py --duration 600 --json-out builds/h1-logs/p2-baseline.json
   ```

   每个候选使用独立文件名。可用 `--stage home|status|ack` 单独重跑对应阶段。P2 基线在第 105 帧观察到整数 uptime 增加 2，但当时最大帧间隔仅 1,149 ms，旧判据误报并中止采样。修正后，相邻帧到达间隔 `gap_s` 超过 1.5 s 时累计 `round(gap_s) - 1` 个缺帧，全量帧到首个周期帧也采用同一判据。从首个周期帧起跑满 `--duration` 后统一按 `missing` 判定。周期帧 uptime 须递增，首个周期帧可与全量帧同秒。`uptime_jumps` 只记录增量大于 1 的次数。5 s 内未收到状态帧或 WS 断开会中止并判失败，原因见 `error`。协议没有序号，结合浏览器 Network → WS 的帧记录交叉核对 1 Hz 连续性。ack 阶段在同一 WS 交替保存 GGA 的 `min_sats` 相邻值和原值，20 次以原值结束。中途中断时脚本尝试恢复，若摘要提示恢复失败，按提示的原值通过网页恢复并用 `gnss status` 复核。

   成功摘要的字段示例，`null` 代表现场待填数值。

   ```json
   {
     "host": "192.168.10.12",
     "verdict": "PASS",
     "home": {"samples": 20, "median_ms": null, "p90_ms": null},
     "status": {"duration_s": 600.0, "intervals": 600, "max_gap_ms": null, "missing": 0, "uptime_jumps": 0},
     "ack": {"samples": 20, "median_ms": null, "p90_ms": null}
   }
   ```

4. 手动确认网页保存设置到 `ack` 的路径与脚本一致。首页与 ack 中位数各须 ≤ 基线中位数 × 1.20，首页 P90 同样记录。用两台设备同时打开浏览器页面并保持前台 5 min，预期都持续收到状态且无断线或重连。若出现重连或断线，检查 httpd 是否触发 LRU purge。两个浏览器的首页 HTTP 连接和 WS 可能短时占用 4 个会话，超出 httpd 当前的 3 个会话上限。记录页面帧和 httpd 日志，先完成双客户端复现，再由 Architect 决定会话上限。
5. 回填每项的 free/min/largest、首页中位数/P90、WS 缺帧与断线数、ack 中位数、双客户端结果及取舍。记录至少一个堆指标 ≥ 1,024 B 的可重复动态收益，所有堆指标均须不退步。通过项组合后重复 A/B 和 BT 堆预算检查。

### H1c 定稿：按 ESP-IDF 手册取值（替代逐项 A/B）

逐项 A/B 跑完基线、`static-rx`、`dynamic-rx` 后由用户叫停：首页中位数在 166–259 ms 间波动，20 次样本不足以判定 ±20%，且全部取舍落在吞吐需求（约 1 KB/s）的数个数量级之下。改为按 ESP-IDF v6.1.1 手册取值：

- `api-guides/wifi-driver/wifi-performance-and-power-save.html` “How to Configure Parameters” 的 Memory saving 档（TCP TX/RX 33.8/30.1 Mbit/s）：`ESP_WIFI_STATIC_RX_BUFFER_NUM=6`、`DYNAMIC_RX/TX_BUFFER_NUM=12`、`RX_BA_WIN` 保持 6；TCP 窗口保持默认 5,760 B（已小于该档 12 KB）；Wi-Fi/lwIP IRAM 优化保持开启。
- `wifi-driver/overview.html`：静态 RX 10→6 省 6,400 B，开 AMPDU 时不得低于 6。
- `api-guides/lwip.html`：关闭 `LWIP_IPV6`（固件只用 IPv4）；`LWIP_MAX_SOCKETS=8`（httpd 要求 ≥ max_open_sockets 3 + 3）。
- 关闭 WPA3 SAE/OWE（SoftAP 只用 WPA2-PSK）。管理帧缓冲、TCP/IP 邮箱保持默认。

| 指标 | `h1-idf` 调优前 | `hb-doc` 定稿 | 变化 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 97,483 B | 95,627 B | −1,856 B |
| Flash 代码+数据 | 1,365,396 B | 1,272,920 B | −92,476 B |
| IRAM | 114,715 B | 114,715 B | 0 |
| Wi-Fi 模式 free（结束） | 74,484 B | 86,836 B | +12,352 B |
| Wi-Fi 模式 min | 55,932 B（600 s） | 70,180 B（60 s） | +14,248 B |
| Wi-Fi 模式 largest | 73,728 B | 86,016 B | +12,288 B |
| 首页中位数 / P90 | 166 / 485 ms | 220 / 367 ms | 处于本机波动范围 |
| 设置 ack 中位数 | 615 ms | 609 ms | 持平 |
| 状态帧缺失 | 0 | 0 | — |

`hb-doc` 只跑了 60 s 快检，min 比 600 s 基线少 540 s 观察窗。性能进一步优化（含 ack 约 600 ms 的来源，见 SV2 评审 §3）按用户要求排在全部功能完成之后。

## 文档待改

- Architect 在 ADR-0011 Q7 及引用处补充构建条件。链入 Wi-Fi 使用 ADR-0017 §3 的 40,960 B，关闭使用 81,920 B，largest 均为 32,768 B。
- Architect 同步核对 AGENTS.md 和 K 系列任务卡的统一 81,920 B 表述。当前检出匹配到 K2、K4、K5 的相关条款。
- Architect 在定稿后更新 `docs/frozen.sha256` 中的 `sdkconfig.defaults`。P2 验收结果补入本报告，OMP 评审见 `docs/reports/HB-review-omp.md`。

