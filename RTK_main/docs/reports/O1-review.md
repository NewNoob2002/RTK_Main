# O1 评审报告：网页 OTA 设备端与应用回滚

- **评审对象**：分支 `feat/o1-ota-fw`（基线 `feat/ota-base`，工作树 `RTK_Main-o1` 全部未提交改动及新增文件）
- **对照依据**：`docs/tasks/O1-ota-firmware.md`、`docs/adr/ADR-0019-web-ota.md`（含 §2.3 字节级接口契约）、`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/reports/O1-ota-firmware.md`
- **评审性质**：只读代码审查 + 独立主机测试与 ESP-IDF 构建复核（未改动任何源码/文档，未提交、未烧录、未连串口）

---

## 1. 评审发现

### [F1] `major` — `web::JsonWriter`（1,048 B）与 `esp_app_desc_t`（256 B）直接放在 4 KB `httpd` 任务栈上

- **位置**：
  - `RTK_main/main/modules/web/WebOtaReply.hpp:76`（`renderSuccess`）
  - `RTK_main/main/modules/web/WebOtaReply.hpp:95`（`renderInfo`）
  - `RTK_main/main/modules/web/WebOta.cpp:304`（`renderInfo` 内 `esp_app_desc_t desc{}`）
  - `RTK_main/main/modules/web/WebOta.cpp:350, 364`（`handleOtaInfo` / `handleOtaPost`）
  - `RTK_main/main/modules/web/WebServer.cpp:744`（`HTTPD_DEFAULT_CONFIG()`，`stack_size = 4096`）
- **问题**：
  接收缓冲 2 KB 已正确从 `BootArena` 外供，但 `web_ota::renderSuccess()` 和 `web_ota::renderInfo()` 在栈上实例化了 `web::JsonWriter`（其内部含 `std::array<char, 1024> buffer_`）。从 `builds/r1-idf/rtk_main.elf` 反汇编与 `builds/o1-logs/WebOta-stack.su` 实测 Xtensa 栈帧如下：
  - `httpd` 到达 URI 回调前已占用约 ~496 B（`httpd_thread` 144 B + `httpd_sess_process` 32 B + `httpd_req_new` 176 B + `httpd_uri` 80 B + 会话遍历约 64 B）。
  - `GET /ota/info` 路径：`handleOtaInfo`（64 B）→ `renderInfo(part)`（400 B，栈上 `esp_app_desc_t desc{}` 在调用 `web_ota::renderInfo` 期间持续存活）→ `web_ota::renderInfo`（`entry a1, 0x490` = **1,168 B**），仅回调内部合计 **1,632 B**；叠加 `httpd` 调用链、`JsonWriter` 叶函数与 FreeRTOS/Xtensa 中断保存帧后，在 4,096 B 栈上已消耗 >2.4 KB（>60%）。
  - `POST /ota` 成功路径：`handleOtaPost`（`entry a1, 0x1c0` = **448 B**）→ `web_ota::renderSuccess`（`entry a1, 0x460` = **1,120 B**），仅回调内部合计 **1,568 B**。
  - 调用方本已传入位于 `BootArena` 的 1,024 B 缓冲 `self.httpdTxFrame_->data`，而两条应答 JSON 仅约 ~70 B 与 ~150 B，在 4 KB `httpd` 栈上再开一份 1 KB 暂存区与 ADR-0019 §2.4 避免大对象压入 `httpd` 栈的约束相背。
- **失败场景**：
  `sdkconfig.defaults` 开启了 `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y`。在 Wi-Fi/lwIP 中断叠加、SPI Flash 读取或后续维护略增调用深度时，4 KB `httpd` 栈余量不足可能触发栈溢出 canary 复位，尤其在 `GET /ota/info` 或 `POST /ota` 成功回执的瞬间导致设备异常重启。
