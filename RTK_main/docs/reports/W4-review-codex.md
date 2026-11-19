# W4 交叉审查

审查基线为 `eaf1415`，依据 `docs/web/ws-protocol.md` v1、`docs/tasks/W4-web-ui.md` 和 ADR-0017 §2.4–2.6。审查覆盖同一设备热点下的页面和离线模拟服务。结论为暂缓合入，先修复三项 block。

## block

1. **8 秒超时后在同一连接发送下一条请求，跳过 ack 串行约束**。先改定位频率，再排队保存。首条请求 8 秒内无 ack 时，计时器会清除在途记录并发送保存，此时服务端仍可能在处理首条请求。独立 JavaScript 测试驱动观察到无 ack 时发送数由 1 变为 2。协议 §3 要求等匹配的 ack；超时可结束旧连接并按现有策略重连，重新同步配置后只发送用户的新操作。覆盖迟到 ack 和排队保存，并更新 mock 的丢 ack 验证。定位 `main/web/ui/app.js:98`、`main/web/ui/app.js:105`、`tools/web_mock/web_mock.py:660`、`docs/web/ws-protocol.md:30`。
2. **连接断开后还保留工厂解锁界面，重连时沿用已失效的会话状态**。解锁后断线，`onclose` 清理请求队列，但工厂写入和恢复出厂按钮仍可见。重连的 `onopen` 到新 `hello` 之间也可提交旧界面上的操作。在独立驱动中，断线后写入控件仍然可见，直到收到 `factory:false` 才隐藏。关闭连接时立即回到锁定界面，等当前连接收到 v1 `hello` 再启用操作。断线时向在途和排队操作提供结果提示，避免一直显示「正在发送」。定位 `main/web/ui/app.js:116`、`main/web/ui/app.js:156`、`main/web/ui/app.js:201`、`main/web/ui/app.js:291`、`main/web/ui/app.js:300`、`docs/web/ws-protocol.md:32`。
3. **mock 会执行 ID 无效的请求，且通过宽松的 JSON 解析掩盖固件拒绝路径**。缺 ID 或 ID 超出 1–65535 时只把回显 ID 改为 0，合法的 `set` 值仍会写入配置。Python 的 `bool` 还是 `int` 的子类，`true` 能通过现有检查。默认 `json.loads()` 保留最后一个重复键，也会通过未知字段里超出 v1 子集的值。应在错误注入和业务分派前校验 ID 类型和范围、重复键、请求字段和 JSON 子集，确保拒绝路径无状态变更。补自测断言错误应答且配置保持原值。定位 `tools/web_mock/web_mock.py:298`、`tools/web_mock/web_mock.py:312`、`tools/web_mock/web_mock.py:362`、`docs/web/ws-protocol.md:8`、`docs/web/ws-protocol.md:34`。

## should

