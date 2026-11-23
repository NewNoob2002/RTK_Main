# O2 独立评审

以 `feat/ota-base` 为基线，对 `feat/o2-ota-ui` 工作树执行 `git diff feat/ota-base` 和 `git status`。交付包括 `app.js`、`app.css`、`web_mock.py` 及未跟踪的执行报告。范围符合任务卡。评审核对了项目 `AGENTS.md`、`docs/CODING-STYLE.md`、O2 任务卡和 ADR-0019，覆盖 ESP-IDF 网页的浏览器端与 Python 主机 mock。仓库内只写入本评审报告，未提交。

## 发现

1. **major** · mock 在真实接收停顿后不返回 `408 timeout`（`tools/web_mock/web_mock.py:337`、`tools/web_mock/web_mock.py:444`、`tools/web_mock/web_mock.py:475`）
   - **问题与失败场景**　ADR-0019 §2.3 要求连续无数据达到 30 秒返回 `408 {"ok":false,"err":"timeout"}`。实际只发送合法的前 288 B 后停传 31.5 秒，连接未收到回复，`GET /ota/info` 仍为 `busy:true`，第二个完整 POST 收到 409。关闭首个客户端后才解除 busy。拒绝分支也会阻塞排空小请求体。实测一个声明长度超过 `max` 的请求只发送 1 B，413 被延后，期间合法上传收到 busy。`--ota-err timeout` 在完整读完请求体后注入错误，现有自测没有覆盖真实停传。
   - **修复建议**　读取时按最近一次收到字节的时间执行 30 秒无进展期限，超时回标准 408 并释放 busy。排空拒绝请求也须有短时限，无法及时排空就发原错误并关闭连接。分别补实际停传和过大请求未发完的用例。

2. **major** · POST 无客户端超时，页面可一直锁定（`main/web/ui/app.js:634`、`main/web/ui/app.js:663`、`main/web/ui/app.js:685`）
   - **问题与失败场景**　上传 XHR 的 `timeout` 保持默认 0，且没有 `ontimeout`。用原页面的 DOM/XHR 替身保持 POST 未完成，实测所有设置持续锁住。若此时 WS 已断开，重连也会一直被抑制。另有明确失败应答时，`finishUpload(false)` 仍可能把进度栏留在“100% · 正在校验…”。
   - **修复建议**　给 POST 设置覆盖最大镜像和合理慢速上传的客户端期限，或用随进展续期的看门狗。限制上下限时应给设备 30 秒接收超时和最终校验留余量。超时走统一解锁、必要时恢复 WS 重连，并结束进度提示。客户端未收到应答时提示确认设备状态。

3. **major** · mock 漏校验 ESP32 芯片 ID（`tools/web_mock/web_mock.py:65`、`tools/web_mock/web_mock.py:491`）
   - **问题与失败场景**　ADR-0019 §2.3 的 `400 bad_image` 引用 §2.2 第 1 条，其中要求 `chip_id == ESP_CHIP_ID_ESP32`。按本地 IDF v6.1 的 `esp_app_format.h:20`、`:82`、`:93`，芯片 ID 为镜像偏移 12 的 16 位字段，ESP32 为 0。送入两个 magic 和工程名均匹配、芯片 ID 为 ESP32-S3（`0x0009`）的测试镜像，mock 实际返回 `200 {"ok":true,…}` 并更新槽位。
   - **修复建议**　在 mock 按小端读取偏移 12 并检查 ESP32 ID。补一个其他字段均合法的错芯片用例，断言精确的 400 JSON。

4. **major** · 先前排队的 WS 操作可跨入上传阶段（`main/web/ui/app.js:456`、`main/web/ui/app.js:653`、`main/web/ui/app.js:790`、`main/web/ui/app.js:863`）
   - **问题与失败场景**　页面锁住控件并拦截新 WS 请求，已发送与已排队的请求仍会继续。复现时先发退出命令，在 ACK 到达前启动上传，再送达成功 ACK。上传尚未完成，页面已显示 `The receiver is restarting. You can close this page.`，同时置 `leaving=true`。单独抑制 `bye` 无法拦住这条回调。普通设置队列也会在 ACK 或超时后继续发送。
   - **修复建议**　在 WS 队列和在途请求排空前禁用 OTA 确认，并在开始上传时再检查一次。给上传期间的 `pump()` 和退出回调保留保护，避免后到的回调触发重启提示。补延迟 ACK 回归用例。