- **修复建议**：
  1. 在 `WebOtaReply.hpp` 中移除对栈上 `web::JsonWriter` 的构造，改为直接向调用方提供的 `std::span<char> out` 线性追加字段并复用轻量 JSON 字符串转义辅助函数（支持直接写出 `"other":null`，同时顺带消除先写 `""` 再 `memmove` 替换的逻辑）。
  2. 在 `WebOta.cpp::renderInfo` 中将 `esp_app_desc_t desc{}` 限制在只复制 32 B `version` 数组的内部辅助函数/作用域内，避免 256 B 结构体与 JSON 编码帧叠加。改造后两条路径峰值栈占用均可下降 >1.2 KB。

---

### [F2] `minor` — `isBinaryRequest` 对 `Content-Type` 的长度边界存在误拒与误放行

- **位置**：`RTK_main/main/modules/web/WebOta.cpp:116-121`
- **问题**：
  `isBinaryRequest` 使用定长 `std::array<char, kBinaryType.size() + 1U> field{}`（25 B）调用 `httpd_req_get_hdr_value_str(req, "Content-Type", field.data(), field.size())` 并用 `strncasecmp(..., 24)` 判断：
  1. **误拒合法头**：RFC 9110 允许媒体类型带参数或尾随空白（如 `application/octet-stream; charset=binary` 或末尾空格）。一旦长度 > 25，IDF 返回 `ESP_ERR_HTTPD_RESULT_TRUNC`，导致合法请求被误拒为 `400 bad_image`。
  2. **误放行非法头**：ESP-IDF v6.1 `httpd_parse.c:1130-1135` 中 `size_t full_size = strlcpy(val, val_ptr, val_size); if (val_size < full_size) return ESP_ERR_HTTPD_RESULT_TRUNC;` 存在 off-by-one（`strlcpy` 返回不含 NUL 的源长度）。当头值恰好为 25 字节（如 `Content-Type: application/octet-streamX`）时，`val_size < full_size`（`25 < 25`）为假，IDF 返回 `ESP_OK` 并静默截断第 25 字节，随后 `strncasecmp(..., 24)` 返回 0 而误判通过。
- **失败场景**：
  某些 HTTP 客户端或代理在 `Content-Type` 后附带 `; charset=...` 或尾随空白时，正常固件上传被错误拒绝为 `400 bad_image`；构造 25 字节后缀的非法媒体类型会绕过检查。
- **修复建议**：
  复用调用方的 `out` 缓冲（或至少 64 B 栈缓冲）先查 `httpd_req_get_hdr_value_len`，读取完整头值后截取 `;` 之前的媒体类型并裁剪首尾空白，再与 `application/octet-stream` 做精确长度 + 大小写不敏感比较。

---

### [F3] `minor` — 关机 `kQuiesce` 时 `sendUpload()` 阻塞可能叠加 `recv_wait_timeout` 超出 5,500 ms 关机等待上限

- **位置**：
  - `RTK_main/main/modules/web/WebOta.cpp:381-397`
  - `RTK_main/main/modules/web/WebServer.cpp:744, 1119-1128`
  - `RTK_main/main/modules/power/PowerModule.cpp:33, 413-416`
- **问题**：
  `PowerModule` 为 Wi-Fi 关机静默预留 `kWifiQuiesceWaitMs = 5500U`（注释：覆盖 HTTP socket 默认 5 s 超时）。但 `HTTPD_DEFAULT_CONFIG()` 同时设有 `recv_wait_timeout = 5` 与 `send_wait_timeout = 5`。当关机触发于慢速/卡顿的上传期间：
  1. `httpd_req_recv()` 最坏阻塞 5 s 后退出循环；
  2. `io.abort()` 执行 `esp_ota_abort()`（此处已释放 Flash 句柄，不至于在断电时仍在写 Flash）；
  3. 随后 `sendUpload()` 调用 `httpd_resp_send()` 回 503，若对端已卡死或 TCP 发送窗满，可能再阻塞最多 5 s；
  4. `ota::endOperation()` 与 `onPowerModeWork`（切换 `WIFI_MODE_NULL`）都须等待 `sendUpload()` 返回，总耗时最坏可达 ~10 s > 5,500 ms。
- **失败场景**：
  上传终端网络卡顿且此时长按电源键关机，`PowerModule` 在 5.5 s 报 `power SoftAP quiesce timed out`，在 `esp_wifi_set_mode(WIFI_MODE_NULL)` 完成前直接进入 `kCutPower`。