4. **完整 config 被当作增量合并，星座键消失后分组仍可编辑**。曾收到星座键，断线后连上一个返回完整但无星座键的配置，旧键仍留在 `lastConfig`。独立驱动确认分组继续可见。按协议 §2 用最新完整 config 替换缓存，切换会话时清除上一连接的配置与控件状态。覆盖有星座配置切换到 `--no-const` 的重连场景。定位 `main/web/ui/app.js:130`、`main/web/ui/app.js:212`、`docs/web/ws-protocol.md:18`、`docs/tasks/W4-web-ui.md:15`。
5. **退出和恢复出厂的成功 ack 会在 bye 到来前放出队列里的下一条**。点击退出后再排队修改参数，成功 ack 会立即驱动下一次发送，设备此时即将重启。将成功的 `exit`、`freset` 视为终止请求，收到 ack 后停止发送，等待 bye 或关闭。普通 `bye` 的提示和停止重连已在独立驱动中确认，`freset` 使用对应的重启提示。定位 `main/web/ui/app.js:249`、`main/web/ui/app.js:265`、`tools/web_mock/web_mock.py:340`、`docs/web/ws-protocol.md:21`。
6. **页面未在发送前限制 UTF-8 帧长**。解锁框没有长度约束，粘贴长文本即可生成超过 1024 B 的消息。设备依协议以 1009 关闭连接，页面不会显示对应输入错误。序列化后按 UTF-8 字节数检查，超限时在解锁行提示并保留连接。补含多字节文本和 JSON 转义后的 1024/1025 B 边界。定位 `main/web/ui/app.js:102`、`main/web/ui/app.js:357`、`main/web/ui/index.html:129`、`docs/web/ws-protocol.md:8`。
7. **设备省略不可用 GNSS 数值时，页面保留上一次的有效读数**。设备先报告 fixed 和精度，随后发送 gnss 组及 `fix:"none"` 并省略 `hacc_mm`，页面仍显示此前的精度。组出现时把缺失的测量值恢复为占位符，只有整组缺席时保留该组数据。mock 始终提供所有 GNSS 数值，增加无定位或未就绪的状态样本可覆盖该路径。定位 `main/web/ui/app.js:232`、`tools/web_mock/web_mock.py:277`、`docs/web/ws-protocol.md:19`、`docs/web/ws-protocol.md:24`。
8. **mock 的白名单更新缺少 config，出站帧也没有 1024 B 检查**。增删白名单成功后只发送 ack 和 status，协议 §2 要求任一成功 set 后都带完整 config，写入键本身继续只出现在请求中。白名单可持续增长，`ws_send_json()` 会直接发出超过 1024 B 的 status。另外，MAC/SN 的 Python 正则用 `match` 配合 `$`，会接受末尾 LF，和 §4 及 W3 校验不同。按正常 set 的顺序补 config，使用 `fullmatch`，并让白名单和出站长度受协议预算约束。若保留多客户端测试能力，定时推送也应能让另一连接看到白名单组的变动。定位 `tools/web_mock/web_mock.py:131`、`tools/web_mock/web_mock.py:219`、`tools/web_mock/web_mock.py:460`、`tools/web_mock/web_mock.py:488`、`docs/web/ws-protocol.md:18`、`docs/web/ws-protocol.md:43`。
9. **截图像素宽度与任务卡要求的宽度不一致**。PNG 文件头显示桌面图为 1354×1215，手机图为 488×1215。报告只记录了窗口尺寸设置，缺少实际 CSS 视口宽度或像素比证据。重新验收时记录 `innerWidth`、设备像素比和横向滚动宽度，并交付对应截图。定位 `docs/reports/W4.md:39`、`docs/tasks/W4-web-ui.md:26`。

## suggest

10. **白名单可以用 DOM 方法清空，保留安全的文本节点渲染**。当前唯一的 `innerHTML` 赋值是常量空串，用来清空白名单列表。设备标题、状态、SN、错误和 MAC 都进入 `textContent` 或表单 `value`，静态追踪未发现设备字符串进入 HTML 解析。用 `replaceChildren()` 清空列表即可去掉该 HTML 写入点。定位 `main/web/ui/app.js:149`、`main/web/ui/app.js:161`、`main/web/ui/app.js:201`、`main/web/ui/app.js:258`。

## 核验

| 项目 | 结果 |
| --- | --- |
| 标准库 mock 自测 | PASS。沙盒限制创建本地套接字，经授权后运行 `python3 -I tools/web_mock/web_mock.py --selftest`，通过 HTTP、握手、§6 示例、现有错误用例、bye 和 1009 检查。 |
| 前端状态机 | FAIL。用 `gjs -c` 载入真实 `app.js`，以替身 DOM、WebSocket 和计时器验证。复现无 ack 时 8 秒后第二条请求发出，断线后工厂写入控件仍可见，无星座键的重连仍显示分组。确认 2 秒重连、收到锁定 hello 后隐藏工厂操作，以及 exit bye 后提示并停止重连。 |
| 打包与冻结文件 | PASS。用 `python3 -I -B` 调用 `pack_ui()` 并在内存 gzip，确认脚本和样式内联，原始 23,024 B，压缩后 6,546 B，小于 24,576 B。运行 `sha256sum -c --quiet docs/frozen.sha256` 通过。 |
| 截图尺寸 | FAIL。用 `file docs/reports/W4/*.png` 读取现有尺寸，结果见第 9 项。 |
| 全量 CI、实际浏览器视口和设备联调 | NOT_RUN。本次验证使用现有截图、源码和独立 JavaScript 状态机驱动。 |
