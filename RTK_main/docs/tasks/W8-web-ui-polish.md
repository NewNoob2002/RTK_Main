# W8：网页配置界面视觉与交互重做（双实现对比）

- 依据：ADR-0017 §2.4（单页 gzip ≤ 24 KB、离线、内嵌固件）、`docs/web/ws-protocol.md` v1；用户 2026-11-20：现页面“样式与组件太粗糙”。
- 基线：`feat/w2-web-server` @ `fb2a4be`（含 W2 rev A 与 gate 开关修复）。
- **同一任务两份独立实现**，用户择优合入：
  - Codex：分支 `feat/w8-ui-codex`，工作树 `RTK_Main-w8c`，构建前缀 `w8c`；
  - AGY：分支 `feat/w8-ui-agy`，工作树 `RTK_Main-w8a`，构建前缀 `w8a`。
  - 两边互不查看对方工作树。
- 执行者不提交、不烧录、不连串口。

## 1 允许修改范围

- 只改 `main/web/ui/**`（index.html、app.css、app.js、logo 资源）与 `tools/web_mock/web_mock.py`（仅模拟数据/新增演示场景，协议行为不变）。
- **不改** 任何 C/C++、Kconfig、`docs/web/ws-protocol.md`、`tools/web_mock/pack.py` 的 24 KB 上限。
- WebSocket 协议、键名、值域、错误码、解锁流程全部不变；页面必须继续通过 `python3 tools/web_mock/web_mock.py --selftest`。

## 2 硬约束

1. 完全离线：无 CDN、无外部字体/图标库、无框架（原生 HTML/CSS/JS，ES2017 以内，iOS 15 Safari 与 Android Chrome 90+ 可用）。
2. `pack.py` 输出 gzip ≤ 24 KB（现 15.9 KB，其中 logo.png 约 6.4 KB 不可压缩；可改为手写精简 SVG 或 CSS 文字标，须保持 “SingularXYZ” 品牌字样与绿色主色）。
3. 移动优先：360 px 宽无横向滚动，触控目标 ≥ 44 px，16 px 侧边距；桌面 ≥ 900 px 时两栏布局。
4. 深色/浅色：用 CSS 变量，跟随 `prefers-color-scheme`；对比度满足 WCAG AA。
5. iOS 门户小窗（W7 将使用）中可用：不依赖 `localStorage` 成功、不弹 `alert/confirm`（危险操作用页内确认）。
6. 无障碍：每个控件有 `<label>`/`aria-label`；开关整块可点（本次 HIL 的 bug 就是开关不可点）；键盘可操作；`:focus-visible` 明显。

## 3 交互要求（参考 ESPHome web_server v3、WLED、Tasmota 的设置页风格，只借鉴，不抄代码）

1. **顶部状态条**（吸顶）：连接状态（已连/重连中/断开，带颜色点）、Fix 类型（NONE/SINGLE/DGPS/FLOAT/FIXED 不同色）、卫星数、GGA 门控 blocked 指示、Wi-Fi 空闲倒计时（若 status 无此字段则省略，不得新增协议字段）。
2. **卡片分组**：Device / GNSS / GGA Quality Gate / Bluetooth / Factory（锁定时折叠并显示锁图标）/ System。卡片可折叠，记住展开状态（存储失败时静默降级）。
3. **乐观更新 + 回滚**：控件改动后进入 pending（旋转/半透明并禁用），收到 `ack ok` 后以 `config` 为准；`ack` 错误或 2 s 超时则回滚到上次 config 值并显示错误。不得出现页面显示与设备不一致。
4. **Toast**：成功/失败/断线消息统一右下（手机底部居中）浮层，3 s 自动消失，错误码映射为英文可读文本。
5. **断线重连**：指数退避 1→2→4→8 s 上限；重连期间整页控件禁用并显示横幅；收到 hello 后恢复。
6. **输入校验**：MAC 自动大写并补冒号、实时校验；SN 字符集与长度实时提示；min_sats 0–40 步进器（− / + 按钮 + 输入框），失焦或回车才发送。
7. **危险操作**：Factory Reset、Exit 使用页内二次确认（按钮变为“Confirm (3)”倒计时样式），不用 `confirm()`。
8. 输出速率选项中高于定位速率的项置灰并说明（保留现有规则）。
9. 不支持的键（`unsupported`）或 config 中缺失的键：整行隐藏或置灰并注明 “Not supported by this receiver”。

## 4 验证与交付

- `python3 tools/web_mock/pack.py` 打印 gzip 大小 ≤ 24 KB；`python3 tools/web_mock/web_mock.py --selftest` PASS（沙箱回环被拒时写 BLOCKED 及原因，不得跳过不报）。
- 用 web_mock 跑通以下场景（`--force-err`、`--drop-ack`、`--gnss-none`、`--no-const`、断线重连），在报告里逐条写结果。
- 若环境有无头浏览器（chromium/playwright 已安装才用，不得安装新依赖），附 360 px 与 1280 px、深/浅色截图到 `docs/reports/W8-<codex|agy>/`；没有则写 NOT_RUN。
- `tools/ci/run_gates.sh --base main --prefix <w8c|w8a>` 全 PASS（W2 尚未合入 main，G1 若报 W2 已授权的 `sdkconfig.defaults` 差异属预期，注明即可）。
- 报告 `docs/reports/W8-<codex|agy>.md`：设计说明（配色、布局、组件）、gzip 前后大小、各场景结果、偏离项。
- 完成后打印 `W8 DONE`。