- **修复建议**：
  对 `kAborted` 分支，若已观测到 SoftAP 站点归零（`hadStation && staCount == 0`）可直接跳过 `sendUpload()` 避免无意义发送阻塞；若仍有站点，保持回发 503 以满足 ADR-0019 §2.4，但可将 `ota::endOperation()` 或发送超时（或关机日志说明）对齐 `kWifiQuiesceWaitMs` 预算。

---

### [F4] `minor` — `ota::confirm()` 失败时仍以 `FW_LOGI` 级别记录

- **位置**：`RTK_main/main/modules/ota/OtaRuntime.cpp:115-127`
- **问题**：
  `confirm()` 无论 `esp_ota_mark_app_valid_cancel_rollback()` 成功还是失败都打印 `FW_LOGI(fw::TraceCat::kSys, "<ota> confirm running image: %s", esp_err_to_name(err))`。同时 `pollConfirmation()`（line 116-118）在 `confirm()` 失败后直接将 `s_confirmation_armed` 置 `false` 不再重试，却未以 `W`/`E` 级别告警，不符合 `docs/CODING-STYLE.md §5.1` 的日志分级规则。
- **失败场景**：
  若 `otadata` 写入失败导致新镜像未能确认，设备下次重启将意外回滚；默认运维日志按 `E`/`W` 检索时容易漏掉此次失败。
- **修复建议**：
  `err == ESP_OK` 时打 `FW_LOGI`，失败时打 `FW_LOGE`（或 `FW_LOGW`）。

---

### [F5] `minor` — `CONFIG_RTK_OTA_ENABLE` 条件编译风格不一致，且 `main_host_check` 未覆盖 OTA 联动路径

- **位置**：
  - `RTK_main/main/app/BootBuffers.hpp:42`、`RTK_main/main/app/BootBuffers.cpp:22, 164, 211`
  - `RTK_main/main/modules/debug/TraceText.cpp:165, 191, 360`
  - `RTK_main/main/modules/web/WebModule.hpp:71, 606`
  - `RTK_main/main/modules/web/WebModule.cpp:6, 416, 438, 461, 702, 772, 814`
  - `RTK_main/main/modules/web/WebServer.cpp:7, 60, 788, 1004, 1077, 1121`
  - `RTK_main/tools/main_host_check/BootAllocCheck.cpp`、`RTK_main/tools/main_host_check/WebModuleCheck.cpp`
- **问题**：
  1. 仓库同文件的 `CONFIG_RTK_WIFI_CONFIG_ENABLE` 与 `CONFIG_RTK_WIFI_CAPTIVE_PORTAL` 均采用 `#if defined(CONFIG_X) && CONFIG_X`，新增的 19 处 `CONFIG_RTK_OTA_ENABLE` 仅写 `#if defined(CONFIG_RTK_OTA_ENABLE)`。在 ESP-IDF `sdkconfig.h` 下关闭项未定义故当前有效，但主机构建若传入 `-DCONFIG_RTK_OTA_ENABLE=0` 会误判定为开启。
  2. `tools/main_host_check` 未在 `BootAllocCheck` / `WebModuleCheck` 中打开 `CONFIG_RTK_OTA_ENABLE=1`，导致 `web.ota` 启动池分配/回滚、以及 `WebModule` 的 `pollIdleExit`、WS `busy`、`RebootKind::kOta`、`powerPauseDone` 联动未在主机回归中执行验证。
- **失败场景**：
  后续主机测试或跨配置构建显式传 `-DCONFIG_RTK_OTA_ENABLE=0` 时会误编译 OTA 分支；重构 `WebModule`/`BootBuffers` 时缺少对 OTA 集成钩子的主机回归保护。
- **修复建议**：
  将上述 19 处统一为 `#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE`；在 `BootAllocCheck` 与 `WebModuleCheck` 中补充最小 OTA 缓冲分配与状态联动断言。

---

### [F6] `nit` — `WebModule::resetRuntimeState()` 未复位新增原子标志

