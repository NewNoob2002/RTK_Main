# O1 网页 OTA 固件与应用回滚

基线为 `02f6006`（`feat/ota-base`），实现位于 `feat/o1-ota-fw`。构建使用 ESP-IDF v6.1、ESP32 Xtensa、C++20 子集（项目 T2）。本卡的实现与验证由 Codex 执行，AGY 评审及 Architect 台架验收待完成。

## 实现

| 文件 | 改动 |
|---|---|
| `CMakeLists.txt`、`main/{CMakeLists.txt,Kconfig.projbuild,AppConfig.hpp}` | 工程版本来自 `PROJECT_VER`，固件运行时读取 app 描述符，host 使用替身。新增上传开关和确认延时。关闭上传时裁掉 `WebOta.cpp`，保留应用回滚。 |
| `main/modules/web/WebOta{.cpp,Transfer.hpp,Reply.hpp}` | 实现两条 HTTP 路由、§2.3 JSON、分段校验、顺序 Flash 写入、连续无数据 30 s 超时、失败清理和启动槽提交。纯逻辑可注入接收和 Flash 操作。 |
| `main/modules/web/{WebModule.hpp,WebModule.cpp,WebServer.cpp,WebIdleTimer.cpp,WebIdleTimer.hpp}` | 提高 URI 数量并在 302 兜底前注册两条路由。暂停上传期间的空闲回落，拦截 kLink 收到的 WS `set`/`cmd`。成功回执后，由 kLink 通过 `RebootKind::kOta` 延迟 1500 ms 调用 `esp_restart()`。 |
| `main/app/{BootBuffers.cpp,BootBuffers.hpp,BootReasons.hpp,BootFlow.cpp}` | 正常 Wi-Fi 启动从 BootArena 申请并绑定 2048 B 的 `web.ota`，启动回退解绑回收。已有启动降级路径先检查是否需要回滚，启动完成后发布确认起点。 |
| `main/modules/ota/{OtaPolicy.hpp,OtaRuntime.hpp,OtaRuntime.cpp}`、`main/modules/system/SystemModule.{cpp,hpp}` | 纯回滚判定及跨任务 OTA 状态修改仲裁。`system` 在三种运行模式都由 kLink 轮询，包括充电待机。启动失败的待确认镜像立即申请回滚，启动正常在延时到期后确认。 |
| `main/modules/ota/OtaCmds.{cpp,hpp}`、`main/app/{ConsoleRegistration.hpp,ConsoleSetup.cpp}` | DEV 注册 `ota status\|valid\|rollback`。回滚成功回执从 IDF 重启钩子发出，失败返回 `ERR`。 |
| `main/modules/{bt_link/BtLinkModule.cpp,system/SystemCmds.cpp}` | 蓝牙设备版本和 `sys info` 改用镜像描述符，与启动横幅、WS `hello.fw` 和 `/ota/info` 一致。 |
| `main/def/SystemDef.hpp`、`main/modules/debug/TraceText.cpp`、`tools/g7b_check/heap_allowlist.txt` | 升级开始与结束各一行 I，块级写入只走 trace。记录 IDF 单个 OTA 会话的间接分配和清理。 |
| `test/ota/`、`tools/main_host_check/ConsoleProfileCheck.cpp`、`tools/ci/run_gates.sh` | 新增 OTA host 普通与 sanitizer 门禁和 OTA-off 构建，更新 DEV、PRODUCTION、NONE 的命令注册断言。 |

校验使用 ESP-IDF v6.1 的 `esp_app_format.h` 和 `esp_app_desc.h` 静态核对线格式偏移，读取 288 B 镜像头后才调用 `esp_ota_begin(..., OTA_WITH_SEQUENTIAL_WRITES, ...)`。基于同版本 `esp_ota_ops.c`，`esp_ota_end()` 成功或失败都会消费句柄。失败路径统一调用 `esp_ota_abort()`，未创建或已消费句柄的 `ESP_ERR_NOT_FOUND` 作为预期结果。截断文件以实际上传长度核对镜像元数据，防止目标槽残留数据补足旧镜像尾部。

采用 B2 的 `powerPauseDone()`。关机请求关闭新上传准入，当前上传在块边界检查信号，先中止并释放句柄。随后同一 httpd 任务执行已排队的 SoftAP 暂停，沿用 B2 的 5500 ms 等待上限。启动槽提交前原子取得重启权，关机若已获受理，上传返回 `aborted`。

## 资源