5. **major** · mock 在重启前就切换运行版本，允许再次覆盖待启动镜像（`tools/web_mock/web_mock.py:506`、`tools/web_mock/web_mock.py:512`）
   - **问题与失败场景**　200 的含义是新镜像已设为启动分区，当前运行版本应在重启时才变化。实测首个 200 后立刻查询，`running` 和 `part` 已切到新槽，`busy:false`。在整个 15 ms 测试窗口内，第二个契约合规 POST 又成功翻槽，导致 mock 失去对原运行槽的正确模拟。若发送首个 200 时抛出异常，同一代码顺序会留下已切换的状态且没有安排重启。
   - **修复建议**　先记录待启动版本和分区，把重启窗口纳入 busy。在唯一一次重启回调中原子切换运行快照并断开 WS。镜像完成后安排重启的路径应处理响应发送失败。用两个客户端核对重启前后的 `running`、`other`、`part` 和第二次上传结果。

6. **major** · mock 的 WS busy 检查和设置更新之间有竞争（`tools/web_mock/web_mock.py:700`、`tools/web_mock/web_mock.py:761`、`tools/web_mock/web_mock.py:790`）
   - **问题与失败场景**　WS 先持锁读 `ota_busy`，释放锁后才进入设置或命令分支。用线程屏障把 `gnss.rate=5` 暂停在 busy 检查之后，置 `ota_busy=true` 模拟 POST 已取得上传权，再放行。实测配置被改为 5，WS 回 `{"t":"ack","id":7,"ok":true}`。退出和恢复出厂分支也使用同一个已过期的检查结果。ADR-0019 §2.4 要求上传期间 WS `set` 和 `cmd` 回 busy。
   - **修复建议**　让接受设置或命令的状态决定与 OTA 抢占互斥。设置应在更新状态的锁内做最终 busy 检查，命令应在同一互斥边界内确定是否接受，网络发送留在锁外。

7. **minor** · 前端和 mock 对文件头字符串有不同解释（`main/web/ui/app.js:139`、`main/web/ui/app.js:613`、`tools/web_mock/web_mock.py:72`、`tools/web_mock/web_mock.py:508`）
   - **问题与失败场景**　浏览器的默认 `TextDecoder` 丢弃字段开头的 UTF-8 BOM。工程名为 BOM 加 `rtk_main` 的文件通过前端工程名比较，mock 返回 `400 bad_image`。版本首字节为无效 UTF-8 的 `0xff` 时，浏览器原生解码得到替代符 `�`，回退分支得到 `ÿ`。空版本又会让 mock 成功应答中的 `ver` 为空，随后查询的 `running` 保留旧版本。
   - **修复建议**　按有界的 NUL 截断字段精确比较工程名字节，版本显示和 mock 使用一致的 UTF-8 策略，约定空版本的处理后让 POST 与 GET 返回一致。加入短文件、BOM、无效 UTF-8 和 32 B 无 NUL 字段的向量。实测 287 B 的头返回空解析结果，288 B 整文件被前端和 mock 拒绝，32 B 无 NUL 的版本也只读取到本字段边界。