- **位置**：`RTK_main/main/modules/web/WebServer.cpp:1010-1035`
- **问题**：
  `resetRuntimeState()` 复位了所有既有原子通信标志（`staState_`、`txWorkQueued_`、`powerPauseRequested_` 等），唯独未复位 `otaIdleReset_` 与 `otaRebootRequested_`。
- **失败场景**：
  固件正常生命周期中 `WebModule::init()` 仅执行一次且静态初值为 `false`，无现网故障；仅在未来重试 `init()` 或主机单测复跑同一实例时可能残留上次状态。
- **修复建议**：
  在 `resetRuntimeState()` 中按 `#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE` 将两者 `store(false, std::memory_order_relaxed)`。

---

### [F7] `nit` — O1 报告栈帧统计遗漏最深路径 `handleOtaInfo -> renderInfo`

- **位置**：`RTK_main/docs/reports/O1-ota-firmware.md:38`
- **问题**：
  报告第 38 行列出了 `handleOtaPost`（448 B）、`renderSuccess`（1,120 B）和 `receiveBlock`（48 B），遗漏了同文件 `WebOta-stack.su` 中更深的 `handleOtaInfo`（64 B）→ `renderInfo(part)`（400 B）→ `web_ota::renderInfo`（1,168 B），合计 1,632 B。
- **失败场景**：
  评审或 HIL 排查栈水位时容易低估 `GET /ota/info` 的栈占用。
- **修复建议**：
  修复 [F1] 后重测 `-fstack-usage` 并将 `handleOtaInfo` 与 `handleOtaPost` 两条路径的更新后栈帧一并写入 O1 报告。

---

## 2. 逐项核对清单