基线使用 `builds/o1-baseline-idf`，O1 使用 `builds/o1-idf`。两者均为同一环境的 ESP32 默认 DEV 构建。通过 `python -m esp_idf_size <map>` 读取内存区，使用 `stat` 读取实际 `.bin` 字节数。

| 指标 | 基线 | O1 | 差值 |
|---|---:|---:|---:|
| 静态 DRAM（蓝牙和 Wi-Fi） | 97,387 B | 96,395 B | −992 B |
| DRAM `.data` | 52,683 B | 51,675 B | −1,008 B |
| DRAM `.bss` | 44,704 B | 44,720 B | +16 B |
| IRAM 已用（蓝牙和 Wi-Fi） | 115,435 B | 115,435 B | 0 B |
| IRAM 余量 | 15,637 B | 15,637 B | 0 B |
| `rtk_main.bin`（蓝牙和 Wi-Fi） | 1,489,072 B | 1,505,984 B | +16,912 B |
| 蓝牙 `web.ota` 启动池 | 0 B | 0 B | 0 B |
| Wi-Fi `web.ota` 启动池 | 0 B | 2,048 B | +2,048 B |

`cpp_cost.py diff` 的 ELF 口径下，`text` +13,288 B，`data` +3,624 B，`bss` +16 B。该工具的 `data` 汇总包含 Flash 只读区，静态 DRAM 以 IDF memory type 为准。首轮在 IDF 编译数据库上追加 `-fstack-usage`，GET 的 `handleOtaInfo`、槽查询 `renderInfo` 与 JSON `renderInfo` 分别为 64、400、1,168 B，至 JSON 编码累计 1,632 B。POST 的 `handleOtaPost` 为 448 B，成功 JSON 编码为 1,120 B，接收块函数为 48 B。修复轮复测见下文。[资源日志](../../builds/o1-logs/size-final.log) · [ELF 差异](../../builds/o1-logs/cpp-cost.log) · [首轮目标栈帧](../../builds/o1-logs/WebOta-stack.su)

BT 和 Wi-Fi 运行同一固件，链接数据对两个模式相同。BT 模式与充电待机的 OTA 启动池用量为 0 B，Wi-Fi 增加 2048 B。门户 DNS 复用 kLink 串行使用的两个已有 Web 工作帧，移除了原来常驻的两个 512 B 缓冲。OTA 接收缓冲来自 BootArena，IDF 的写入会话和 lwIP 接收仍有运行期堆需求。

## 验证

执行 `bash tools/ci/run_gates.sh --prefix o1`，最终 42 项 PASS、1 项 FAIL。G1 的唯一不匹配是任务卡 §1.6 要求修改的工程根 `CMakeLists.txt`。基线副本经同一冻结清单验证通过。`AGENTS.md §4` 将清单更新保留给 Architect，当前等待更新受影响的单条散列。[门禁总日志](../../builds/o1-logs/gates.log) · [冻结校验](../../builds/o1-logs/G1-frozen.log)

默认沙箱的首次试跑中，LeakSanitizer 附加自身测试进程时返回 EPERM。最终门禁通过自动审核在支持该附加操作的环境复跑，保留 ASan、UBSan 与 LeakSanitizer。G6 使用已有的离线缓存 `lizard 1.26.1`。

| Host 套件 | G2 普通 | G2b ASan / UBSan |
|---|---|---|
| `host` | PASS | PASS |
| `app_fw` | PASS | PASS |
| `services` | PASS | PASS |
| `stream_demux` | PASS | PASS |
| `bt_reconnect` | PASS | PASS |
| `cli` | PASS | PASS |
| `gnss` | PASS | PASS |
| `nvs_store` | PASS | PASS |
| `web_proto` | PASS | PASS |
| `settings` | PASS | PASS |
| `ota` | PASS | PASS |
| `main_host_check` | PASS | PASS |
| `t13_host_check` | PASS | PASS |
| `gnss-all` | PASS | PASS |

OTA host 测试覆盖正常与错误头、零长度／头长边界／槽大小／超限、回滚启动结果与计时回绕、跨头分块、累计超时与进度复位、主动中止、EOF、begin/write/end/set_boot 失败和成功，及 §2.3 的状态码和 JSON 字节。

