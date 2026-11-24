# O3：OTA HIL 缺陷修复报告（F-CONC / F-OTHER）

- 分支：`feat/o3-ota-fix`（基线：`feat/ota-base` `1ec627e`）
- 契约：`docs/adr/ADR-0019-web-ota.md`（§2.3、§2.4）
- 硬件边界：未执行烧录、串口连接、git commit 或联网操作；未修改任何冻结文件。

## 1. 改动文件

| 文件 | 变更摘要 |
|---|---|
| `main/modules/web/WebOtaTransfer.hpp` | 新增 `kTotalTimeoutMs = 150000U`（低于浏览器 `OTA_TOTAL_MS = 180000`）与回绕安全的 `elapsedAtLeast()`；将准入起点 `startMs` 透传至 `transfer()` 与 `receiveBlock()`，在头部/正文阶段的每轮循环（含接收成功与 `timedOut` 分支）及写入前同时检查 30 s 无进展超时与 150 s 总时限，超限返回 `UploadCode::kTimeout`。 |
| `main/modules/web/WebOta.cpp` | `handleOtaPost()` 将准入时刻 `startMs` 传入 `runUpload()` → `web_ota::transfer()`；`OtaUploadIo::begin()` 在 `esp_ota_begin()` 成功后调用 `ota::markOtherSlotDirty()`，`setBoot()` 在 `esp_ota_set_boot_partition()` 成功后调用 `ota::clearOtherSlotDirty()`；`renderInfo()` 改为复用共享辅助函数 `ota::otherSlotVersion()`。 |
| `main/modules/ota/OtaPolicy.hpp` | 新增纯函数 `otherSlotVersionReadable(bool stateDefined, bool uploadDirty)`，供主机测试与运行时共用。 |
| `main/modules/ota/OtaRuntime.hpp` | 声明 `markOtherSlotDirty()`、`clearOtherSlotDirty()` 与共享读取辅助 `otherSlotVersion(const esp_partition_t *part, esp_app_desc_t &desc)`。 |
| `main/modules/ota/OtaRuntime.cpp` | 新增 1 B 原子脏标志 `s_other_slot_dirty` 及上述三个函数实现：当另一槽指针为空、状态为 `ESP_OTA_IMG_UNDEFINED`/查询失败、本轮启动已有未提交上传、或描述符读取失败时返回 `std::nullopt`。 |
| `main/modules/ota/OtaCmds.cpp` | `cmdOtaStatus()` 改用 `ota::otherSlotVersion(other, desc).value_or("-")`，与 `/ota/info` 共享同一判定逻辑，消除重复代码。 |
| `test/ota/TestOta.cpp` | 新增 `otherSlotVersionReadable` 全组合与 `kTotalTimeoutMs`/`elapsedAtLeast` 回绕 `static_assert`；扩展 `FakeOtaIo` 覆盖快速上传、间歇停顿（>30 s 且 <150 s）、稳态慢滴超 150 s、时钟回绕超 150 s、头部阶段慢滴超 150 s、单次 `recv` 超时分支触碰 150 s、以及前置准入延迟场景。 |
| `tools/main_host_check/WebModuleCheck.cpp` | 扩展 `MockOtaState` 支持注入另一槽状态及查询错误；新增 `testOtaOtherSlotAndTotalTimeout()` 覆盖 `ESP_OTA_IMG_UNDEFINED`、状态查询失败、中止上传后的脏标志抑制、150 s 总时限返回 HTTP 408 `"timeout"` 与清理，以及后续成功提交后恢复另一槽版本显示。 |
| `tools/web_mock/web_mock.py` | 新增 `OTA_TOTAL_TIMEOUT_S = 150.0` 与可配置属性 `MockState.ota_total_timeout_s`，在 `_handle_ota_post()` 中镜像设备端自准入起算的总上传时限（含读取超时截断与 `slow_kbps` 限速等待）；在 `run_selftest()` 新增用例 `8d-4` 验证稳态慢滴触碰总时限返回 408 并释放 `busy`。 |
| `docs/reports/O3-ota-hil-fixes.md` | 本任务报告。 |

## 2. 验证命令与结果

