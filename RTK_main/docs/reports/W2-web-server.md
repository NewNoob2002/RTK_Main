# W2：Wi-Fi 配置模式网页服务实施报告

- **任务卡**：`docs/tasks/W2-web-server.md`（合并 ADR-0017 W2 + W5 + S2）
- **基线**：`main` `c404e96`，分支 `feat/w2-web-server`，工作树 `RTK_Main-w2`
- **构建前缀**：`builds/w2-*`
- **工具链**：ESP-IDF v6.1（`/home/gtc/esp/esp-idf-v6.1`）、Xtensa GCC 15.2、Python 3.13
- **说明**：工作树中 `docs/adr/ADR-0017-wifi-web-config.md`、`docs/tasks/W2-web-server.md`、`docs/web/ws-protocol.md` 存在 Architect 未提交更新（SoftAP 地址改为 `192.168.10.12/24`），本任务完整保留未触碰；未修改任何冻结文件（`G1-frozen` PASS）。

---

## 1. 改动文件

### 新增文件

| 文件 | 用途 |
| --- | --- |
| `main/modules/web/WebMailbox.hpp` | 无锁 SPSC 帧邮箱（静态模板版 `WebFrameMailbox<N>`、外供存储版 `WebFrameMailboxView`）、控制事件邮箱 `WebCtrlMailbox<N>` 及满邮箱回 `busy` 用的零缓冲 `extractRequestId()` |
| `main/modules/web/WebIdleTimer.hpp` / `.cpp` | 纯 C++20 空闲回落计时器（无终端连接 / 最后终端断开计时、期间短连/重连取消、32 位毫秒回绕安全） |
| `main/modules/web/WebLogic.hpp` / `.cpp` | 纯 C++20 协议逻辑层：复用 `components/web_proto` 的键表/校验/JSON 输出，实现 `set` → `fw::Command` 翻译（含 Q1 `gnss.gga_gate.*`）、`hello`/`config`/`status`（按分组差分）/`ack`/`bye` 编码 |
| `main/modules/web/WebModule.hpp` / `WebModule.cpp` / `WebServer.cpp` | `web` 模块（登记为 `kWifiOnly` / `kLink`）：SoftAP（`192.168.10.12/24` + DHCP、`WIFI_STORAGE_RAM`、最多 2 终端）、`esp_http_server`（core 0、最多 3 套接字、`GET /` gzip 嵌入页、`GET /ws`、`close_fn` 关闭套接字、`httpd_queue_work` + `httpd_ws_send_frame_async` 异步发送）、kLink 协议状态机（单在途请求、GNSS 跨执行器异步两阶段等待、1 Hz 状态差分推送、每连接工厂解锁、延迟重启等待 TX 与 GGA 门控 NVS 去抖落盘） |
| `tools/main_host_check/WebServerCheck.cpp` | 新增主机回归测试：帧邮箱（含外供版与 4000 帧双线程 SPSC 并发压力）、超长/满邮箱、全键 `set` 翻译与越界 `bad_value`、`hello`/`config`/`status` JSON 输出与差分、空闲回落计时 |

### 修改文件