| 门禁 | 结果 | 证据 |
|---|---|---|
| `G1-frozen` | FAIL | 唯一变化为 `CMakeLists.txt`，等待 Architect 更新冻结清单。 |
| `G3-idf` | PASS | 默认，0 warning。 |
| `G3-idf-trace-off` | PASS | `RTK_TRACE_ENABLE=n`，0 warning。 |
| `G3-idf-wifi-off` | PASS | `RTK_WIFI_CONFIG_ENABLE=n`，0 warning。 |
| `G3-idf-ota-off` | PASS | `RTK_OTA_ENABLE=n`，0 warning。 |
| `G3-idf-production` | PASS | PRODUCTION，0 warning。 |
| `G3-idf-console-none` | PASS | NONE，0 warning。 |
| `G3-idf-none-wifi` | PASS | NONE + Wi-Fi，0 warning。 |
| `G3b-rtk_pure-esp32` | PASS | ESP32 纯组件，0 warning。 |
| `G3b-rtk_pure-esp32c3` | PASS | ESP32-C3 纯组件，0 warning。 |
| `G5-format` | PASS | clang-format 全量。 |
| `G5b-tidy` | PASS | clang-tidy 全量。 |
| `G6-doc_check` | PASS | 新增及改动函数注释。 |
| `G7b-runtime` | PASS | 保留既有异常/RTTI 拉入链，自有目标文件没有新增直接堆引用。 |
| `G7c-no-cli-deps` | PASS | NONE + Wi-Fi 保持 CLI 裁剪。 |

检查 ELF 符号确认 OTA-off 与 Wi-Fi-off 中没有 HTTP OTA handler、`esp_ota_begin`、`esp_ota_write` 或 `esp_ota_end`，仍有启动确认逻辑。PRODUCTION 与 NONE 中没有 `otaCmds`。检查 `.bin` 描述符确认 6 个任务卡构建均为 `rtk_main` / `6.1.0-dev`。

## 契约与待确认项

- ADR-0019 §2.3 的路由、状态码、`err`、字段顺序和成功延时见 `test/ota/TestOta.cpp` 的逐字节断言。小于或等于 288 B 的非零请求以及错误媒体类型返回 `400 bad_image`。零长度返回 `411 length`。
- ADR-0019 §2.4 指定的同步 httpd 模型串行分派请求。上传时 kLink 处理已入队的 WS 变更会回 `busy`。正在等待 HTTP 分派的连接会在当前回调退出后被处理。成功提交后仍保留 OTA 占用直到重启，这期间分派的 POST 和 WS 变更都会回 `busy`。若前一次失败并已释放占用，之后才获分派的请求可被受理。同步分派下无法观察这些请求在 TCP 层的准确到达时刻。
- 接收到 TCP 断开或 SoftAP 站点归零时终止为 `aborted`。两个站点同时接入时，若上传站点静默失联且 socket 未报告断开，按连续无数据的 `timeout` 处理。`ota status` 的 `last_upload` 记录本次启动中最后一个获准上传的结果，重启后查看槽版本及状态确认结果。
- HIL **NOT_RUN**。待确认蓝牙负载下堆水位、Wi-Fi 升级最低空闲堆 ≥ 30 KB、升级耗时、GNSS 干扰、充电待机自动确认及实板回滚。应用回滚须使用启用回滚的 bootloader。对比升级前后的版本与槽位时，选用不同版本的同工程镜像。

## Architect 台架步骤

1. 在 P2 v1.41 上串口更新启用回滚的 bootloader。用已知良好的 O1 DEV 固件启动并运行 `ota status`。确认当前启动槽为 `valid`。执行 `radio wifi` 后连接热点。用任意 Host 头验证 OTA 信息路由绕过门户 302，与 `ota status` 核对运行版本、槽状态和另一槽版本。

   ```sh
   curl -sS -i -H 'Host: captive.invalid' http://192.168.10.12/ota/info
   ```

2. 在成功升级前验证以下错误路径。前两条分别应返回 `411 length`、`413 too_large`。下一条应为 `400 bad_image`，本次升级日志的接收字节数为 288 B，启动槽保持原值。截断测试应为 `400 validate`；随后重复读取 `/ota/info` 并确认设备持续运行。

   ```sh
   ota_url=http://192.168.10.12/ota
   ota_image=builds/o1-idf/rtk_main.bin
   ota_type='Content-Type: application/octet-stream'
   curl -sS -i -X POST -H "$ota_type" "$ota_url"
   curl -sS -i -X POST -H "$ota_type" -H 'Content-Length: 3145729' "$ota_url"
   curl -sS -i -H "$ota_type" --data-binary @builds/o1-rtk_pure-esp32/rtk_pure.bin "$ota_url"
   head -c 4096 "$ota_image" > builds/o1-logs/hil-truncated.bin
   curl -sS -i -H "$ota_type" --data-binary @builds/o1-logs/hil-truncated.bin "$ota_url"
   ```