| 检查项 | 结论 | 证据 / 说明 |
|---|---|---|
| HTTP 路由与 ADR-0019 §2.3 字节级契约 | 基本符合（见 F2） | `GET /ota/info`、`POST /ota` 路由、状态码、9 个 `err` 码、JSON 字段名与顺序、成功体 `{"ok":true,"ver":...,"reboot_ms":1500}`、`other:null` 均与 §2.3 逐字节一致；`max_uri_handlers` 增至 14，优先于门户 302 兜底。仅 `Content-Type` 检查有 F2 边界问题。 |
| 头部校验在 `esp_ota_begin` 前完成 | 通过 | `WebOtaTransfer.hpp:148-157` 先收齐 288 B（`WebOta.cpp:36-49` 用 `static_assert` 校对 IDF 结构体偏移与魔数），校验 `0xE9`、`ESP_CHIP_ID_ESP32`、`0xABCD5432`、`project_name` 后才调用 `esp_ota_begin(..., OTA_WITH_SEQUENTIAL_WRITES, ...)`。 |
| 所有失败路径 `esp_ota_abort` 与截断保护 | 通过 | `WebOta.cpp:381-383` 统一调用 `io.abort()`（`handle_ == 0` 或已被 `esp_ota_end` 释放返回 `ESP_ERR_NOT_FOUND` 并正确忽略；已核对 IDF v6.1 `esp_ota_ops.c:149, 486, 660`）；`OtaUploadIo::end()` 额外以实际长度调用 `esp_image_get_metadata()` 防止目标槽旧尾段补全截断镜像。 |
| 30 s 累计超时 / 断开 `aborted` | 通过 | `WebOtaTransfer.hpp:113-132` 使用无符号差值 `io.nowMs() - progressMs >= 30000U`（回绕安全，有数据即重置），TCP EOF/错误及 SoftAP 站点归零均返回 `kAborted`（503），超时返回 `kTimeout`（408）。 |
| 缓冲 ≤ 2 KB 来自启动池、蓝牙不申请、`.bss` ≤ 256 B | 部分通过（见 F1） | `web.ota` 为 2,048 B，仅 `kNormal && kWifiConfig` 从 `BootArena` 申请并在失败回滚时解绑；蓝牙/充电待机 0 B；`.bss` +16 B、静态 DRAM 净减 992 B（复用 kLink Web 工作帧做 DNS）。但 `httpd` 栈上存在 F1（1,048 B `JsonWriter`）。 |
| 单上传原子标志与 `WebIdleTimer`/WS `busy`/关机中止联动 | 基本通过（见 F3、F5、F6） | `s_operation` 原子无锁仲裁 `kUpload`/`kConfirm`/`kRollback`；`PowerRequests::claimRestart()` 在 `setBoot()` 与关机/`exit`/`freset`/重启之间形成严格互斥；上传中暂停 `WebIdleTimer`、失败后经 `otaIdleReset_` 重头计时、已排定 `kIdle` 会被 `tryAbortIdleReboot` 中止；WS `set`/`cmd` 回 `busy`；B2 关机并入 `powerPauseDone()`。仅 F3 有极端卡顿超时风险。 |
| `RebootKind::kOta` 1500 ms 重启不写射频记录 | 通过 | `WebModule.cpp:388-390, 462-470, 814-820` 经 `RebootKind::kOta` 延时 1,500 ms 直接调用 `esp_restart()`，不调用 `requestRadioMode()`，重启后默认进蓝牙模式。 |
| 回滚判定纯函数、时钟回绕、启动失败判无效、充电待机确认 | 通过（见 F4） | `OtaPolicy.hpp` 无 IDF 依赖、纯 `constexpr`、无符号差值抗回绕；`BootFlow.cpp:440` 在 `handleDegrade()` 首行调用 `ota::bootFailed()`，对 `pending_verify` 立即 `esp_ota_mark_app_invalid_rollback_and_reboot()`；`BootFlow.cpp:577` 发布 `bootReady()`，`SystemModule`（`kAlways`，含充电待机）在 `kLink` 轮询 `pollConfirmation()`。 |
| `PROJECT_VER` 单一来源 | 通过 | 根 `CMakeLists.txt:3` 设 `PROJECT_VER "6.1.0-dev"`，`kAppVersion` 已全部替换为 `appVersion()`；实测 `.bin` 描述符为 `6.1.0-dev` / `rtk_main`，启动横幅、`sys info`、蓝牙版本、WS `hello.fw`、`/ota/info`、`ota status` 统一来源。 |
| `RTK_OTA_ENABLE=n` 裁剪与控制台分档 | 通过 | `r1-ota-off` ELF 确认无 `esp_ota_begin`/`write`/`end`/`handleOtaPost`/`handleOtaInfo`/`registerOtaHandlers`，回滚确认保留；`o1-production` / `o1-console-none` 无 `otaCmds`；DEV 下 `ota status\|valid\|rollback` 符合 §2.7。 |
| O1 报告 `docs/reports/O1-ota-firmware.md` 真实性 | 基本属实（见 F7） | DRAM（96,395 B）、IRAM（115,435 B / 余 15,637 B）、`rtk_main.bin`（1,505,984 B）、G7b PASS、G1 仅 `CMakeLists.txt` 差异、符号裁剪、HIL `NOT_RUN` 均经独立复核属实；仅第 38 行栈帧列举遗漏 `renderInfo` 路径。 |

---

## 3. 独立复核记录（`builds/r1-*`）

| 复核项 | 结果 | 说明 |
|---|---|---|
| `test/ota`（普通 + ASan/UBSan） | PASS | `builds/r1-ota`、`builds/r1-ota-asan` 全通过（沙箱下 LSan 需 `ASAN_OPTIONS=detect_leaks=0`，ASan/UBSan 正常运行） |
| `tools/main_host_check` | PASS | `builds/r1-main_host_check` 103/103 用例全通过 |
| `tools/doc_check/doc_check.py --base feat/ota-base` | PASS | 改动函数 76 个，不合格 0 个 |
| `clang-format --style=file --dry-run --Werror`（全部改动/新增 C++ 文件） | PASS | 0 差异 |
| `sha256sum -c docs/frozen.sha256` | FAIL（预期） | 仅 `CMakeLists.txt` 不匹配（任务卡 §1.6 要求新增 `PROJECT_VER`，待 Architect 重算冻结清单） |
| `idf.py -B builds/r1-idf build` | PASS | 0 warning；DRAM 96,395 B，IRAM 115,435 B（余 15,637 B），`rtk_main.bin` 1,505,984 B，描述符 `6.1.0-dev`/`rtk_main` |
| `idf.py -B builds/r1-ota-off build`（`CONFIG_RTK_OTA_ENABLE=n`） | PASS | 0 warning；ELF 无 OTA 写入与 HTTP 路由符号，保留回滚确认符号 |
| `tools/g7b_check/g7b_check.py --build builds/r1-idf` | PASS | 自有目标文件 138 个，0 个直接堆符号，0 个 `__cxa_guard`，6/6 异常白名单符号 |

