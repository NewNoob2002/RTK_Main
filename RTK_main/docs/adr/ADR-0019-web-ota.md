# ADR-0019：网页固件升级（OTA）与回滚

状态：**已批准**（2026-11-23，用户答复 §7 Q1–Q2）。由 O 系列任务卡实施（见 §6）。
关联：ADR-0002 §9（OTA 延后，本 ADR 解除）、ADR-0014 §2.1（分区与 3 MB app 槽）、ADR-0017（Wi-Fi 网页配置）、
ADR-0015（P2 电源管理与关机序列）、ADR-0011（启动内存池）。

> 命名：O = OTA。

## 1 背景

- 旧 P2（`P2_V3.1_IDF/main/user_src/user_web_from.c:144` `cgi_bin_update_handler`）：`POST /upload` 接收 multipart，
  在 httpd 回调里 `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)` → `esp_ota_write` → `esp_ota_end` → `esp_ota_set_boot_partition`，
  按文件名比较版本号，升级期间停 4G 任务，OLED 显示进度；启动时无条件 `esp_ota_mark_app_valid_cancel_rollback()`（回滚未开，等于无回滚）。
- 本工程现状：`partitions.csv` 已有 `otadata`、`ota_0`、`ota_1`（各 0x300000）；Wi-Fi 配置模式（ADR-0017）已有 SoftAP + httpd
  （core 0，3 个套接字）+ WS；手机 0x1D 是固定回失败的占位（`components/link_core/services/PhoneCompatService.cpp:437`）。
- 固件为多产品单镜像（启动时按 GPIO35 识别板型），因此镜像不需要按板型区分。

## 2 决定

### 2.1 范围

- 只做 **Wi-Fi 配置模式下的网页上传**（`kWifiConfig`）。蓝牙模式、手机 0x1D、4G 远程升级、GNSS 接收机固件升级不做（§5）。
- 开启 IDF **应用回滚**；不开防回滚（anti-rollback eFuse）、不开签名、不开 flash 加密、不开安全启动。

### 2.2 安全门槛（Q1）

- 访问门槛 = 热点 WPA2 口令（与旧 P2 一致），**不另设口令**。
- 设备端镜像校验（全部满足才写 boot 分区）：
  1. 收到前 `sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)` 字节后立即检查：
     镜像头 magic `0xE9`、`chip_id == ESP_CHIP_ID_ESP32`、`esp_app_desc_t.magic_word == ESP_APP_DESC_MAGIC_WORD`、
     `project_name` 等于当前运行镜像的 `project_name`（`rtk_main`）；不满足立即中止（不擦写后续扇区）。
  2. `Content-Length` 必须给出且 `> 上面的头长`、`<= 目标分区大小`。
  3. `esp_ota_end()` 的完整校验（段校验和 + 附加 SHA-256）通过。
- 不比较版本号（Q2）：允许降级。页面在上传前于浏览器端解析所选文件的 `esp_app_desc_t`，显示“当前版本 → 新版本”，用户确认后上传。

### 2.3 HTTP 接口（O1 实现，O2 页面与 mock 按此对接，字节级契约）

| 方法 / 路径 | 请求 | 应答 |
|---|---|---|
| `GET /ota/info` | — | `200`，JSON：`{"running":"<ver>","project":"rtk_main","part":"ota_0","state":"valid","other":"<ver>"\|null,"max":3145728,"busy":false}` |
| `POST /ota` | 体为原始 `.bin`（`Content-Type: application/octet-stream`，必须有 `Content-Length`；**不用 multipart**） | 见下表 |

- `state` 取 `esp_ota_img_states_t` 的小写名去前缀：`new`/`pending_verify`/`valid`/`invalid`/`aborted`/`undefined`；`other` 为另一个 app 槽
  `esp_ota_get_partition_description` 的 `version`，读不到为 `null`；`max` 为下一个更新分区的字节数。
- `POST /ota` 应答一律 `Content-Type: application/json`，体为 `{"ok":true,"ver":"<新版本>","reboot_ms":1500}` 或 `{"ok":false,"err":"<码>"}`：