3. 上传成功应返回 `200` 和 `reboot_ms:1500`。重启后进入蓝牙模式，从控制台观察 `pending_verify`，正常运行 30 s 后变为 `valid`，另一槽保留旧版本。在另一轮测试中，用下面的一对命令发起并发上传，预期第一条成功，第二条在 httpd 恢复分派后返回 `409 busy`。首个上传已进入接收后，再发送第二个请求。第二条用 `Expect` 等待以免在收到拒绝前就上传正文。可在并行请求前预建一条 WS 连接并发送 `set` 和 `cmd`，检查 `ack` 的 `busy`。

   ```sh
   curl -sS -i -H "$ota_type" --data-binary @"$ota_image" "$ota_url"

   curl -sS -i --limit-rate 128k -H 'Expect:' -H "$ota_type" \
     --data-binary @"$ota_image" "$ota_url" &
   ota_first=$!
   sleep 2
   curl -sS -i -H 'Expect: 100-continue' --expect100-timeout 45 -H "$ota_type" \
     --data-binary @"$ota_image" "$ota_url"
   wait "$ota_first"
   ```

4. 单终端连热点，用 `--limit-rate 32k` 慢速上传，传输中关闭终端 Wi-Fi。通过控制台观察 `aborted` 并确认原槽仍运行。另一次慢速上传中长按电源键，确认中止日志和正常关机。有 USB 时进入充电待机，断开 USB 后保持关机。
5. 再次升级，在新镜像 `pending_verify` 的 30 s 窗口内执行 `ota rollback`，确认命令末行 `OK`、重启回旧槽且另一槽为 `invalid`。用启动即 `abort()` 的同工程测试镜像复测 bootloader 自动回退，另一槽应为 `aborted`。用 `RTK_BOOT_FAULT_STEP="started"` 的测试镜像可另测启动降级路径，应记录判无效并重启。充电待机的计时用一份处于 `pending_verify` 的健康镜像，在 USB 供电、U3 关闭的条件下单独验证。
6. 每轮升级前、传输期间和重启前采样 `gnss status` 的 `uart.rx`、`nmea.bad`、`demux.unknown/resyncs` 和 `pipe.drop`。比较同一时间窗内的正常字节率与坏帧、丢失计数。用 `sys mem` 记录最低空闲堆，记录升级日志的 `elapsed` 和重启原因。蓝牙模式用手机持续下发 RTCM 10 min，按 ADR-0017 比较基线与 O1 最低空闲堆。为避免统计值在重启时归零，建议在传输末尾使用独立的控制台采样脚本记录。

## 修复轮