---

## 4. 总结论

**CHANGES_REQUIRED**

O1 整体架构清晰，ADR-0019 §2.3 契约、头部前置校验、顺序写入与截断防护、并发/关机互斥及回滚确认逻辑均扎实且经主机测试与 IDF 构建验证；但存在 1 项 `major`（`WebOtaReply.hpp` 在 4 KB `httpd` 栈上构造 1 KB `web::JsonWriter`，叠加 `esp_app_desc_t` 导致 `handleOtaInfo` 回调栈帧达 1,632 B）以及若干易修复的 `minor`（`isBinaryRequest` 媒体类型截断边界、关机中止发送超时处理、`confirm()` 失败日志级别、`CONFIG_RTK_OTA_ENABLE` 宏风格与主机覆盖）。请按上述建议修改并更新栈帧测量后复审。

---

## 复审

- **复审范围**：`git diff feat/ota-base` 全量代码、测试与主机替身，对照 `docs/reports/O1-ota-firmware.md` “## 修复轮”，并独立运行主机测试（`builds/rr1-*`）、格式/注释门禁及 `builds/o1-idf` / `builds/o1-ota-off` ELF 核验。
- **复审结果概览**：F1–F7 全部已修复；未发现阻塞性新缺陷，记录 2 条非阻塞观察项（`nit`）。

### 1. F1–F7 逐条核实

