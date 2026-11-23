# O1：网页 OTA 设备端与应用回滚

- 依据：**`docs/adr/ADR-0019-web-ota.md`（逐条遵守，接口契约 §2.3 字节级不得改）**；ADR-0017（Wi-Fi 网页、httpd 约束）；
  ADR-0015 / `docs/tasks/B2-power-key-shutdown.md`（关机序列 `kQuiesce`）；ADR-0011（启动内存池）；`AGENTS.md`、`docs/CODING-STYLE.md`。
- 参考（只读）：旧 P2 `../../P2_V3.1_IDF/main/user_src/user_web_from.c:144-340`（流程参考，不照抄其 multipart/版本比较/看门狗喂狗写法）；
  IDF `~/esp/esp-idf-v6.1/components/app_update/include/esp_ota_ops.h`、`examples/system/ota/native_ota_example`。
- 基线：分支 `feat/ota-base`（main `75cc992` + 本 ADR/任务卡 + `sdkconfig.defaults` 回滚开关）。
- 分支 `feat/o1-ota-fw`，工作树 `../RTK_Main-o1`（仓库根），构建前缀 `builds/o1-*`。
- 执行者：Codex。评审：AGY。执行者不提交、不烧录、不连串口；HIL 由 Architect 做。

## 1 范围

1. **HTTP 路由**（`main/modules/web/`，建议新文件 `WebOta.cpp` + 私有头）：`GET /ota/info`、`POST /ota`，行为、状态码、`err` 码、
   JSON 字段严格按 ADR-0019 §2.3；`max_uri_handlers` 相应调大；门户开启时这两条路由优先于 302 兜底。
2. **流式写入**（ADR-0019 §2.4）：`OTA_WITH_SEQUENTIAL_WRITES`；头部校验在擦写第一块之前完成（先收齐头再 `esp_ota_begin`）；
   任何失败 `esp_ota_abort()`；接收超时累计 30 s → `timeout`；客户端断开 → `aborted`。
3. **缓冲**：≤ 2 KB，从启动内存池在 `kWifiConfig` 下申请（参照 `BootBuffers::webFrames` 的申请/绑定/回滚路径），蓝牙模式不申请；
   不放 httpd 栈，不新增常驻 `.bss` > 256 B。
4. **并发与联动**：单上传原子标志；上传中暂停 `WebIdleTimer`、WS `set`/`cmd` 回 `busy`；电源关机请求中止上传并在 B2 关机序列中等待中止
   （并入 `powerPauseDone` 或等价判定，说明选择）；成功后经现有 `RebootKind` 机制新增 `kOta`，1500 ms 后重启（不写射频记录）。
5. **回滚确认**（ADR-0019 §2.5）：纯函数判定（状态 × 启动结果 × 运行时长 → 无动作/确认/判无效），放无 IDF 头的文件，接入启动流程与 kLink
   定时；启动失败回退路径中 `pending_verify` → `esp_ota_mark_app_invalid_rollback_and_reboot()`；充电待机同样计时确认。
6. **版本单一来源**（§2.6）：根 `CMakeLists.txt` 设 `PROJECT_VER`（初值 `"6.1.0-dev"`）；`kAppVersion` 的所有使用点改为运行期取
   `esp_app_get_description()->version`（host 检查工程用替身）；网页 `fw` 字段、控制台、`/ota/info` 一致。
7. **控制台**（DEV 档）：`ota status|valid|rollback`（§2.7），输出末行 `OK`/`ERR …`；PRODUCTION/NONE 不注册。
8. **Kconfig**：`RTK_OTA_ENABLE`（默认 y，depends on `RTK_WIFI_CONFIG_ENABLE`）、`RTK_OTA_VALID_DELAY_S`（默认 30，range 10 600）。
   `RTK_OTA_ENABLE=n` 时不注册路由、不链入写入代码；回滚确认始终保留。
9. **日志**：每次升级开始/结束各一行 I（大小、结果码、字节数、耗时 ms）；块级只 `fw::trace`；确认/判无效各一行 I。

## 2 不做

- 页面与 `tools/web_mock`（O2 负责）；手机 0x1D；签名/加密；改 `partitions.csv`、`sdkconfig.defaults`（已由 Architect 提交）。

## 3 测试（执行者）

- Host（普通 + ASan/UBSan）：
  - 镜像头校验：正常、magic 错、chip_id 错、app desc magic 错、project_name 不同、头不足、`Content-Length` 0/超限/恰好等于分区大小；
  - 回滚判定纯函数：`pending_verify` + 启动失败 → 判无效；`pending_verify` + 正常 + 未满/刚满/超过延时；`valid`/`undefined` → 无动作；时钟回绕；
  - 若把 POST 处理写成可注入的状态机（推荐：recv/ota 操作表注入），覆盖：分块边界跨头部、超时累计、中止标志、`esp_ota_end` 失败、`set_boot` 失败、成功。
- IDF 构建：默认、`CONFIG_RTK_TRACE_ENABLE=n`、`RTK_WIFI_CONFIG_ENABLE=n`（G3-idf-wifi-off）、`RTK_OTA_ENABLE=n`、PRODUCTION 与 NONE 档，全部 0 warning。
- 全量门禁 `bash tools/ci/run_gates.sh --prefix o1`（含 g7b、doc_check、clang-format、frozen 校验）全过。
- 报告 `docs/reports/O1-ota-firmware.md`：改动文件、DRAM/IRAM/bin 差值（蓝牙模式与 Wi-Fi 模式）、门禁逐项结果、对 ADR 的偏离、未决问题、
  给 Architect 的 HIL 操作步骤（含 curl 命令，见 §4）。

## 4 HIL（Architect，P2 v1.41，先串口刷新 bootloader）

1. `curl http://192.168.10.12/ota/info` 字段齐全；`ota status` 一致。
2. `curl --data-binary @builds/o1/rtk_main.bin -H 'Content-Type: application/octet-stream' http://192.168.10.12/ota` → 200，重启后进蓝牙模式，
   `ota status` 显示 `pending_verify` → 30 s 后 `valid`；另一槽为旧版本。
3. 错误路径：随意 `.bin`（非本工程）→ 400 `bad_image`，设备不重启、不擦写；截断文件 → 400 `validate`；两个并发上传 → 一个 409 `busy`；
   上传中拔掉终端 Wi-Fi → `aborted`，设备继续运行旧固件。
4. 回滚：刷入一个启动即 `abort()` 的测试镜像（或在新镜像确认前执行 `ota rollback`）→ 回到旧槽，`ota status` 显示另一槽 `aborted/invalid`。
5. 上传中长按电源键 → 上传中止、正常关机；上传全程无任务看门狗复位；记录 GNSS 丢字节/坏帧计数与升级耗时、Wi-Fi 模式最低空闲堆。