| 文件 | 改动摘要 |
| --- | --- |
| `main/Kconfig.projbuild` | 在 `RTK_WIFI_CONFIG_ENABLE` 下新增 `RTK_WIFI_AP_PASSWORD`（默认 `"12345678"`）、`RTK_WIFI_AP_CHANNEL`（1–11，默认 1）、`RTK_WIFI_IDLE_EXIT_S`（60–3600，默认 300） |
| `main/CMakeLists.txt` | 仅在 `CONFIG_RTK_WIFI_CONFIG_ENABLE=y` 时链入 `esp_wifi`/`esp_netif`/`esp_event`/`esp_http_server`/`web_proto`、编译 `modules/web/*.cpp`，并调用 `python3 tools/web_mock/pack.py` 生成构建期 gzip 单页经 `EMBED_FILES` 嵌入（超 24 KB 构建失败）；关该开关时完全不链入 |
| `main/AppConfig.hpp` | 在 `ESP_PLATFORM` 下引入 `sdkconfig.h`；`ModuleKind` 新增 `kWeb` |
| `main/module_list.inc` | 在 `CONFIG_RTK_WIFI_CONFIG_ENABLE` 开启时登记 `WebModule`（`web`、`kWeb`、`kLink`、`kWifiOnly`） |
| `main/app/BootReasons.hpp` / `BootBuffers.hpp` / `BootBuffers.cpp` | 新增 `web.frames`（12 × 1,032 B = 12,384 B）外供缓冲，**仅在 `RadioMode::kWifiConfig` 下从 `BootArena` 分配并绑定**（蓝牙模式零占用）；失败/回滚解绑，新增 `bind:web` 原因常量 |
| `main/app/Executors.cpp` | `stopBootTasks()` 在 `kWifiConfig` 降级路径调用 `s_app.web.stopBoot()` |
| `main/def/StorageDef.hpp` | `StorageVerb` 新增 `kWhitelistAdd`（3）、`kWhitelistDel`（4）、`kSetPairNext`（5）；`StoreStatus` 新增 `btPairNext` |
| `main/modules/storage/StoreKeys.hpp` / `StoreCatalog.cpp` | 新增 NVS 键 `rtk/pair_next`（`kU8`，0..1）及 `"bt"` 分组加载入口 |
| `main/modules/storage/StorageModule.hpp` / `.cpp` | 实现 S2 白名单增删命令（写同一 NVS 报警器/白名单记录并同步刷新 `DataCenter::linkStatus`）与 `pair_next` 读写/一次性消费接口 `consumeBtPairNext()`、公开 `sn()`/`btPairNext()` |
| `main/modules/bt_link/BtLinkModule.cpp` | `BtLinkModule::init()` 末尾检查 `ctx().storage.consumeBtPairNext()`，若置位则立即开启配对窗口并清除该 NVS 键 |
| `main/data/AppData.hpp` / `.cpp` | 新增跨执行器槽 `webGnssCommand`/`webGnssResult` 及无锁原子 `gnssRateHz`、`ggaGateSavePending`（保持零初值） |
| `main/modules/gnss/GnssModule.hpp` / `.cpp` / `GnssModuleCmds.cpp` | 发布 `gnssRateHz` 与 `ggaGateSavePending`；在 `GnssModule::poll()` 中轮询 `webGnssCommand` 并经 `onCommand()` 写回 `webGnssResult`，复用控制台同一校验与持久化路径 |
| `main/web/ui/app.js` | 回退主机地址从 `192.168.4.1` 改为 `192.168.10.12`；增加 `syncSelectValue()` 兼容非下拉预设周期（如 GSV 默认 5000 ms 显示为 `1/5 Hz`） |
| `tools/main_host_check/CMakeLists.txt` / `AppTestAdapter.hpp` / `MainHostCheck.cpp` / `RadioModeCheck.cpp` | 接入 `web_server_check`；修复 host mock `nvs_store_get_alarms` 读回；新增 S2 场景 `web_s2_storage_bt`（`concurrent`/`preempt` 双策略，覆盖 Wi-Fi 写白名单 → BT 读回与 `pair_next` 一次性消费）；更新 `radio_mode_check` 覆盖 `web` 仅在 `kWifiConfig` 启用 |

---

## 2. 资源与镜像用量

### 2.1 固件构建大小（`esp_idf_size` 与 `stat -c %s`）

| 构建目录 | IRAM (B) | IRAM 余量 (B) | 静态 DRAM (B) | DRAM `.data` / `.bss` (B) | DRAM 余量 (B) | Flash Code (B) | Flash Data (B) | 链接镜像 (B) | `rtk_main.bin` (B) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `builds/w2-idf`（DEV，Wi-Fi 开） | 114,715 | 16,357 | 97,067 | 50,683 / 46,384 | 27,513 | 1,110,492 | 254,252 | 1,530,182 | 1,530,304 |
| `builds/w2-trace-off` | 114,715 | 16,357 | 96,659 | 50,283 / 46,376 | 27,921 | 1,099,768 | 247,068 | 1,511,874 | 1,511,984 |
| `builds/w2-wifi-off`（`RTK_WIFI_CONFIG_ENABLE=n`） | 86,287 | 44,785 | 75,215 | 44,847 / 30,368 | 49,365 | 616,464 | 162,184 | 909,822 | 909,936 |
| `builds/w2-production` | 114,715 | 16,357 | 96,659 | 50,283 / 46,376 | 27,921 | 1,084,532 | 240,988 | 1,490,558 | 1,490,672 |
| `builds/w2-console-none` | 114,699 | 16,373 | 94,603 | 50,235 / 44,368 | 29,977 | 1,046,688 | 230,524 | 1,442,186 | 1,442,304 |

