# O2 执行报告：网页“固件升级”卡片与离线 mock

日期：2026-11-23  
分支：`feat/o2-ota-ui`（工作树 `../RTK_Main-o2`）  
依据：`docs/tasks/O2-ota-ui.md`、`docs/adr/ADR-0019-web-ota.md`（§2.3、§2.8）、`docs/adr/ADR-0017-wifi-web-config.md`（§2.4）、`AGENTS.md`

## 1. 结论

**PASS**（离线页面、打包预算与 mock 全项通过；实机联调/HIL 未在 O2 范围内，记 `NOT_RUN`）。

## 2. 改动文件

| 文件 | 改动摘要 |
| --- | --- |
| `main/web/ui/app.js` | 新增“固件升级”卡片（`id: "ota"`，位于右侧栏 `System` 卡片上方）；进入页面调用 `GET /ota/info` 展示当前版本、运行分区、状态、另一槽版本（`null` 显示“无”），失败（含旧固件 404 或门户 302 非 JSON）显示“设备不支持网页升级”且不影响其他卡片；选 `.bin` 后用 `FileReader` + `Blob.slice(0, 288)` 解析偏移 `0`（`0xE9`）、偏移 `32`（`0xABCD5432` u32 小端）、偏移 `48`（`version`，32 B `\0` 截断）、偏移 `80`（`project_name`，32 B `\0` 截断），校验工程名与大小上限；显示“当前 X → 新 Y”及同版本提示；沿用 `confirmBtn()` 二次确认（倒计时写明“将替换当前固件”）后用 `XMLHttpRequest` 发送 `POST /ota`（`Content-Type: application/octet-stream`，原始文件体）并更新进度条；上传期间禁用全页其他设置控件及本卡按钮/文件输入，并抑制 WS 断线重连与 `bye` 退出提示；按 §2.3 映射 `ok:true`、9 种 `err` 与 XHR `onerror`/`onabort` 中文提示。 |
| `main/web/ui/app.css` | 新增 `.ota-file`、`.ota-msg`、`.prog`/`.prog-bar`/`.prog-fill`/`.prog-txt` 样式，完全复用既有深/浅色主题令牌。 |
| `tools/web_mock/web_mock.py` | 按 ADR-0019 §2.3 实现 `GET /ota/info` 与 `POST /ota`；新增 CLI 参数 `--ota-err`、`--ota-slow-kbps`、`--ota-max`（另增 `--no-ota` 便于模拟旧固件 404）；校验 `Content-Length`、镜像头 magic 与 `project_name`；上传期间并发 `POST /ota` 与 WS `set`/`cmd` 返回 `busy`；上传成功后更新槽位/版本并延迟 `reboot_ms` 断开全部活跃 WS 连接模拟重启；在 `--selftest` 中补齐 OTA §2.3 全量自测用例。 |
| `docs/reports/O2-ota-ui.md` | 本执行报告。 |

固件 C/C++ 代码、`main/CMakeLists.txt`、HTTP 契约及全部冻结文件均未修改。

## 3. 页面体积预算

命令：`python3 -I tools/web_mock/pack.py`

| 指标 | 基线 | 本次 | 变化 | 预算上限 |
| --- | --- | --- | --- | --- |
| 未压缩（raw） | 40,497 B | 52,242 B | +11,745 B | — |
| gzip（level 9） | 13,704 B（55.8%） | **17,210 B（70.0%）** | +3,506 B | ≤ 24,576 B（24 KB） |

结论：**PASS**，剩余预算 7,366 B。

## 4. 验证步骤与结果