8. **minor** · AGY 脚本的进程等待无期限，执行报告也扩大了覆盖范围（`docs/reports/O2-ota-ui.md:42`、`e2e_ota_ui.mjs:135`、`e2e_ota_ui.mjs:158`、`e2e_ota_ui.mjs:240`、`e2e_ota_ui.mjs:403`）
   - **问题与失败场景**　原样脚本本次四次通过。独立调用其 `stopMock` 验证了一个等待缺陷。若子进程在调用前已被 SIGTERM 终止，`exitCode` 仍为 `null`、`signalCode` 为 `SIGTERM`，新的 `exit` 监听无法完成 Promise，300 ms 检查仍未解决。若子进程一直存活却不输出启动地址，`startMock` 也没有截止时间。当前缺少此前 51 分钟运行的日志，无法确定当时卡在哪一步。脚本用假 DOM/XHR，进度事件来自测试端主动分块写入，上传禁用断言检查了 System、一个 GNSS 按钮和白名单按钮，未检查 Factory。脚本仅触发网络 `onerror` 和 HTTP 注入错误，未模拟 POST XHR 超时。mock 自测把 `TimeoutError` 当作 `OSError`，可能把未收到数据计为 WS 已断开（`tools/web_mock/web_mock.py:1406`）。
   - **修复建议**　给启动、退出和网络等待设置上限，退出时同时处理 `exitCode`、`signalCode` 和进程创建错误，超时时打印当前用例和子进程状态。WS 重启断开用例应等待 EOF 或关闭帧，明确将等待超时判失败。补 Factory、真实超时和重启后断开断言，再按实测范围更新执行报告。

9. **minor** · 重选文件后，读取完成前上传按钮仍可按（`main/web/ui/app.js:591`、`main/web/ui/app.js:629`）
   - **问题与失败场景**　选择过合法文件后再选择另一个文件，事件处理会清空 `fileValid`，却没有立即调用 `refresh()`。延迟 FileReader 回调时，实测旧的上传按钮仍可进入二次确认倒计时。最终发送检查仍生效，但新文件的验证结果可能在一个已经启动的确认倒计时里出现。
   - **修复建议**　清除原选择和确认状态后立即刷新禁用状态，仅读取并校验成功后恢复按钮。

10. **minor** · 宽屏提示横幅靠左，边缘与卡片区错位（`main/web/ui/app.css:43`、`main/web/ui/app.css:46`、`main/web/ui/app.js:642`、`main/web/ui/app.js:955`）
    - **问题与失败场景**　`.banner` 的 `margin-left:16px` 和 `margin-right:16px` 覆盖了 `margin` 中的左右 `auto`。`.grid` 则继续居中。宽屏断线截图显示横幅贴近视口左侧，卡片区仍居中。按 CSS 计算，1440 px 可用宽度时，横幅左边缘为 16 px，卡片区左边缘为 176 px。该样式在 `feat/ota-base` 中已存在，O2 新增的升级成功提示也复用了同一横幅。
    - **修复建议**　在横幅上保留 `margin:12px auto 0` 并移除左右固定外边距，使用 `width:calc(100% - 32px)` 和 `max-width:1088px` 对齐卡片内容区。核对窄屏的 16 px 边距及宽屏下断线、升级成功两种提示的左右边缘。

## 逐项核对

| O2 任务卡 | 评审结果 |
| --- | --- |
| 第 11–12 行，卡片位置、四项信息、旧固件降级 | 通过。卡片在 System 上方，进入页面发 GET，`other:null` 显示“无”。404 后旧配置卡片仍可用。 |
| 第 13–15 行，选择及校验文件 | `.bin` 过滤、前 288 B、偏移 0/32/48/80、u32 小端、短文件和超限基本路径符合要求。工程名的字节比较差异见发现 7，重选过程见发现 9。 |
| 第 16–17 行，版本、二次确认及上传 | 显示“当前 → 新”和“版本相同”，确认文案提示替换当前固件。按任务卡允许降级。XHR 发原始文件，设置 `application/octet-stream`，慢速用例观察到 8 次进度事件。 |
| 第 18 行，禁止操作及 WS 断开 | AGY 脚本验证上传中 WS 断开不弹掉线提示，并检查部分控制项。补充替身用例验证 Factory 的 5 个输入和按钮全部禁用。已有退出 ACK 会绕过提示抑制，见发现 4。 |
| 第 19–21 行，结果与错误 | 成功提示按 `reboot_ms` 显示。九种 HTTP 错误和 XHR 网络错误的中文反馈通过脚本用例，客户端停顿处理见发现 2。 |
| 第 22–23 行，mock 与故障注入 | CLI 三个选项有效。常规响应为紧凑 UTF-8 JSON，`Content-Type: application/json`。GET 七个字段及成功 POST 的 `ok`、`ver`、`reboot_ms` 均核对。错误映射为 409 `busy/state`、411 `length`、413 `too_large`、400 `bad_image/validate`、408 `timeout`、500 `flash`、503 `aborted`。真实超时、芯片校验、重启与并发语义见发现 1、3、5、6。mock 也接受 `text/plain` 的原始镜像，§2.3 规定了客户端媒体类型，未定义错类型的错误应答。 |
| 第 24 行及第 28 行，体积、范围与冻结文件 | 17,210 B gzip，限额 24,576 B，余 7,366 B。CSS 沿用现有主题令牌，`[hidden]` 可隐藏新控件（`main/web/ui/app.css:22`、`main/web/ui/app.css:141`）。未改固件、CMake 或冻结契约。 |
| 第 32–34 行，测试与执行报告 | 必跑命令均已独立复跑。执行报告的 gzip、两个自测和 AGY 脚本 PASS 可复现。报告第 69 行的“字节级契约完全实现”与上述行为偏差不符。 |