| HTTP | `err` | 含义 |
|---|---|---|
| 200 | —（`ok:true`） | 已写入并设为启动分区，`reboot_ms` 后重启 |
| 409 | `busy` | 已有上传进行中，或电源管理正在关机 |
| 409 | `state` | 当前运行镜像仍是 `pending_verify`（IDF 规定此时不能开始新的 OTA） |
| 411 | `length` | 缺 `Content-Length` 或为 0 |
| 413 | `too_large` | 超过目标分区大小 |
| 400 | `bad_image` | §2.2 第 1 条头校验失败 |
| 400 | `validate` | `esp_ota_end` 校验失败（`ESP_ERR_OTA_VALIDATE_FAILED`） |
| 408 | `timeout` | 接收连续超时（累计 ≥ 30 s 无数据） |
| 500 | `flash` | `esp_ota_begin`/`write`/`set_boot_partition` 失败 |
| 503 | `aborted` | 上传中被关机请求中止，或客户端断开（断开时应答可能无法送达） |

- 失败任何分支都调用 `esp_ota_abort()`，不改变启动分区，设备继续运行当前固件。
- 已有 WS/页面状态不变；升级进度由浏览器 `XMLHttpRequest.upload.onprogress` 显示（流式写入，接收进度 ≈ 写入进度）。

### 2.4 设备端实现约束

- 写入在 httpd 回调（core 0，httpd 任务）内同步流式完成：`esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, ...)`，按块
  `httpd_req_recv` → `esp_ota_write`。**禁止** `OTA_SIZE_UNKNOWN`/传入整包大小（会一次擦 3 MB，阻塞数十秒且关 cache）。
- 接收缓冲 ≤ 2 KB，不放 httpd 任务栈（默认 4 KB 栈），不新增常驻 `.bss`：从启动内存池按 `kWifiConfig` 模式申请
  （参照 `BootBuffers::webFrames`），蓝牙模式不占内存。本工程代码不新增堆分配；`esp_ota_*` 内部分配记入 G7b 白名单说明。
- 并发：同一时刻只允许一个上传（原子标志）；上传期间：
  - Wi-Fi 空闲回落计时（`WebIdleTimer`）暂停，上传结束后从头计时；
  - WS `set`/`cmd` 请求回 `ack ok:false err:"busy"`（不进设置服务）；
  - 电源关机请求（ADR-0015 / B2 的 `kQuiesce` 阶段）置中止标志，httpd 回调在下一块前 `esp_ota_abort()` 并回 503；
    关机序列等待中止完成（并入现有 `powerPauseDone` 判定，超时沿用 B2 规则）。
- httpd ↔ kLink 交互沿用 ADR-0017 的约束：回调不直接改模块业务状态；需要 kLink 动作（暂停空闲计时、排定重启）时经原子量或现有邮箱。
- 成功后由 kLink 按现有重启机制排定 `reboot_ms`（1500 ms，保证应答发出）后 `esp_restart()`；不写射频模式记录，
  因此新固件以默认 `kBluetooth` 启动。
- 每次升级打 I 日志一行（开始：大小；结束：结果码、字节数、耗时）；块级只用 `fw::trace`。
- Flash 擦写期间 cache 关闭，core 1 上的 kLink/GNSS 会被短暂阻塞，GNSS UART 可能丢字节。Wi-Fi 配置模式下可接受，
  但实现方须在 HIL 记录升级前后 `gnss` 丢字节/坏帧计数，且升级期间不得触发任务看门狗。

### 2.5 回滚与“确认有效”

- `sdkconfig.defaults` 增加 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（冻结文件，Architect 提交）。
  回滚由 **bootloader** 执行：已在用的板子须用串口刷一次新 bootloader 才具备回滚能力；
  旧 bootloader 下 OTA 仍可用，只是新镜像启动失败时不会自动回退（HIL 须覆盖新 bootloader）。
- 启动时若运行镜像状态为 `pending_verify`：
  - 启动步骤出现启动失败回退（`BootFlow` 现有失败路径）→ 立即 `esp_ota_mark_app_invalid_rollback_and_reboot()`；
  - 否则在启动完成后连续运行 `RTK_OTA_VALID_DELAY_S`（Kconfig，默认 30 s）→ `esp_ota_mark_app_valid_cancel_rollback()`，打 I 日志；
  - 这段时间内 panic/看门狗/掉电复位 → bootloader 把它标为 `aborted` 并回到旧槽。
  - 充电待机（B2 `BootMode::kChargeStandby`）下同样计时确认（该模式只跑少量模块，启动正常即视为可用）。
- 判定逻辑写成纯函数（输入：镜像状态、启动结果、运行时长；输出：无动作/确认/判无效），host 测试覆盖。

### 2.6 版本号单一来源

- 当前 `kAppVersion`（`main/AppConfig.hpp`，手写 `"6.1.0-dev"`）与 `esp_app_desc_t.version`（IDF 默认取 `git describe`）不一致。
  O1 改为单一来源：工程 `CMakeLists.txt` 设置 `PROJECT_VER`，运行期 `kAppVersion` 与网页、控制台、`/ota/info` 显示的版本
  都等于 `esp_app_get_description()->version`。