| 检查项 | 命令 / 方法 | 结果 | 说明 |
| --- | --- | --- | --- |
| Python 语法检查 | `python3 -I -m py_compile tools/web_mock/*.py` | **PASS** | 0 错误 |
| 前端打包与 gzip 预算 | `python3 -I tools/web_mock/pack.py` | **PASS** | `raw=52242 B, gzip=17210 B / 24576 B (70.0%)` |
| JS 语法检查 | `node --check main/web/ui/app.js` | **PASS** | 0 语法错误，保持 ES2017 无框架、无 `alert`/`confirm`/`innerHTML` |
| mock 自测（默认能力） | `python3 -I tools/web_mock/web_mock.py --selftest` | **PASS** | 原有 HTTP/WS/强制门户用例 + 新增 OTA §2.3 全量用例（`/ota/info`、缺/0 长度 411、超上限 413、4 类畸形头/错工程 400、全部 9 种 `--ota-err` 状态码与 JSON、慢速限速 + 并发 HTTP/WS `busy` + 模拟重启断 WS + 槽位翻转、404 降级） |
| mock 自测（CM680 能力） | `python3 -I tools/web_mock/web_mock.py --selftest --caps-cm680` | **PASS** | 同上 |
| 无头端到端 UI 验证（首轮） | Node DOM/XHR/WS 替身脚本直连真实 `web_mock.py` 执行 `main/web/ui/app.js` | **PASS**（首轮覆盖不足，已在修复轮补齐） | 首轮覆盖旧固件 404、初始 `other=null`、基础 magic/工程名/超限校验、同版本提示、二次确认文案、由测试端 `MockXHR` 分块写入驱动的 8 次 `upload.onprogress`、上传期间部分控件（System、1 个 GNSS 按钮、白名单按钮，未含 Factory 5 项）禁用与中途 WS 断开抑制、正常升级、9 种 `--ota-err` 与 XHR `onerror`；未覆盖客户端 POST 超时、重选竞态、Factory 禁用及重启断 WS 断言（详见 §7 修复轮补齐结果） |
| `curl` 契约端到端验证 | `curl` 对真实 `web_mock.py --ota-max 4096` 执行 `GET /ota/info` 与 3 组 `POST /ota` | **PASS** | `/ota/info` 200、错工程 `bad_image` 400、超大 `too_large` 413、合法固件 200 `ok:true` |
| 冻结文件校验 | `sha256sum -c --quiet docs/frozen.sha256` | **PASS** | 全部冻结文件完整未改 |
| 增量函数注释门禁 | `python3 -I tools/doc_check/doc_check.py --base $(git merge-base main HEAD)` | **PASS** | 改动 C/C++ 函数 0 个，不合格 0 个 |
| 实机烧录 / P2 HIL 联调 | — | **NOT_RUN** | O2 禁止烧录与连串口，且固件端由 O1 并行实现，待 O1+O2 合并后由 Architect 验收 |

## 5. 界面与交互说明

1. **卡片位置与初始展示**：右侧栏 `Factory` 下方、`System` 上方新增可折叠卡片“固件升级”（上传箭头 SVG 图标 + 状态徽章）。进入页面自动请求 `GET /ota/info`：
   - 成功时以 `.kv` 表格显示：当前版本、运行分区、状态（如 `已确认 (valid)` / `待确认 (pending_verify)`）、另一槽版本（`null` 显示“无”）。
   - 失败（如旧固件 404 或强制门户 302 重定向返回 HTML）时隐藏表单，仅显示“设备不支持网页升级”，徽章为“不支持”，不影响其他卡片与 WS 配置功能。
2. **文件选择与浏览器端校验**：
   - 文件选择框限定 `accept=".bin"`，选文件后使用 `FileReader.readAsArrayBuffer(file.slice(0, 288))` 解析镜像头。
   - 若文件长度 ≤ 288 B、偏移 0 ≠ `0xE9`、偏移 32 u32 LE ≠ `0xABCD5432` 或偏移 80 `project_name` ≠ `/ota/info.project`，提示红色“不是本设备固件”并保持上传按钮禁用；若超过 `max`，提示“文件过大（X，上限 Y）”并禁用（两者同时不满足时一并提示）。
   - 校验通过后显示“当前 X → 新 Y（文件大小）”，若 `Y === X` 额外显示黄色“版本相同”。
3. **二次确认与上传进度**：
   - 点击“上传并升级固件”后按钮进入 3 秒倒计时“再次点击确认：将替换当前固件 (3)”，期间若重选文件会自动取消确认态；再次点击即以 `XMLHttpRequest` 发送 `POST /ota`（`Content-Type: application/octet-stream`，直接发送 `File` 对象）。
   - `upload.onprogress` 实时驱动进度条与百分比/字节数（达 100% 后显示“100% · 正在校验…”）。
   - 上传期间全页其他设置控件与本卡输入/按钮全部禁用；WS 若在此期间断开或收到 `bye`，静默显示状态而不弹出“退出”或“连接断开”横幅。