## 复跑记录

| 检查 | 结果 |
| --- | --- |
| `python3 -I tools/web_mock/pack.py --out <临时路径>/index.html.gz` | **PASS**，退出 0。`raw=52242 B, gzip=17210 B / 24576 B (70.0%)`。输出重定向到仓库外的临时路径，验证后已删除。 |
| `python3 -I tools/web_mock/web_mock.py --selftest` | **PASS**，退出 0，打印 `SELFTEST PASS`。 |
| `python3 -I tools/web_mock/web_mock.py --selftest --caps-cm680` | **PASS**，退出 0，打印 `SELFTEST PASS`。 |
| `timeout 120 ~/.nvm/versions/node/v24.21.0/bin/node <AGY 脚本绝对路径>` | **PASS**，原文件未改，四次均退出 0 并打印 `ALL E2E UI TESTS PASSED`。后三次分别约 2.34、2.22、2.31 秒。脚本位于 `/home/gtc/.gemini/antigravity-cli/brain/2d90de6e-e6fe-4cd1-b547-e33deadde8e9/scratch/e2e_ota_ui.mjs`。 |
| `curl` 对实际 mock 的 `/ota/info` 与 `/ota`（`max=4096`） | **PASS**，复现 GET 200、错工程 400、超限 413、合法镜像 200，并检查响应类型和正文。补充用例复现发现 5。 |
| Python 源码与 JS 检查 | **PASS**，Python 两个脚本在内存中 `compile()`，避免生成 `__pycache__`。Node `--check` 与 Acorn `ecmaVersion: 2017` 均通过，确认无 `innerHTML`、`alert()`、`confirm()`。两处 `replaceChildren()` 是基线已有的 DOM API。 |
| `sha256sum -c --quiet docs/frozen.sha256`、`git diff --check feat/ota-base`、`doc_check.py --base feat/ota-base` | **PASS**，均退出 0，改动 C/C++ 函数 0 个。 |
| 实体浏览器及 P2 HIL | **NOT_RUN**，本次为只读评审，UI 证据来自 AGY 脚本和补充的 Node 替身测试。 |

沙箱内首轮联网用例在创建本地 TCP socket 时均因 `EPERM` 退出。随后通过自动审批在 `127.0.0.1` 临时端口复跑，表中的结论采用复跑结果。所有运行命令均设 timeout。

## 总结论

**CHANGES_REQUIRED**

## 复审

对 `feat/o2-ota-ui` 工作树执行 `git status` 与 `git diff feat/ota-base`，核对 O2 任务卡、ADR-0019 及修复轮报告。改动范围为 ESP-IDF 网页资源和 Python 主机 mock。测试使用本地回环服务，覆盖单页面操作和并发客户端。

### 上轮 10 条核实