- **IRAM 余量**：`16,357 B`（门槛 ≥ 8 KB，通过）。
- **静态 DRAM 对比 W0**：W0 链入 Wi-Fi 基线为 `94,795 B`，本交付为 `97,067 B`（仅 `+2,272 B`）。`WebModule` 的 12 个 1 KB 帧槽位（`web.frames`，`12,384 B`）已改由 `BootArena` 仅在 `RadioMode::kWifiConfig` 分配，**`kBluetooth` 模式下不消耗这 12.4 KB 堆**，从而保持与 W0 相近的蓝牙模式堆余量（W0 实测 BT 空闲堆 61.8 KB，Wi-Fi 空闲堆 92.3 KB；扣除 12.4 KB 后 Wi-Fi 预期仍有约 80 KB 堆余量，实际运行期堆值待 P2 HIL 实测）。
- **堆检查（G7b）**：128 个自有目标文件，0 个未白名单堆符号，0 个 `__cxa_guard`，6/6 ELF 允许符号；项目自身代码无动态分配（仅既有启动期 `BootAlloc.cpp` 与 IDF/lwIP/Bluedroid 内部堆）。
- **`w2-wifi-off` 裁剪验证**：`xtensa-esp32-elf-nm builds/w2-wifi-off/rtk_main.elf` 检出 `esp_wifi_init`、`httpd_start`、`_binary_index_html_gz_start` 均为 **0 命中**（`NONE_FOUND`）；`builds/w2-idf/rtk_main.elf` 中四符号均存在。

### 2.2 嵌入网页大小

- `builds/w2-idf/esp-idf/main/index.html.gz`：**15,979 B** / 24,576 B 上限（65.0%，构建期由 `tools/web_mock/pack.py` 强制校验 ≤ 24 KB）。

---

## 3. 契约偏离与 §文档待改

### 3.1 偏离说明

- **`gnss.const.*` 返回 `unsupported`**：按任务卡 §1.6 核查 `docs/reports/W0.md` §1.5，CM680 `MASK`/`UNMASK` 星座命令的板级应答仍为 `PENDING_HIL`，`components/gnss` 亦无单星座开关动词。因此：
  - `set` 下发 `gnss.const.gps/bds/glo/gal/qzss` 固定返回 `{"t":"ack","id":N,"ok":false,"err":"unsupported"}`；
  - `config` 帧省略 `gnss.const.*` 字段（符合 `docs/web/ws-protocol.md` §2 约定，前端自动隐藏 `#const-box`）。
- **未改动冻结文件**：未触碰 `components/*/include/**`、`sdkconfig.defaults`、`docs/frozen.sha256` 等任何冻结文件；`G1-frozen` PASS。

### 3.2 §文档待改（供 Architect 验收时统一更新）

1. **`docs/adr/ADR-0011-data-path-memory.md` §7（BootArena 台账表）**：补充 `kWifiConfig` 模式专属启动期分配项 `"web.frames"`（`12 × sizeof(WebFrame) = 12,384 B`，对齐 `alignof(WebFrame)`），蓝牙模式不分配；同时 `bind:web` 加入降级原因表。
2. **`docs/web/ws-protocol.md`（后续待 W0 HIL 完成后）**：待 CM680 `MASK`/`UNMASK` 在 P2 实测并合入 GNSS 星座动词后，再放开 `gnss.const.*` 的回显与设置。
3. **`docs/frozen.sha256`**：若 Architect 后续合入已未提交修改的 `docs/adr/ADR-0017-wifi-web-config.md`、`docs/tasks/W2-web-server.md`、`docs/web/ws-protocol.md`，按流程由 Architect 重算 `docs/frozen.sha256`（本分支当前 `sha256sum -c docs/frozen.sha256` 仍全量 PASS）。

---

## 4. 实际执行的命令与结果