4. **结果反馈**：
   - `ok: true`：进度条置 100%，在卡片、顶部横幅和 Toast 显示“升级完成，设备将在约 N 秒后重启进入蓝牙模式，热点会断开”，随后模拟重启断开 WS 不再触发重连或掉线警告。
   - `ok: false`：按 `err` 显示中文原因并恢复全部控件；若上传期间 WS 曾断开则自动重连 WS。
   - 网络错误（`onerror`/`onabort`）：显示“连接中断，设备仍运行原固件”并恢复控件。

## 6. 偏离与未决问题

- **与 ADR-0019 §2.8 措辞的细化对齐**：ADR-0019 §2.8 早期文字提及“降级时加醒目提示”，任务卡 `docs/tasks/O2-ota-ui.md` §1.1 已明确细化为“Y 与 X 相同提示‘版本相同’，字符串比较无法判断新旧，不做降级判断，只在确认框里写明‘将替换当前固件’”。本次严格按任务卡执行，未做字符串大小比较。
- **HTTP 契约问题**：未发现 ADR-0019 §2.3 契约本身存在矛盾；首轮实现中存在的 408 真实接收停顿、ESP32 `chip_id` 校验、重启窗口切换时机、WS 并发竞争与字符串边界差异已按 `docs/reports/O2-review.md` 在 §7 修复轮逐项修正并复测。
- **未决 / 待后续阶段验证**：真机（P2 v1.41）与 O1 固件端合并后的实际升级耗时、真实浏览器（桌面与 Safari/Android 强制门户小窗）中的布局与 `<input type="file">` / 真实 `XMLHttpRequest.upload.onprogress` 行为，需在合并后的实机/HIL 验收中实测确认。

## 7. 修复轮（对应 `docs/reports/O2-review.md`）

针对独立评审结论 `CHANGES_REQUIRED` 的全部 10 条发现（6 major + 4 minor），均已认可并逐项修复（无异议跳过项），未修改固件 C/C++ 代码、`O2-review.md` 或任何冻结文件。

### 7.1 逐条处理与改动