### 2.7 控制台（DEV 档）

- `ota status`：运行分区与状态、启动分区、另一槽版本与状态、`esp_ota_check_rollback_is_possible()`、最近一次升级结果。
- `ota valid`：手动确认（台架用）；`ota rollback`：`esp_ota_mark_app_invalid_rollback_and_reboot()`（HIL 验证回滚用）。
- PRODUCTION / NONE 档不注册 `ota` 组。

### 2.8 页面（O2）

- 新增“固件升级”卡片（系统分组附近）：显示 `/ota/info` 的当前版本、运行分区、另一槽版本；选择 `.bin` 后浏览器端解析
  `esp_app_desc_t`（文件偏移 32 起：`magic_word` u32 小端 = `0xABCD5432`，`version` 在 +16 处 32 B，`project_name` 在 +48 处 32 B），
  magic 或 `project_name` 不符直接提示且不上传；显示“当前 → 新版本（降级时加醒目提示）”，确认后 `XMLHttpRequest` 上传并显示进度。
- 成功：提示“升级完成，设备将重启进入蓝牙模式，热点会消失”；失败按 `err` 显示中文原因。
- 上传期间禁用其他设置控件；页面 gzip 总预算仍 ≤ 24 KB（现 13.7 KB）。
- `tools/web_mock` 实现 `GET /ota/info` 与 `POST /ota`（可配置注入每种 `err`、慢速接收），便于离线调试。

## 3 资源预算

- 蓝牙模式：DRAM 静态增量 ≤ 256 B；空闲堆不得下降（门槛同 ADR-0017 §3）。
- Wi-Fi 模式：升级过程中空闲堆最低 ≥ 30 KB（`esp_ota_begin` 与 lwIP 接收缓冲在 IDF 内部分配）。
- IRAM 余量 ≥ 8 KB；镜像大小增量报告即可（槽 3 MB，现约 1.5 MB）。
- 3 MB 槽 / 1.5 MB 镜像：升级时间目标 ≤ 60 s（实测记入报告，不作硬门槛）。

## 4 构建与冻结文件

- Architect 提交：本 ADR、任务卡、`sdkconfig.defaults` 的 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`，重算 `docs/frozen.sha256`。
- 新 Kconfig（实现方加在 `main/Kconfig.projbuild`）：`RTK_OTA_ENABLE`（默认 y，依赖 `RTK_WIFI_CONFIG_ENABLE`；关闭时不注册 `/ota*`
  路由、不链入 OTA 写入代码，回滚确认逻辑仍保留）、`RTK_OTA_VALID_DELAY_S`（默认 30，范围 10–600）。
- `partitions.csv` 不改。httpd `max_uri_handlers` 按新增路由调大。

## 5 不做的事

- 蓝牙 SPP 升级（手机 0x1D 保持占位）、4G/HTTP 拉取升级、GNSS 接收机固件升级。
- 镜像签名、安全启动、flash 加密、防回滚 eFuse（以后若需要另立 ADR；签名可用 `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`，不烧 eFuse）。
- 从旧 P2（`P2_V3.1_IDF`，4 MB 分区布局、app0 在 0x10000）经 OTA 迁移到本固件：分区表不同，只能串口刷写。
- 升级后自动回到 Wi-Fi 模式。

## 6 任务拆分

| 卡 | 内容 | 依赖 | 执行者 | 评审 | 分支 / 工作树 |
|---|---|---|---|---|---|
| O1 | 设备端：`/ota/info`、`POST /ota` 流式写入与校验、并发/空闲/关机联动、回滚确认、版本单一来源、控制台 `ota`、host 测试 | 本 ADR | Codex | AGY | `feat/o1-ota-fw`，`../RTK_Main-o1` |
| O2 | 页面“固件升级”卡片 + `tools/web_mock` 的 OTA 接口 | §2.3 契约 | AGY | Codex | `feat/o2-ota-ui`，`../RTK_Main-o2` |

两卡并行；Architect 合并 O1+O2、复跑全量门禁并在 P2 v1.41 上做 HIL（见各卡 §HIL）后合入 main。

## 7 用户决定（2026-11-23）

- **Q1 安全门槛**：仅热点口令 + 设备端镜像校验，不另设口令；签名以后另立卡。
- **Q2 降级**：允许；页面显示当前与新版本，用户确认后上传。