| 发现 | 处理及改动文件 | 复测 |
|---|---|---|
| F1 | `main/modules/web/WebOtaReply.hpp` 直接编码到既有的 1,024 B httpd TX 帧，GET 与 POST 共用转义函数，直接写入 `other:null`。 `main/modules/web/WebOta.cpp` 在现有 2,048 B OTA 工作区内构造并读出另一槽的 `esp_app_desc_t`。绑定时验证对齐与容量，GET 结束后 POST 可重新使用同一工作区。 `main/app/BootBuffers.{cpp,hpp}` 与 `main/modules/web/WebModule.hpp` 更新工作区类型和契约。 | OTA host 测试验证等长和短 1 B 缓冲、控制字符、UTF-8 及 JSON 原文。真实路由接入主机替身后验证可读槽、不可读槽及对齐拒绝。修复轮静态 `.bss` 增量为 0 B。 |
| F2 | `main/modules/web/WebOta.cpp` 先查头值长度并预留 NUL，将完整媒体类型读入 OTA 工作区。截去参数并裁剪空格或制表符后，与 `application/octet-stream` 做 ASCII 不区分大小写的等长比较。`Content-Length` 同样在读取前检查容量。 | `tools/main_host_check/WebModuleCheck.cpp` 模拟 IDF v6.1 的截断边界。25 B 非法后缀在接收前返回 `400 bad_image`，合法参数、大小写变体及尾随空白可进入接收。 |
| F3 | `main/modules/web/WebOta.cpp` 将 OTA 连接的单次接收等待缩短为 1 s，成功提交后恢复原 socket 选项。30 s 无进展超时继续由传输逻辑判定。站点归零时中止并直接关闭。其余失败应答通过一次 `MSG_DONTWAIT` 发送完整 HTTP 回包，随后由 httpd 关闭会话，避免等待发送窗口、应答事件队列或剩余请求体。在站点仍可达时保留 §2.3 的 503 和 JSON 正文。 | `tools/main_host_check/WebModuleCheck.cpp` 核对 503 完整报文。模拟写入后等待接收时发起关机，窗口满时完成 abort 并释放 OTA 仲裁，执行已排队的 SoftAP 暂停后 `powerPauseDone()` 成立。站点归零测试验证直接关闭。 |
| F4 | `main/modules/ota/OtaRuntime.cpp` 的 `confirm()` 成功记 I，失败记 E 并记录 SDK 错误名。 | 检查两条分支与状态映射。目标构建及回滚判定 host 用例通过。 |
| F5 | `main/app/BootBuffers.{cpp,hpp}`、`main/modules/debug/TraceText.cpp`、`main/modules/web/{WebModule.hpp,WebModule.cpp,WebServer.cpp}` 的 22 处既有 OTA 条件统一检查宏已定义且值为真。`tools/main_host_check/{CMakeLists.txt,BootAllocCheck.cpp,WebModuleCheck.cpp,web_shim/esp_http_server.h,web_shim/esp_image_format.h,web_shim/esp_ota_ops.h,web_shim/lwip/sockets.h}` 用 IDF 替身运行真实 BootArena 绑定、OTA 路由及 WebModule。 | 普通和 ASan/UBSan 各 103/103。验证 Wi-Fi 的 2,048 B 分配、失败回收、蓝牙与待机裁剪、WS `set`/`cmd` 返回 `busy`、失败后空闲计时重启、关机等待及成功后 1,500 ms 直接重启。显式 OTA=0 的 Web 目标无 OTA 路由。 |
| F6 | `main/modules/web/WebServer.cpp` 的 `resetRuntimeState()` 清除 `otaIdleReset_` 和 `otaRebootRequested_`。 | 主机在残留空闲重置或重启请求的情况下停止并重新初始化同一实例，验证新实例的空闲期限和重启行为。 |
| F7 | 本报告补录首轮 GET 调用链，并列出修复后 GET、POST 的自有栈帧。 | 使用最终默认 ESP32 构建的编译参数追加 `-fstack-usage`，并检查最终 ELF。 |

F3 的 B2 等待仍为 5,500 ms。IDF v6.1 在关闭会话时仍可发布一次 `DISCONNECTED` 事件，默认队列等待配置为 2,000 ms。OTA 将单次 socket 接收从 5 s 缩至 1 s，并消除失败应答的等待发送窗口路径。Flash 调用、事件调度、TCP 关闭及 SoftAP 停止的合计实机时延仍由 Architect 台架验证。出现发送背压时只尝试一次，在会话关闭前应答可能无法完整送达。

### 修复轮栈与内存

默认 ESP32 DEV 构建沿用 4,096 B httpd 栈。用 `builds/o1-idf/compile_commands.json` 中 `WebOta.cpp` 的命令追加 `-fstack-usage`，并对最终 ELF 检查 Xtensa `entry` 指令及调用关系。表中的合计为同一路径上同时存活的 OTA 自有栈帧。

| 路径 | 首轮 | 修复后 |
|---|---|---|
| GET `/ota/info` 到 JSON 编码 | `handleOtaInfo` 64 B → 槽查询 `renderInfo` 400 B → JSON `renderInfo` 1,168 B。到编码函数累计 1,632 B。 | 槽查询内联到 `handleOtaInfo`（128 B）→ JSON `renderInfo`（80 B），到编码函数累计 **208 B**。再进入 `appendString` 或 `appendText`（各 48 B），自有编码调用链最深 **256 B**。 |
| POST `/ota` 成功回执 | `handleOtaPost` 448 B → `renderSuccess` 1,120 B，到编码函数累计 1,568 B。 | `renderSuccess` 内联到 `handleOtaPost`（512 B）。再进入字符串编码（48 B），自有成功回执调用链 **560 B**。 |
| POST `/ota` 接收数据 | `handleOtaPost` 448 B → `receiveBlock` 48 B，累计 496 B。 | `handleOtaPost` 512 B → `receiveBlock` 48 B，累计 **560 B**。 |