| # | 级别 | 评审发现 | 处理与代码改动 | 复测结果 |
| --- | --- | --- | --- | --- |
| 1 | major | mock 真实接收停顿不返回 `408 timeout`，拒绝分支排空可阻塞 `busy` | `tools/web_mock/web_mock.py`：新增 `_read_body_chunk()`（基于 `rfile.read1` 与按次重置的 socket 空闲超时，默认 `OTA_RECV_TIMEOUT_S = 30.0` s，超时回 `408 {"ok":false,"err":"timeout"}` 并关闭连接）；`_drain_small_body()` 增加短时限（`OTA_DRAIN_TIMEOUT_S = 0.25` s），超时立即放弃排空并置 `Connection: close`；拒绝分支 `fail()` 先释放 `ota_busy` 再限时排空与回包，避免未发完的大请求拖住并发合法上传。 | **PASS**：`--selftest` 新增真实 288 B 后停传 408 用例（验证停顿期 `busy:true`、超时回 408 JSON 并释放 `busy`）及 1 B 超大请求 Prompt 413 用例。 |
| 2 | major | 前端 POST 无客户端超时，失败时进度栏可停留在“100% · 正在校验…” | `main/web/ui/app.js`：`startUpload()` 设置 `xhr.timeout = 180000`（3 min）并绑定 `xhr.ontimeout`，同时增加随 `e.loaded` 前进续期的 45 s 停顿看门狗（覆盖设备 30 s 接收超时 + 校验余量），统一经 `settled` 保护中止并调用 `finishUpload(false, "上传超时，未收到设备应答，请确认设备状态后重试")`；`finishUpload(false)` 隐藏并重置 `.prog` 进度条，解锁全页控件并在 WS 断开时自动重连。 | **PASS**：无头脚本 Case 5/6/7a/7b 验证全部 9 种错误、`onerror`、45 s 看门狗超时及 `xhr.ontimeout` 均隐藏进度栏、解锁控件并恢复 WS 重连。 |
| 3 | major | mock 漏校验 ESP32 芯片 ID（偏移 12 u16 LE == `0x0000`） | `tools/web_mock/web_mock.py`：`parse_esp_app_header()` 按小端读取偏移 12 的 16 位 `chip_id` 并校验 `== OTA_CHIP_ID_ESP32 (0x0000)`；`build_mock_firmware()` 增加 `chip_id` 参数。 | **PASS**：`--selftest` 新增其余字段全合法但 `chip_id = 0x0009`（ESP32-S3）用例，断言精确返回 `400 {"ok":false,"err":"bad_image"}`。 |
| 4 | major | 先前排队的 WS 操作可跨入上传阶段 | `main/web/ui/app.js`：新增 `wsPending()`（`!!(inflight \|\| queue.length)`），在 `W.ota` 刷新中当 `wsPending()` 为真时禁用上传按钮并复位确认态；`startUpload()` 再次检查 `wsPending()`；`pump()` 增加 `otaUploading \|\| otaDone \|\| leaving` 守卫；`command()` 回调与 `W.stepper` 的防抖 `commit()` 亦在上传/完成期保护，防止迟到 ACK 触发重启提示或发出排队帧。 | **PASS**：无头脚本 Case 3 验证 `exit` 命令与 `gnss.rate` 设置在途期间上传按钮禁用，直至 ACK 超时排空后恢复且不弹重启横幅。 |
| 5 | major | mock 在重启前就切换运行版本，重启窗口允许二次翻槽且发送 200 失败无保护 | `tools/web_mock/web_mock.py`：`MockState` 新增 `ota_rebooting` / `ota_pending_ver` / `ota_pending_part` / `complete_ota_reboot()`；200 发送前仅记录待启动版本与分区，保持 `ota_busy = True` 并将所有权移交给重启定时器，发送 200 捕获 `OSError` 且在 `finally` 中必启动定时器；仅在唯一一次 `complete_ota_reboot()` 回调中原子切换 `running`/`other`/`part`、清 `busy` 并断开全部 WS。 | **PASS**：`--selftest` 用双客户端核对 200 后重启前 `running`/`other`/`part` 未变且 `busy:true`、第二次 POST 返回 `409 busy`、重启后原子切换且 `busy:false`，并覆盖客户端提前断连时仍完成重启。 |
| 6 | major | mock 的 WS busy 检查与设置更新之间有竞争 | `tools/web_mock/web_mock.py`：重构 `_on_text()` 与 `_handle_set()`，将 `cmd`（`save`/`exit`/`freset`）和全部 `set` 分支的 `ota_busy \|\| ota_rebooting` / `force_err` / `caps()` 检查与状态修改合并进同一次 `with self.state.lock:`，所有 `ws_send_json()` 与 `_send_status()` 均移至锁外。 | **PASS**：`--selftest` 验证上传期并发 WS `set` 与 `cmd`（含 `exit`）均返回 `err:"busy"` 且配置未被修改。 |
| 7 | minor | 前端与 mock 对文件头字符串（BOM、非法 UTF-8、空版本、32 B 无 NUL）解释不一致 | `main/web/ui/app.js`：新增 `readCBytes()` / `decodeUtf8()`（`TextDecoder("utf-8", { ignoreBOM: true })` 及一致替换 `\uFFFD` 的纯 JS 回退）/ `strToUtf8Bytes()` / `bytesEq()`，按有界 NUL 截断原始字节精确比较 `project_name`；`tools/web_mock/web_mock.py`：`parse_esp_app_header()` 返回 `(version, proj_bytes)` 并以原始字节精确比对 `ota_project.encode("utf-8")`；空版本时 `complete_ota_reboot()` 保留 `""` 使 POST `"ver":""` 与重启后 GET `"running":""` 一致。 | **PASS**：`--selftest` 与无头脚本 Case 2 覆盖 287 B / 288 B、BOM+`rtk_main`、非法 UTF-8 `0xFF`（含禁用 `TextDecoder` 回退分支）、32 B 无 NUL 字段及空版本一致性。 |
| 8 | minor | 无头脚本等待无超时/挂起风险、`--selftest` 将 `TimeoutError` 误判为 WS 断开、报告夸大覆盖范围 | 重写无头脚本（已在第二修复轮正式纳入仓库 `tools/web_mock/e2e_ota_ui.mjs`，改为相对路径解析）：增加 60 s 全局看门狗（超时打印当前用例与子进程状态并 `SIGKILL` 非零退出）、`startMock`/`stopMock`（检查 `exitCode` 与 `signalCode` 及进程错误）与 `MockXHR` 网络超时；补齐全部 5 个 Factory 控件禁用、超时解锁及重启后真实 WS 断开断言；`web_mock.py` 自测中将 `TimeoutError` 显式判失败，仅接受 EOF/关闭帧（第二修复轮进一步改为绝对截止时间）；本报告同步修正首轮夸大表述。 | **PASS**：无头脚本实测全 8 组用例通过（含 Factory 5 控件与重启 WS 断开），耗时约 12 s；`--selftest` 双配置通过。 |
| 9 | minor | 重选文件后读取完成前上传按钮仍可按 | `main/web/ui/app.js`：`fileInp` 的 `change` 监听在清空 `selectedFile`/`fileValid` 并复位 `uploadBtn` 后立即调用 `refresh()`，在异步 `FileReader` 完成校验前保持按钮禁用。 | **PASS**：无头脚本 Case 2f 在延迟 100 ms `FileReader` 期间立即断言 `uploadBtn.disabled === true` 且点击无法进入倒计时，校验通过后恢复可用。 |
| 10 | minor | 宽屏提示横幅靠左与卡片区错位 | `main/web/ui/app.css`：`.banner` 移除覆盖性的 `margin-left:16px;margin-right:16px`，改为 `width:calc(100% - 32px);max-width:1088px;margin:12px auto 0`（窄屏保留 16 px 边距，宽屏居中并与 `.grid` 内 1088 px 卡片内容区左右严格对齐，断线与升级成功两条横幅共用）。 | **PASS**：无头脚本 Case 8 断言 `.banner` 规则，按盒模型计算 375 px（左 16 px / 宽 343 px）与 1440 px（左 176 px / 宽 1088 px）均与卡片区 0 px 误差对齐；实机/真实浏览器视觉渲染记 `NOT_RUN`。 |