| 项 | 命令 | 结果 | 备注 |
| --- | --- | --- | --- |
| 前端模拟自检 | `python3 tools/web_mock/web_mock.py --selftest` | **PASS** | `SELFTEST PASS: HTTP, WS handshake, §6 examples, all §5 error codes, bye, 1009` |
| 全量 CI 门禁 | `IDF_PATH=/home/gtc/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=/home/gtc/.espressif/python_env/idf6.1_py3.13_env IDF_COMPONENT_MANAGER=0 tools/ci/run_gates.sh --base main --prefix w2` | **PASS（36/36）** | 细分见下表 |
| G1 冻结校验 | `G1-frozen`（`sha256sum -c docs/frozen.sha256`） | **PASS** | 全部冻结文件无改动 |
| G2 / G2b Host + ASan/UBSan | 12 组 × 普通 + ASan/UBSan（共 24 项，含 `main_host_check` 76/76，覆盖 `web_server_check`、`radio_mode_check`、`main_{concurrent,preempt}_web_s2_storage_bt`） | **PASS（24/24）** | 0 失败、0 ASan/UBSan 报告 |
| G5 代码格式 | `G5-format`（`clang-format --style=file --dry-run --Werror`） | **PASS** | 全部改动文件符合 `.clang-format` |
| G6 Doxygen 文档 | `G6-doc_check`（`doc_check.py --base main`） | **PASS** | 改动函数 108 个，不合格 0 个 |
| G3 / G3b ESP-IDF 构建 | `w2-idf`、`w2-trace-off`、`w2-wifi-off`、`w2-production`、`w2-console-none`、`w2-rtk_pure-esp32`、`w2-rtk_pure-esp32c3` | **PASS（7/7）** | 全部 0 warning |
| G5b 静态分析 | `G5b-tidy`（`clang-tidy` 全库 139 个文件） | **PASS** | 139 文件 0 发现 |
| G7b 运行期堆与异常 | `G7b-runtime` | **PASS** | 128 目标文件，0 堆违规，0 `__cxa_guard`，6/6 白名单符号 |
| `w2-wifi-off` 符号裁剪 | `xtensa-esp32-elf-nm builds/w2-wifi-off/rtk_main.elf \| grep -E "esp_wifi_init\|httpd_start\|_binary_index_html_gz_start"` | **PASS** | `NONE_FOUND` |
| 空白字符检查 | `git diff --check` | **PASS** | `DIFF_CHECK_OK` |
| P2 台架 HIL | 烧录 / 串口 / 实机热点与浏览器验证 | **NOT_RUN** | 依 `AGENTS.md` §6 禁止执行者烧录或打开串口，交由 Architect 执行 |

---

## 5. Architect P2 台架 HIL 验收清单

1. 烧录 `builds/w2-idf/rtk_main.bin` 到 P2，串口 `radio wifi` 重启进入 Wi-Fi 配置模式；确认日志打印 `SoftAP P2-<SN> up at 192.168.10.12/24, ch=1, idle=300s, ui=15979B gz`。
2. 手机或电脑连接热点 `P2-<SN>`（WPA2-PSK 口令 `12345678`），确认 DHCP 获取 `192.168.10.x` 地址；打开 `http://192.168.10.12/`。
3. 核对页面收到 `hello` / `config` / 1 Hz `status` 刷新；在串口执行 `sys mem`，记录打开页面与 1 Hz 推送下的空闲堆（要求 ≥ 40 KB，预期约 75–80 KB）。
4. 在网页开启 **GGA 质量门控** 并修改 `min_sats`（如 6）→ 串口执行 `gnss status` 核对门控开关与最小卫星数一致 → 点击“退出”切回蓝牙模式重启后，再次 `gnss status` 确认门控配置已持久化仍在。
5. Wi-Fi 模式下增删蓝牙白名单并开启“下次启动打开配对窗口”（`bt.pair_next`），退出切回蓝牙模式：核对 `bt` 控制台显示白名单与网页一致、配对窗口已自动打开，且再次重启蓝牙模式后配对窗口不再重复自动打开。
6. 重新进入 Wi-Fi 模式后不连接任何终端（或断开最后一台终端），等待 300 s（可临时调小 `RTK_WIFI_IDLE_EXIT_S=60` 复测），确认自动推送 `bye{reason:"idle"}` 并重启回 `kBluetooth` 模式。
7. 在 `kBluetooth` 模式下做 10 min RTCM 下发负载测试，核对 `sys mem` 的 `min` 空闲堆 ≥ 40 KB。

---