| 检查项 | 命令 | 结果 | 备注 |
|---|---|---|---|
| 格式化检查 | `clang-format --style=file --dry-run --Werror ...`（含于 `G5-format`） | PASS | 全部改动 C/C++ 文件 0 差异 |
| 冻结文件校验 | `sha256sum -c --quiet docs/frozen.sha256`（含于 `G1-frozen`） | PASS | 无任何冻结文件改动 |
| 网页打包与预算 | `python3 -I tools/web_mock/pack.py` | PASS | `raw=56012 B, gzip=18145 B / 24576 B (73.8%)` |
| Web Mock 自检 | `python3 -I tools/web_mock/web_mock.py --selftest` | PASS | 含新增 `8d-4` 稳态慢滴总时限 408 用例（需本机 loopback 套接字） |
| 网页 OTA E2E | `timeout 300 node tools/web_mock/e2e_ota_ui.mjs` | PASS | 全部 8 组 E2E UI 用例通过（需本机 loopback 套接字） |
| 全量 CI 门禁 | `bash tools/ci/run_gates.sh --prefix o3`（已按 AGENTS.md §5 导出 IDF 6.1 环境） | PASS | **43 passed, 0 failed**（说明：沙箱内 `ptrace` 限制 LeakSanitizer 且 `~/.cache/uv` 不可写，放开沙箱后 43/43 全过） |
| 固件体积统计 | `idf.py -B builds/o3-idf size` | PASS | `rtk_main.bin` 1,516,736 B（槽余量 23%）；IRAM 剩余 14,933 B（≥ 8 KB）；DRAM 静态增量 +1 B（`s_other_slot_dirty`） |

`run_gates.sh --prefix o3` 分项明细（43/43 PASS）：
- `G1-frozen`：PASS
- `G2-*` + `G2b-*-asan`（`host`、`app_fw`、`services`、`stream_demux`、`bt_reconnect`、`cli`、`gnss`、`gnss-all`、`nvs_store`、`web_proto`、`settings`、`ota`、`main_host_check`、`t13_host_check`，共 28 项）：全部 PASS
- `G5-format`：PASS
- `G6-doc_check`（base `75cc9928`）：PASS
- `G3-idf`、`G3-idf-trace-off`、`G3-idf-wifi-off`、`G3-idf-ota-off`、`G3-idf-production`、`G3-idf-console-none`、`G3-idf-none-wifi`、`G3b-rtk_pure-esp32`、`G3b-rtk_pure-esp32c3`（共 9 项，均 0 warning）：全部 PASS
- `G5b-tidy`：PASS
- `G7b-runtime`：PASS
- `G7c-no-cli-deps`：PASS

## 3. 建议的 ADR-0019 修订文本（供 Architect 合并）

`docs/adr/ADR-0019-web-ota.md` 属于冻结文件，本任务未修改；建议 Architect 应用如下修订：

### §2.3（`POST /ota` 应答表 `408` 行与 `other` 字段说明）

将 `408` 行：
```markdown
| 408 | `timeout` | 接收连续超时（累计 ≥ 30 s 无数据） |
```
改为：
```markdown
| 408 | `timeout` | 接收连续超时（累计 ≥ 30 s 无数据），或自请求准入起总上传时长超过 150 s（低于浏览器端 `OTA_TOTAL_MS=180000`） |
```

并在 `GET /ota/info` 的 `other` 字段说明补充：
```markdown
- `other` 为另一个 app 槽 `esp_ota_get_partition_description` 的 `version`，在以下情形返回 `null`（控制台 `ota status` 显示 `-`）：读不到描述符、该槽 `esp_ota_img_states_t` 为 `ESP_OTA_IMG_UNDEFINED` 或状态查询失败、或本次启动期间已对该槽调用 `esp_ota_begin` 但尚未成功提交。
```

### §2.4（并发条款补充说明）

在“并发：同一时刻只允许一个上传（原子标志）”一段后补充：
```markdown
- ESP-IDF httpd 为单任务事件循环，同步流式接收期间无法同时派发第二个 HTTP 请求；第二个并发 `POST /ota` 或 `GET /ota/info` 会在当前上传结束后才由 httpd 依次受理（因此 `409 busy` 仅返回给在准入完成之后到达的请求，例如成功提交后等待重启的 1500 ms 窗口内，或关机阶段到达的请求）。配合 150 s 总上传时限（`web_ota::kTotalTimeoutMs`），即便异常并发导致第一路连接被慢滴拖慢，httpd 也在有界时间内以 408 中止并恢复服务。
```

## 4. 未解决问题 / 待 HIL 复测项

- **真机 HIL 复测未执行**（受 AGENTS.md §6 授权边界限制）：需由 Architect 在 P2 v1.41 实板上复测：
  1. 双路并发 `POST /ota` 慢滴场景：确认第一路在 ~150 s 内返回 408 `"timeout"`、调用 `esp_ota_abort()` 并释放 httpd，而不再持续 212 s 阻塞；
  2. 中止上传后执行 `ota status` 与 `GET /ota/info`：确认另一槽版本在控制台显示 `version=-`、在 `/ota/info` 返回 `"other":null`，且再次完整上传成功后恢复正常；
  3. 正常 1.52 MB 固件上传仍远低于 150 s（目标 ≤ 60 s）且不受新时限影响。