| 编号 | 状态 | 核实依据（代码 / 测试 / ELF） |
|---|---|---|
| F1 | **已修复** | `WebOtaReply.hpp:75-204` 移除栈上 `web::JsonWriter`，改为直接向调用方 `std::span<char> out`（即 `BootArena` 的 1,024 B `httpdTxFrame_->data`）线性编码并手写 `appendNumber`（避开 Xtensa `std::to_chars` 256 B 栈帧），直接写出 `"other":null`。`WebOta.cpp:385-403, 407-421` 在 2,048 B `otaBuffer_`（`std::span<std::byte>`）内 placement-new 构造/析构 `esp_app_desc_t`，`bindOtaStorage` 静态断言容量并在运行期校验精确长度与 `alignof(esp_app_desc_t)` 对齐。**并发与容量核对**：`httpdTxFrame_`（`onTxDrainWork` / `handleOtaInfo` / `handleOtaPost`）与 `otaBuffer_`（`handleOtaInfo` / `handleOtaPost`）均严格在单线程 ESP-IDF `httpd` 任务同步串行执行（未启用 async req handler），且单个回调内各阶段不重叠；1,024 B / 2,048 B 容量均留足余量并在越界时安全失败。**ELF 独立复核**（`builds/o1-idf/rtk_main.elf`）：`handleOtaInfo` 128 B → `web_ota::renderInfo` 80 B → `appendString`/`appendText` 48 B（自有合计 **256 B**，较首轮 1,632 B 下降 1,376 B）；`handleOtaPost` 512 B → `receiveBlock`/`appendString` 48 B（自有合计 **560 B**，较首轮 1,568 B 下降 1,008 B）；`.bss` 增量 0 B。 |
| F2 | **已修复** | `WebOta.cpp:86-153` 中 `declaredLength` 与 `isBinaryRequest` 均先调用 `httpd_req_get_hdr_value_len`，校验 `len > 0 && len < field.size()` 后再以 `len + 1U` 调用 `httpd_req_get_hdr_value_str`，彻底规避 ESP-IDF v6.1 `httpd_parse.c:1133` 的 `val_size < full_size` 截断边界。`isBinaryRequest` 复用 2,048 B `otaBuffer_`，截取 `;` 前媒体类型、裁剪 SP/HTAB 并执行精确长度的 ASCII 不区分大小写比对。`WebModuleCheck.cpp:697-712, 1806-1820` 复现了 IDF v6.1 截断边界，验证 25 B `"application/octet-streamX"` 在接收前拒为 `400 bad_image`，带参数/大小写/尾随空白的合法头可正常进入接收。 |
| F3 | **已修复** | `WebOta.cpp:39, 307-323` 在进入 `transfer()` 前经 `lwip_getsockopt` 保存 `SO_RCVTIMEO` 并临时设为 `1 s`（30 s 累计无进展超时仍由 `receiveBlock` 判定），仅在 `kOk` 时恢复原 5 s 超时（失败一律返回非 `ESP_OK`，由 `httpd_sess_delete` 直接关闭 socket，无残留风险；且 `kOk` 前已 `claimRestart()` 与关机互斥）。`WebOta.cpp:335-354, 485-487` 对站点归零的 `kAborted` 直接跳过发包并返回 `ESP_FAIL`；对其余全部错误响应使用 `sendUploadError()` 组装含 `Connection: close` 的完整 HTTP/1.1 报文并经 `lwip_send(..., MSG_DONTWAIT)` 单次非阻塞发送，返回 `ESP_FAIL`/`ESP_ERR_HTTPD_RESP_SEND` 跳过 `httpd_req_delete` 剩余正文清空并立即关会话。`WebModuleCheck.cpp:1865-1892` 已验证背压关机（1,000 ms 退出、`abort` 释放仲裁、随后 `powerPauseDone()` 成立）与站点归零直接关闭。 |
| F4 | **已修复** | `OtaRuntime.cpp:122-131` 的 `confirm()` 改为 `err == ESP_OK` 打印 `FW_LOGI`，失败打印 `FW_LOGE` 并携带 `esp_err_to_name(err)`，符合 `CODING-STYLE.md §5.1`。 |
| F5 | **已修复** | 全仓检索 `CONFIG_RTK_OTA_ENABLE`，所有 C/C++ 条件编译已统一为 `#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE`（0 处遗留裸 `defined()`）。`tools/main_host_check/CMakeLists.txt:288-299, 380-446` 将真实 `WebOta.cpp`/`OtaRuntime.cpp` 接入 `boot_alloc_check_on` 与 `web_module_check`（OTA=1），同时在 `boot_alloc_check_{gnss_off,trace_off,all_off}` 与 `web_module_portal_off_check` 显式验证 `CONFIG_RTK_OTA_ENABLE=0`。**替身真实性核对**：`web_shim/{esp_ota_ops.h,esp_image_format.h,esp_http_server.h,lwip/sockets.h}` 的宏值、枚举、结构偏移均与 ESP-IDF v6.1 源码一致（并经 `WebOta.cpp` 全部 `static_assert` 校对）；`httpd_req_get_hdr_value_str` 精确还原 IDF v6.1 off-by-one；`lwip_{get,set}sockopt`/`lwip_send` 核对 `SO_RCVTIMEO`/`sizeof(timeval)`/`MSG_DONTWAIT`；`BootAllocCheck.cpp` 与 `WebModuleCheck.cpp` 覆盖启动池分配/OOM/绑定失败/待机裁剪、WS `busy`、空闲暂停与重启、`RebootKind::kOta` 1,500 ms 精确重启。 |
| F6 | **已修复** | `WebServer.cpp:1022-1025` 的 `resetRuntimeState()` 已将 `otaIdleReset_` 与 `otaRebootRequested_` 复位为 `false`。`WebModuleCheck.cpp:1893-1903, 1931-1944` 通过残留标志后重启同一 `WebModule` 实例完成回归验证。 |
| F7 | **已修复** | `O1-ota-firmware.md:38, 147-170` 已补录首轮 GET 完整调用链（1,632 B）及修复后 GET（256 B）/POST（560 B）栈帧与更新后的内存/体积数据，与 `WebOta-repair-stack.su` 及最终 `builds/o1-idf/rtk_main.elf`（`rtk_main.bin` 1,508,368 B、DRAM 96,395 B、IRAM 115,435 B / 余 15,637 B）一致。 |

### 2. 新发现（非阻塞）