## 6. Revision A（2026-11-20，HIL 阻断 + 交叉评审 P1–P3 全量修复）

依据 `docs/tasks/W2-web-server.md` §4 与 `docs/reports/W2-cross-review.md`，完成全部修复并以 `builds/w2a-*` 复跑全量门禁。

### 6.1 改动明细（逐条对应）

| 来源 | 项 | 实施内容 | 涉及文件 |
| --- | --- | --- | --- |
| 任务卡 §4.1（HIL 阻断） | IDF v6.1 WS 握手回调 | 在 `sdkconfig.defaults` 开启 `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`（Architect 授权，需重算 `docs/frozen.sha256`）；`/ws` 注册 `ws_post_handshake_cb = &WebModule::handleWsPostHandshake` 投递 `kConnect`，删除 `handleWs` 中永不执行的 `HTTP_GET` 分支并加编译期 `#error` 守卫 | `sdkconfig.defaults`、`main/modules/web/WebServer.cpp` |
| 任务卡 §4.3 | `/favicon.ico` 204 | 新增 `WebModule::handleFaviconGet` 返回 `204 No Content`，避免每次打开页面刷 404 告警 | `main/modules/web/WebServer.cpp`、`WebModule.hpp` |
| 交叉评审 P1 #1 | `httpd_stop` 静态上下文安全 | 注册具名空释放回调 `noopFreeGlobalCtx` 赋值给 `cfg.global_user_ctx_free_fn`（防止 IDF `httpd_main.c:617` 对静态 `WebModule` 调用 `free()`）；`httpd_stop()` 失败时保留 `httpd_` 句柄与网络资源并阻止 `unbindStorage()` | `main/modules/web/WebServer.cpp` |
| 交叉评审 P1 #2 | 会话代号与无损控制事件 | 为每次升级分配单调非 0 会话代号 `gen`，全链路（`httpdSessions_`、`WebCtrlEvent`、`WebFrame`、`ClientSlot`、`InflightRequest`、`onTxDrainWork`）核对 `(fd, gen)` 杜绝 fd 复用串会话；`ctrlMailbox_` 扩至 16 槽并在升级前强制检查 `freeSlots() >= active + 2`，数学保证 `kDisconnect` 永不丢失、满时拒绝新升级 | `main/modules/web/WebMailbox.hpp`、`WebModule.hpp`、`WebServer.cpp`、`WebModule.cpp` |
| 交叉评审 P1 #3 | GNSS 结果按 web seq 精确绑定 | `DataCenter` 用 `WebGnssOutcome{seq, phase, status, detail}` 替换原 `webGnssResult`，`GnssModule` 在受理时把 `pending_apply_` / 维护保存 / GGA 门控保存绑定到 `webApplySeq_` / `webMaintSeq_` / `webGgaSeq_`，控制台交错完成不再影响网页命令序号；并顺手修复 `GnssModule::initObjects()` 空 NVS 冷启动未置 `rate_loaded_=true` 导致首次改频率回写默认 1 Hz 的缺陷 | `main/data/AppData.hpp` / `.cpp`、`main/modules/gnss/GnssModule.hpp` / `.cpp` / `GnssModuleProbe.cpp` / `GnssModuleMaintenance.cpp` |
| 交叉评审 P1 #4 | GGA 门控持久化确认与退出门控 | `DataCenter` 新增 32 位无锁原子 `ggaGatePersist`（`kClean/kPending/kFailed`）与 `webGnssFlushGgaSeq`/`webGnssFlushGgaDoneSeq`；网页 `gnss.gga_gate.*` 等待 NVS 实际写盘后按 `seq` 回报成功/失败（`safeDefaults`/NVS 不可写立即报 `kInvalidState`）；`cmd exit` 若持久化非 `kClean` 则先触发立即落盘并等待确认，落盘失败回 `err:"failed"` 且保留在配置模式 | `main/data/AppData.hpp`、`main/modules/gnss/GnssModule.cpp` / `GnssModuleCmds.cpp`、`main/modules/web/WebModule.cpp` |
| 交叉评审 P2 #5 | 发送背压、队列重调与发送完成确认 | `kTxMailboxCap` 增至 8 槽（仅 `kWifiConfig` 下由 `BootArena` 分配，`web.frames` 16 × 1,036 B = 16,576 B，BT 模式仍 0 占用），预留 4 槽给必达帧（`status` 不挤占）；满时 `ack`/`config`/`bye` 保留在槽位 `pending*` 标志中重试不丢；`poll()` 自动重调失败的 `httpd_queue_work()`；`onTxDrainWork` 每帧后推进 `txCompletedSeq_`，重启等待 `httpd` 实际完成发送 + 100 ms 宽限（上限 500 ms） | `main/modules/web/WebModule.hpp`、`WebServer.cpp`、`WebModule.cpp` |
| 交叉评审 P2 #6 | GNSS 超时取消与背压 | `DataCenter` 新增 `webGnssCancelSeq`；`kLink` 超时回 `timeout` 后不释放槽位，转 `kWaitCleanup` 维持 `busy` 背压直到 `kIo` 确认 `kCancelled`（未执行）或 `kDone`（已开始的操作按同一 `seq` 回报最终结果并刷新 `config`） | `main/data/AppData.hpp`、`main/modules/gnss/GnssModule.cpp`、`main/modules/web/WebModule.cpp` |
| 交叉评审 P2 #7 | `bt.pair_next` 先清 NVS 再开窗 | `StorageModule::consumeBtPairNext()` 改为先确认 `store::writeValue(kBtPairNextKey, 0)` 返回 `ESP_OK`，再清 RAM 标志并返回 `true` 允许开窗；NVS 不可写或写失败时保留标志待重试且本次不开窗 | `main/modules/storage/StorageModule.cpp` |
| 交叉评审 P2 #8 | 站点一致快照与空闲回落复核 | 将 SoftAP 终端数与活动序号打包进单个 32 位原子 `staState_`（低 4 位计数 + 高 28 位序号，CAS 更新）；排定 `kIdle` 重启后在等待及最终提交前调用 `tryAbortIdleReboot()` / `canCommitIdleExit()` 复核，期间重连立即中止重启 | `main/modules/web/WebIdleTimer.hpp` / `.cpp`、`main/modules/web/WebServer.cpp`、`WebModule.cpp` |
| 交叉评审 P2 #9 | 初始 `config` 优先与拒收分片帧 | `pollInitialSync()` 同轮连续排入 `hello`+`config`，`pollRxFrame()` 在任意客户端尚处于 `kSendHello`/`kSendConfig` 或有 `pendingAck` 时延后消费 RX，确保 `config` 必先于请求 `ack`；`handleWs()` 明确拒绝 `!wsPkt.final`、`HTTPD_WS_TYPE_CONTINUE` 及非文本帧，回 RFC 6455 关闭码 `1003` 并返回 `ESP_FAIL` | `main/modules/web/WebServer.cpp`、`WebModule.cpp` |
| 交叉评审 P3 #10 | 实时路径改 `fw::trace`、检查关闭返回值与成员命名 | 在 `main/def/SystemDef.hpp` 新增 `WebTrace`（`TraceCat::kSys`）并在 `TraceText.cpp` 登记文本表；`WebModule::poll()` 与 `httpd` 回调全部改用 `fw::trace`，零格式化日志；检查 `httpd_sess_trigger_close()` 返回值；`WebModule` 与 `WebIdleTimer` 全部私有成员改为 `lowerCamelCase_` | `main/def/SystemDef.hpp`、`main/modules/debug/TraceText.cpp`、`main/modules/web/WebIdleTimer.hpp` / `.cpp`、`WebModule.hpp`、`WebServer.cpp`、`WebModule.cpp` |

