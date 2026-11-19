# W4：网页前端 + 离线模拟服务

依据：ADR-0017 §2.4、§2.6；协议 `docs/web/ws-protocol.md` v1。
参考样式：`/home/gtc/Desktop/workspace/ESP32_PROJ/P2 手持式GNSS接收机项目/源码/P2 V2.7 基于V2.6增加电子围栏多区块/AP-Config/`（只读参考，不复制其依赖）。
基线：main。分支：`feat/w4-web-ui`（worktree）。与 W1、W3 并行。
实现：agy；交叉评审：codex。Architect（Claude）在浏览器里验收、合入。

## 1 目标

1. `main/web/ui/index.html`（可拆 `app.css`、`app.js`，构建时内联）：原生 HTML/CSS/JS，**不用任何框架和 CDN**，
   离线可用（设备热点没有外网）。
2. 视觉沿用旧页面：背景 `#272b30`、卡片 `#262626`、强调色 `#83ca98`、最大宽 600 px 单列、可折叠分组按钮、
   操作结果行内红/绿提示；手机竖屏 360 px 宽可用，无横向滚动。
3. 分组按 ADR-0017 §2.6：设备、GNSS、蓝牙、工厂（`hello.factory=false` 时只显示解锁框）、系统（退出到蓝牙模式）。
   GNSS 星座分组在 `config` 里没有 `gnss.const.*` 键时隐藏。
4. 行为：自动连接 `ws://<location.host>/ws`，断线 2 s 重连并显示状态条；请求按协议串行（等 `ack`）；`bye` 后提示
   “设备已切回蓝牙模式，可以关闭本页”。
5. `tools/web_mock/web_mock.py`：只用 Python 标准库（`http.server` + 手写 WebSocket 握手/帧），`uv run`/`python3 -I` 可跑；
   托管 `main/web/ui/`，按协议回 `hello/config/status/ack/bye`，状态数据随时间变化，可用参数模拟 `busy`/`locked` 错误。
6. `tools/web_mock/pack.py`：把页面内联并 gzip 成单个文件，打印字节数；**gzip 后 ≤ 24 KB**。

## 2 测试

- `python3 -I tools/web_mock/web_mock.py --selftest`：起服务、用标准库客户端走一遍 §6 示例与每个错误码。
- `pack.py` 输出大小写进报告。
- 截图：桌面 1280 宽与手机 360 宽各一张（存 `docs/reports/W4/`），说明怎么截的。
- 现有门禁不受影响（`tools/ci/run_gates.sh` 照跑）。

## 3 交付

- 报告 `docs/reports/W4.md`：页面结构、包大小、自测结果、截图路径、与旧页面的差异。
- 不 commit；不改冻结文件；结束打印 `W4 DONE`。