| # | 结果 | 代码与实测 |
| --- | --- | --- |
| 1 | **已修复** | `tools/web_mock/web_mock.py:366`、`:385`、`:524`、`:557` 使用读空闲时限，拒绝分支先释放 busy，再限时排空。同一真实 TCP 用例发送合法头 288 B 后停传，并发 POST 得 409，原请求在 **30.023 s** 收到 `408 {"ok":false,"err":"timeout"}`，随后 busy 解除，运行槽保持原值。另一个只发送 1 B 的超限请求在 **0.252 s** 收到精确 413 JSON。 |
| 2 | **已修复** | `main/web/ui/app.js:694`、`:727`、`:773` 设置 180 s XHR 上限和随已发送字节增长续期的 45 s 看门狗。加固 E2E 分别触发缩时看门狗和 XHR `ontimeout`，均隐藏进度、显示确认设备状态提示并解锁，途中断开的 WS 会重连。九种 HTTP 失败也清除进度。 |
| 3 | **已修复** | `tools/web_mock/web_mock.py:68` 读取偏移 12 的 u16 小端芯片 ID。另发 magic 和工程名都正确的 ESP32-S3 测试镜像，实际得到 `400 {"ok":false,"err":"bad_image"}`。 |
| 4 | **已修复** | `main/web/ui/app.js:506`、`:717`、`:804`、`:896` 用队列和在途请求共同阻止 OTA 确认与启动，升级开始后也保护发送循环和重启提示。加固 E2E 分别挂起退出 ACK 和设置 ACK，验证上传按钮保持禁用，超时排空后恢复，退出提示未弹出。 |
| 5 | **已修复** | `tools/web_mock/web_mock.py:213`、`:594` 在重启前保留运行快照及 busy。双客户端自测核对首个 200 后仍是旧运行槽，第二个 POST 得 409，重启后新版本和槽位切换、WS 断开。额外在 200 发送路径注入 `BrokenPipeError`，实测仍切槽并解除 busy。 |
| 6 | **已修复** | `tools/web_mock/web_mock.py:812`、`:870` 将 busy 判定和接受命令或更新配置放进同一把锁。用屏障暂停 `gnss.rate=5`，让另一个 HTTP 客户端先取得 OTA busy，再放行，WS 返回 busy，速率仍为 10。上传期间 `exit`、`save` 和 `freset` 也都返回 busy。 |
| 7 | **部分** | `main/web/ui/app.js:140`、`:669` 和 `tools/web_mock/web_mock.py:68`、`:218` 已实现工程名字节比较、有界字段读取及空版本 POST/GET 一致性。BOM 前缀、287/288 B、32 B 无 NUL、单字节 `FF` 和空版本用例通过。截断多字节序列仍有差异，见新发现 1。 |
| 8 | **部分** | 加固脚本增加启动、退出、网络和全局等待上限，覆盖 Factory 五项、客户端超时和重启断开 WS。报告已注明替身驱动的进度来源。实测子进程先被 SIGTERM 终止时，加固脚本的 `stopMock` 立即完成，指定路径的旧脚本 300 ms 后仍未完成。mock 自测已把 `TimeoutError` 判为失败，持续收到状态帧时的等待上限见新发现 2。 |
| 9 | **已修复** | `main/web/ui/app.js:649`、`:669`、`:804` 在选择变化时清除确认态并同步刷新，旧读取回调有序号守卫。加固 E2E 延迟 FileReader 100 ms，期间无法进入确认倒计时，新文件校验通过后按键恢复。 |
| 10 | **已修复** | `main/web/ui/app.css:43`、`:46` 的横幅与卡片区采用相同的可用宽度。用无头 Firefox 实际触发断线和上传成功，分别在 1440 px 和 500 px 视口取得 DOM 边界。两种横幅与卡片区左右误差均为 0 px，500 px 时左右留白都是 16 px。 |

### 新发现