### 6.2 新增主机回归测试

- **`tools/main_host_check/WebModuleCheck.cpp` + `tools/main_host_check/web_shim/`**（`web_module_check`，普通 + ASan/UBSan）：编译真实 `WebServer.cpp` + `WebModule.cpp`，按 IDF v6.1 行为模拟（握手只调 `ws_post_handshake_cb`，不调 `handler`），覆盖：
  - 未走 post-handshake 直接调 `handler` 拒收且不发 `hello`；走 post-handshake 正确发送 `hello`/`config`/`status`；
  - `GET /favicon.ico` 返回 `204 No Content`、`GET /` 返回 gzip 页；
  - `httpd_stop` 调用具名空释放回调（不调用 `free()`）及停机失败保句柄禁解绑；
  - 分片 `TEXT` / `CONTINUE` / `BINARY` 返回关闭码 `1003`，超长 1025 B 返回 `1009`；
  - 握手后立即发请求仍保证 `hello` → `config` 先于 `ack`；
  - fd 复用会话代号隔离、旧 TX 帧按代号丢弃、旧解锁不继承、控制队列槽位预留；
  - `httpd_queue_work` 失败自动重调、TX 8 槽满时第 9 条 `ack` 背压保留必达；
  - 空闲回落排定后站点重连中止重启；GGA 门控持久化失败阻止 `exit`、恢复 `kClean` 后等 `httpd` 排空 + 宽限再切回蓝牙。