### 7.2 修复轮页面体积预算

命令：`timeout 180 python3 -I tools/web_mock/pack.py`

| 指标 | 基线（`feat/ota-base`） | 首轮 | 修复轮 | 预算上限 |
| --- | --- | --- | --- | --- |
| 未压缩（raw） | 40,497 B | 52,242 B | **56,013 B** | — |
| gzip（level 9） | 13,704 B（55.8%） | 17,210 B（70.0%） | **18,152 B（73.9%）** | ≤ 24,576 B（24 KB） |

结论：**PASS**，剩余预算 **6,424 B**。

### 7.3 修复轮复测命令与结果

所有命令均使用 `timeout`（≤ 180 s）执行：

| 检查项 | 命令 | 结果 | 说明 |
| --- | --- | --- | --- |
| Python 语法检查 | `timeout 180 python3 -I -m py_compile tools/web_mock/*.py` | **PASS** | 退出 0 |
| 前端打包与 gzip 预算 | `timeout 180 python3 -I tools/web_mock/pack.py` | **PASS** | `raw=56013 B, gzip=18152 B / 24576 B (73.9%)` |
| JS 语法检查 | `timeout 180 ~/.nvm/versions/node/v24.21.0/bin/node --check main/web/ui/app.js` | **PASS** | 退出 0，仍为离线无框架 ES2017，无 `alert`/`confirm`/`innerHTML` |
| mock 自测（默认能力） | `timeout 180 python3 -I tools/web_mock/web_mock.py --selftest` | **PASS** | 退出 0，打印 `SELFTEST PASS`（含 408 停传、1 B 超大 413、错芯片 ID 400、BOM/非法 UTF-8/32 B 无 NUL/空版本、重启窗口 busy 与原子翻槽、EOF/关闭帧 WS 断开） |
| mock 自测（CM680 能力） | `timeout 180 python3 -I tools/web_mock/web_mock.py --selftest --caps-cm680` | **PASS** | 退出 0，打印 `SELFTEST PASS` |
| 加固无头 UI 端到端脚本 | `timeout 120 node tools/web_mock/e2e_ota_ui.mjs`（原位于 scratch 目录，第二修复轮已纳入仓库） | **PASS** | 退出 0，打印 `ALL E2E UI TESTS PASSED`（8 组用例全过；注：该脚本使用 Node DOM/XHR 替身，进度事件由 `MockXHR` 分块写入触发） |
| 冻结文件校验 | `timeout 60 sha256sum -c --quiet docs/frozen.sha256` | **PASS** | 退出 0 |
| 增量函数注释门禁 | `timeout 60 python3 -I tools/doc_check/doc_check.py --base feat/ota-base` | **PASS** | 改动 C/C++ 函数 0 个，不合格 0 个 |
| 空白与补丁检查 | `timeout 60 git diff --check feat/ota-base` | **PASS** | 退出 0 |
| 实体浏览器渲染 / P2 HIL | — | **NOT_RUN** | O2 范围外，待 O1+O2 合并后验收 |