- **[N1] `nit` — `BootBuffers.cpp` 与 `WebOta.cpp` 的对齐常量隐式耦合**
  - **位置**：`RTK_main/main/app/BootBuffers.cpp:80, 165-166`、`RTK_main/main/modules/web/WebOta.cpp:416`
  - **失败场景**：当前 `BootBuffers.cpp` 使用 `kBufAlign = 4U` 分配 `web.ota`，而 `WebOta.cpp:416` 按 `alignof(esp_app_desc_t)` 校验；目前两者在 ESP32 与主机上均为 4 B，无实际故障。若未来平台或描述符对齐大于 4 B，启动绑定阶段会报 `kBindWebOta`。
  - **建议**：保持现状亦可，或在 `WebOta.cpp` 旁增加 `static_assert(alignof(esp_app_desc_t) <= 4U)`（或由 `WebOtaTransfer.hpp` 导出 `kBufferAlign`）使契约显式化。
- **[N2] `nit` — `WebModuleCheck` 替身未覆盖 `lwip_send` 部分写与 `esp_ota_end` 句柄消费**
  - **位置**：`RTK_main/tools/main_host_check/WebModuleCheck.cpp:628-639, 848-857`
  - **失败场景**：`lwip_send` 替身仅模拟全量写入或 `-1/EWOULDBLOCK`，未覆盖 `0 < written < len` 部分写分支；`esp_ota_end` 替身未将模拟句柄标为已消费（`WebOta.cpp` 生产代码已通过 `== len` 与允许 `ESP_ERR_NOT_FOUND` 正确处理，无现网缺陷）。
  - **建议**：非阻塞；后续维护若扩展 OTA 主机用例可顺手补齐这两项状态模拟。另 `sendUploadError()` 对普通错误也采用单次 `MSG_DONTWAIT`，在极端发送背压下会直接关闭连接而不等待重发（报告 §修复轮第 145 行已如实披露），以及实机 `DISCONNECTED` 事件派发叠加关机总耗时，均交由 Architect 台架 HIL 最终确认。

### 3. 复审独立复核记录（`builds/rr1-*`）

| 复核项 | 结果 | 说明 |
|---|---|---|
| `test/ota`（普通 + ASan/UBSan） | PASS | `builds/rr1-ota` 1/1、`builds/rr1-ota-asan` 1/1（`ASAN_OPTIONS=detect_leaks=0`） |
| `tools/main_host_check`（普通 + ASan/UBSan） | PASS | `builds/rr1-main_host_check` 103/103、`builds/rr1-main_host_check-asan` 103/103（含 `boot_alloc_check_{on,gnss_off,trace_off,all_off}`、`web_module_check`、`web_module_portal_off_check`） |
| `tools/doc_check/doc_check.py --base feat/ota-base` | PASS | 改动函数 83 个，不合格 0 个 |
| `clang-format --style=file --dry-run --Werror` | PASS | 全部改动/新增 C++ 与 shim 文件 0 差异 |
| `builds/o1-idf` 产物体积、栈帧与 G7b | PASS | 产物时间戳晚于全部源码修改；`rtk_main.bin` 1,508,368 B，DRAM 96,395 B（`.bss` 44,720 B），IRAM 115,435 B（余 15,637 B）；反汇编核验 `handleOtaInfo` 128 B、`renderInfo` 80 B、`handleOtaPost` 512 B；`g7b_check.py` PASS（138 个自有目标文件，0 个直接堆符号） |
| `builds/o1-ota-off` 符号裁剪 | PASS | 无 `handleOtaInfo`/`handleOtaPost`/`registerOtaHandlers`/`esp_ota_begin`/`write`/`end`，保留 `bootFailed`/`pollConfirmation` |

### 4. 复审结论

**ACCEPT**

F1–F7 均已在源码、主机回归与目标 ELF 层面完整闭合，未引入新的 `blocker` / `major` / `minor` 缺陷。G1 冻结清单中的 `CMakeLists.txt` 散列更新及实机 HIL 台架验收（含 `httpd` 任务实际栈水位、最低空闲堆、慢速上传关机静默时延与回滚）按既定流程交由 Architect 处理。