反汇编核对了进入 URI 前仍在栈上的四个 IDF 帧 `httpd_thread` 144 B、`httpd_sess_process` 32 B、`httpd_req_new` 176 B 和 `httpd_uri` 80 B。GET 的另一槽读取分支经 `esp_ota_get_partition_description`（32 B）、`esp_partition_read`（48 B）至 `esp_flash_read`（64 B），与 GET 回调帧合计 272 B，后续进入 SDK 内部 Flash 例程。实际峰值受会话遍历、SDK 调用、中断及调度影响，整任务水位待台架测量。[GCC 栈帧](../../builds/o1-logs/WebOta-repair-stack.su) · [修复后反汇编](../../builds/o1-logs/o1-idf-repair-disasm.log) · [评审构建反汇编](../../builds/o1-logs/r1-idf-repair-disasm.log)

| 指标 | O1 基线 | 首轮 O1 | 修复后 O1 | 相对基线 / 相对首轮 |
|---|---:|---:|---:|---:|
| 静态 DRAM（蓝牙和 Wi-Fi） | 97,387 B | 96,395 B | 96,395 B | −992 / 0 B |
| DRAM `.data` | 52,683 B | 51,675 B | 51,675 B | −1,008 / 0 B |
| DRAM `.bss` | 44,704 B | 44,720 B | 44,720 B | +16 / 0 B |
| IRAM 已用（蓝牙和 Wi-Fi） | 115,435 B | 115,435 B | 115,435 B | 0 / 0 B |
| IRAM 余量 | 15,637 B | 15,637 B | 15,637 B | 0 / 0 B |
| `rtk_main.bin`（蓝牙和 Wi-Fi） | 1,489,072 B | 1,505,984 B | 1,508,368 B | +19,296 / +2,384 B |
| 蓝牙及充电待机 `web.ota` 启动池 | 0 B | 0 B | 0 B | 0 / 0 B |
| Wi-Fi `web.ota` 启动池 | 0 B | 2,048 B | 2,048 B | +2,048 / 0 B |

`cpp_cost.py diff` 对 O1 基线的 ELF 差值为 `text` +15,372 B、`data` +3,928 B、`bss` +16 B。相对首轮分别为 +2,084 B、+304 B、0 B。ELF 的 `data` 口径包含 Flash 只读区，表中静态 DRAM 取自 IDF map。[IDF 体积与镜像字节数](../../builds/o1-logs/repair-size.log) · [基线 ELF 差异](../../builds/o1-logs/repair-cpp-cost.log) · [首轮 ELF 差异](../../builds/o1-logs/repair-cpp-cost-r1.log)

### 修复轮最终复测

在指定 ESP-IDF v6.1 环境执行 `timeout 1200 bash tools/ci/run_gates.sh --prefix o1`。默认沙箱拒绝 LeakSanitizer 附加，由自动审核环境保留 ASan、UBSan 与 LeakSanitizer 完成最终复跑。全量 42 项 PASS。唯一 FAIL 是 `G1-frozen` 的工程根 `CMakeLists.txt`，由 Architect 处理。[最终门禁总日志](../../builds/o1-logs/repair-gates.log) · [冻结校验](../../builds/o1-logs/G1-frozen.log)

| 复测 | 结果 |
|---|---|
| G2、G2b | 普通及 sanitizer 各 14 套通过，含全驱动 GNSS。OTA 专用套件各 1/1，`main_host_check` 各 103/103。 |
| 任务卡 §3 G3 | 默认、trace-off、Wi-Fi-off、OTA-off、PRODUCTION、NONE 均通过。每份构建日志 0 warning。 |
| 额外 G3 / G3b | NONE + Wi-Fi、纯组件 ESP32、纯组件 ESP32-C3 均通过。每份构建日志 0 warning。 |
| G5、G5b、G6 | clang-format、clang-tidy、Doxygen 门禁通过。 |
| G7b、G7c | 堆符号、启动守卫和异常/RTTI 检查通过。NONE + Wi-Fi 的 CLI 裁剪通过。 |
| ELF 与镜像描述符 | Wi-Fi-off、OTA-off 裁掉两条路由与 OTA 写入符号，保留启动确认。PRODUCTION、NONE 无 `otaCmds`。全部产品镜像的描述符为 `rtk_main` / `6.1.0-dev`。 |
| HIL | **NOT_RUN**。实机 httpd 栈水位、最低空闲堆、卡顿上传中的关机时延以及应用回滚由 Architect 台架验证。 |

[构建配置与符号复核](../../builds/o1-logs/repair-idf-configs.log) · [主机常规](../../builds/o1-logs/G2-main_host_check.log) · [主机 sanitizer](../../builds/o1-logs/G2b-main_host_check-asan.log) · [G7b](../../builds/o1-logs/G7b-runtime.log)