## 第二修复轮

针对 `docs/reports/O2-review.md` 末尾“## 复审”的全部 3 条新发现（均为 minor）及无头 UI 脚本入库要求，逐项完成修复并复测。未修改固件 C/C++ 代码、`docs/reports/O2-review.md` 或任何冻结文件，未执行 `git commit`。

### 逐条修复记录

| # | 复审发现 | 代码改动 | 复测结果 |
| --- | --- | --- | --- |
| 1 | `app.js` UTF-8 回退解码将截断多字节序列（如尾部 `E2 82`）输出为多个 `U+FFFD`，与原生 `TextDecoder` 和 Python mock 不一致 | `main/web/ui/app.js`：重写 `decodeUtf8()` 回退分支为 WHATWG 最大合法前缀算法（按首字节范围确定续字节数与首续字节上下界 `E0:[A0,BF]`、`ED:[80,9F]`、`F0:[90,BF]`、`F4:[80,8F]`；遇 EOF 或非续字节时消耗已确认合法的前缀并仅输出单个 `U+FFFD`）。在 `tools/web_mock/e2e_ota_ui.mjs` Case 2c 补齐 10 组向量（含尾部 `E2 82`、`E2 82 41`、截断 4 字节 `F0 9F 98`、`F0 9F 2E 31`、`F0 9F E2 82 AC`、过长编码、代理区、超范围与 BOM+合法多字节）并精确断言原生与回退一致；在 `tools/web_mock/web_mock.py` `--selftest` 同步补齐截断多字节向量。 | **PASS**：`e2e_ota_ui.mjs` Case 2c 与 `--selftest` 全过；额外对 538 组边界/随机字节串交叉比对 Python `decode("utf-8","replace")`、Node 原生 `TextDecoder` 与 `app.js` 回退分支，结果 100% 一致。 |
| 2 | `web_mock.py` 自测的 WS 重启断开断言仅设置单次 socket 超时，持续收到状态帧时会避开超时而无限等待；且旧复审路径仍指向外部 scratch 脚本 | `tools/web_mock/web_mock.py`：为 `_WsClient._read_exact()` / `recv_next()` 增加绝对截止时间参数 `deadline`，新增 `_wait_ws_closed(client, timeout_s)` 在 `time.monotonic() + timeout_s` 绝对期限内等待关闭帧（`0x8`）或 EOF/连接关闭，超期一律返回 `False`；自测中新增 `_SpamWsClient` 回归用例（每 40 ms 持续返回状态帧），断言在 0.18 s 绝对期限内判失败。 | **PASS**：`--selftest` 及 `--selftest --caps-cm680` 中重启 WS 断开与持续状态帧超时回归断言均通过。 |
| 3 | `web_mock.py` WS 注册进 `ws_clients` 后的初始 `hello`/`config`/`status` 发送位于 `try/finally` 之外，初始发送异常会遗留 `fileno() == -1` 的 socket；`finally` 亦无条件 `join` ticker 线程 | `tools/web_mock/web_mock.py`：将 `_handle_ws()` 中 `ws_clients.add(sock)` 之后的初始发送（`hello`/`config`/`_send_status`）、`ticker` 启动及帧循环整体纳入同一 `try/finally`，并捕获初始/帧发送 `OSError` 优雅返回；仅在 `th.start()` 成功后赋值 `t_thread`，`finally` 始终执行 `ws_clients.discard(sock)` 且仅对已启动的 `t_thread` 调用 `join(timeout=0.5)`。在 `--selftest` 中新增初始 `_send_status`（`tick == 0`）注入 `BrokenPipeError` 回归用例，断言连接关闭且 `state.ws_clients` 清空无残留。 | **PASS**：`--selftest` 及 `--selftest --caps-cm680` 中初始发送 `BrokenPipeError` 清理用例通过，`ws_clients` 无残留。 |
| 4 | 加固无头 UI 脚本需纳入仓库、消除绝对路径硬编码、统一报告与复审命令 | 新增 `tools/web_mock/e2e_ota_ui.mjs`：通过 `fileURLToPath(import.meta.url)` 相对脚本自身位置解析 `REPO`、`main/web/ui/app.js`、`main/web/ui/app.css` 与 `tools/web_mock/web_mock.py`（零硬编码绝对路径）；保留 60 s 全局看门狗及启动/停止/网络/等待上限，任意失败或超时均清理子进程并以非零码退出；复审与报告统一使用 `timeout 120 node tools/web_mock/e2e_ota_ui.mjs`。 | **PASS**：在 `RTK_main` 及仓库根目录分别执行均退出 0 并打印 `ALL E2E UI TESTS PASSED`。 |