- **`tools/main_host_check/GnssResetCheck.cpp`**（`testWebGnssBridgeAndGgaPersist`，纳入 `gnss_output` 普通 + ASan/UBSan）：覆盖控制台 `kSetRate` 与网页 `kSetRate` 交错不串单、超时 `webGnssCancelSeq` 跳过执行回 `kCancelled`、GGA 门控 NVS 写失败 / `safeDefaults` 回报失败与 `webGnssFlushGgaSeq` 立即落盘恢复。
- **`tools/main_host_check/MainHostCheck.cpp`**（`web_s2_storage_bt` 扩展 + `NvsShim.c` 写失败注入）：验证 `bt.pair_next` NVS 清零失败时保留标志且本次不开配对窗口，NVS 恢复后下一次启动清零并仅开窗一次。
- **`tools/main_host_check/WebServerCheck.cpp`**：扩展 `{fd, gen}` 透传、`freeSlots()`、一致快照打包与 `canCommitIdleExit()` 验证。

### 6.3 Revision A 资源与门禁结果（`builds/w2a-*`）

| 构建目录 | IRAM (B) / 余量 (B) | 静态 DRAM (B) / 余量 (B) | 链接镜像 (B) | `rtk_main.bin` (B) |
| --- | --- | --- | ---: | ---: |
| `builds/w2a-idf`（DEV，Wi-Fi 开） | 114,715 / 16,357 | 97,483 / 27,097（`.data` 51,099 / `.bss` 46,384） | 1,533,578 | 1,533,696 |
| `builds/w2a-trace-off` | — | — | — | 1,514,512 |
| `builds/w2a-wifi-off`（`RTK_WIFI_CONFIG_ENABLE=n`） | 86,287 / 44,785 | 75,247 / 49,333 | 911,506 | 911,616 |
| `builds/w2a-production` | — | — | — | 1,493,152 |
| `builds/w2a-console-none` | — | — | — | 1,444,832 |

- 嵌入网页 `builds/w2a-idf/esp-idf/main/index.html.gz`：**15,979 B** / 24,576 B。
- `xtensa-esp32-elf-nm builds/w2a-wifi-off/rtk_main.elf`：`NONE_FOUND`（未链入 Wi-Fi/httpd/页面）。
- `tools/ci/run_gates.sh --base main --prefix w2a`：**35/36 PASS，1 FAIL（仅 `G1-frozen`：`sdkconfig.defaults`）**：
  - `G1-frozen`：**EXPECTED FAIL** — 仅 `sdkconfig.defaults`（新增 `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`，任务卡 §4.1 授权，待 Architect 重算 `docs/frozen.sha256`）；其余全部冻结文件无改动。
  - `G2` / `G2b`（24 组 Host + ASan/UBSan，含 `main_host_check` **77/77**）：**PASS**。
  - `G5-format`：**PASS**；`G6-doc_check`（改动函数 130 个，不合格 0 个）：**PASS**；`G5b-tidy`（139 文件 0 发现）：**PASS**；`G3/G3b`（7 档 0 warning）：**PASS**；`G7b-runtime`（128 目标文件，0 堆违规，0 `__cxa_guard`，6/6 允许符号）：**PASS**。
- `python3 tools/web_mock/web_mock.py --selftest`：**PASS**；`git diff --check`：**PASS**。
- 冻结文件清单（供 Architect 重算 `docs/frozen.sha256`）：
  - `sdkconfig.defaults`（+1 行：`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`）。
- §文档待改补充：`docs/adr/ADR-0011-data-path-memory.md` §7 中 `web.frames` 增至 `16 × sizeof(WebFrame) = 16,576 B`（仅 `kWifiConfig` 模式分配，蓝牙模式仍为 0 B）。