1. **minor** · UTF-8 回退把一个截断序列显示成多个替代符（`main/web/ui/app.js:149`、`:162`、`:178`，`tools/web_mock/web_mock.py:80`）。将版本尾部设为 `E2 82` 时，用 Node 运行原函数的原生 `TextDecoder` 分支和 Python mock 的解码表达式，均得到一个 `U+FFFD`。禁用 `TextDecoder` 后得到两个。版本显示会随浏览器能力变化，错误的字符串也会参与“版本相同”的比较。建议让回退在报错时消耗已确认合法的前缀，并补截断序列及后续非续字节用例。
2. **minor** · WS 断开断言仍可一直等待（`tools/web_mock/web_mock.py:1530`、`:1535`，`docs/reports/O2-ota-ui.md:87`）。模拟重启漏关 socket 且每秒继续推送状态，2 s socket 超时会被每次收到的帧避开。将原断言块接到连续返回状态帧的客户端替身，模拟经过 4 s 后仍未通过或失败。指定路径的旧脚本也保留无上限的启动等待和 SIGTERM 后的退出等待（`2d90de6e…/scratch/e2e_ota_ui.mjs:135`、`:158`），当前命令外层的 `timeout` 会限制整个过程。建议为自测断开断言使用绝对截止时间，并让复审命令和修复报告指向同一份加固脚本。
3. **minor** · WS 首批消息发送失败会遗留已关闭的 socket（`tools/web_mock/web_mock.py:635`、`:650`、`:674`、`:698`）。客户端在初始快照期间断开时，发送异常发生在清理块之前。对本地 mock 的初始 `status` 注入 `BrokenPipeError` 后，客户端收到 EOF，`ws_clients` 仍保留一个 `fileno() == -1` 的对象。反复发生会累积到下一次成功 OTA。建议让注册后的初始发送也进入同一个 `try/finally`，并只等待确实已启动的 ticker。

### 本轮命令与结果

从 `RTK_main` 运行 Python 命令，Node 使用 `~/.nvm/versions/node/v24.21.0/bin/node`。两个 E2E 文件名相同，指定脚本位于 `…/brain/2d90de6e-e6fe-4cd1-b547-e33deadde8e9/scratch/e2e_ota_ui.mjs`，执行报告中的加固脚本位于 `…/brain/5ad9d61d-eeb2-42a5-917f-324b7938ded5/scratch/e2e_ota_ui.mjs`。

| 检查 | 结果 |
| --- | --- |
| `timeout 60 python3 -I tools/web_mock/pack.py --out <临时路径>/index.html.gz` | **PASS**，退出 0。raw 56,013 B，gzip 18,152 / 24,576 B（73.9%，余 6,424 B）。构建产物写至 `/tmp` 并已清除。 |
| `timeout 120 python3 -I tools/web_mock/web_mock.py --selftest` | **PASS**，退出 0，`SELFTEST PASS`。 |
| `timeout 120 python3 -I tools/web_mock/web_mock.py --selftest --caps-cm680` | **PASS**，退出 0，`SELFTEST PASS`。 |
| `timeout 120 node <指定脚本绝对路径>` | **PASS**，退出 0，4 组用例，`ALL E2E UI TESTS PASSED`。 |
| `timeout 120 node <加固脚本绝对路径>` | **PASS**，退出 0，8 组用例，`ALL E2E UI TESTS PASSED`。 |
| 定向回环测试、Node/Python 替身、无头 Firefox | **已执行**。真实 30 s 停传、发送异常后的翻槽及确定性 WS 并发用例均通过。边界复现见新发现。Firefox 已验证真实 POST 成功与两种横幅对齐。 |
| `git diff --check feat/ota-base`、冻结校验、Node 语法检查和 `doc_check.py --base feat/ota-base` | **PASS**。改动 C/C++ 函数 0 个。 |
| P2 HIL、移动端强制门户 | **NOT_RUN**。本轮验证环境为主机 mock 与桌面 Firefox。 |

默认沙箱拒绝创建 TCP socket。两套自测和指定 E2E 的首轮均因 `EPERM` 退出。按运行时审批在回环临时端口重跑后得到上述结果。

**最终结论 CHANGES_REQUIRED。** 10 条中 8 条已修复，2 条部分修复。版本解析仍会出现可复现的显示差异，WS 关闭回归门禁仍缺少绝对截止时间，mock 初始发送异常路径需要补齐清理。