### 第二修复轮页面体积预算

命令：`timeout 60 python3 -I tools/web_mock/pack.py`

| 指标 | 基线（`feat/ota-base`） | 首轮 | 第一修复轮 | 第二修复轮 | 预算上限 |
| --- | --- | --- | --- | --- | --- |
| 未压缩（raw） | 40,497 B | 52,242 B | 56,013 B | **55,793 B** | — |
| gzip（level 9） | 13,704 B（55.8%） | 17,210 B（70.0%） | 18,152 B（73.9%） | **18,200 B（74.1%）** | ≤ 24,576 B（24 KB） |

结论：**PASS**，剩余预算 **6,376 B**。

### 第二修复轮验证命令与结果

以下验证均在 `RTK_main` 目录下执行，单条命令均使用 `timeout`（≤ 180 s）；本机 Node 二进制路径为 `~/.nvm/versions/node/v24.21.0/bin/node`：

| 检查项 | 命令 | 结果 | 说明 |
| --- | --- | --- | --- |
| Python 语法检查 | `timeout 60 python3 -I -m py_compile tools/web_mock/web_mock.py tools/web_mock/pack.py` | **PASS** | 退出 0 |
| JS 语法检查 | `timeout 60 node --check main/web/ui/app.js && timeout 60 node --check tools/web_mock/e2e_ota_ui.mjs` | **PASS** | 退出 0 |
| 前端打包与 gzip 预算 | `timeout 60 python3 -I tools/web_mock/pack.py` | **PASS** | `raw=55793 B, gzip=18200 B / 24576 B (74.1%)` |
| mock 自测（默认能力） | `timeout 120 python3 -I tools/web_mock/web_mock.py --selftest` | **PASS** | 退出 0，`SELFTEST PASS`（含截断 UTF-8、WS 绝对截止时间、初始发送 `BrokenPipeError` 清理） |
| mock 自测（CM680 能力） | `timeout 120 python3 -I tools/web_mock/web_mock.py --selftest --caps-cm680` | **PASS** | 退出 0，`SELFTEST PASS` |
| 仓库内无头 UI 端到端脚本 | `timeout 120 node tools/web_mock/e2e_ota_ui.mjs` | **PASS** | 退出 0，`ALL E2E UI TESTS PASSED`（8 组用例，含 10 组 UTF-8 原生/回退精确一致向量） |
| UTF-8 三方一致 fuzz 核对 | 538 组边界与随机字节串比对 Python / `TextDecoder` / `app.js` 回退 | **PASS** | `UTF8 FUZZ PASS: 538 vectors identical` |
| 冻结文件校验 | `timeout 60 sha256sum -c --quiet docs/frozen.sha256` | **PASS** | 退出 0 |
| 增量函数注释门禁 | `timeout 60 python3 -I tools/doc_check/doc_check.py --base feat/ota-base` | **PASS** | 改动 C/C++ 函数 0 个，不合格 0 个 |
| 空白与补丁检查 | `timeout 60 git diff --check feat/ota-base` | **PASS** | 退出 0 |
| 实体浏览器渲染 / P2 HIL | — | **NOT_RUN** | O2 范围外，待 O1+O2 合并后验收 |
