# OL6 — 弹窗两选项选择框 + COMM 页 Wi-Fi 信息

分支 feat/ol2-oled-ui（OL5 提交之后），worktree ../RTK_Main-ol2。不 stash，不改 main。

## A. 弹窗统一为两选项选择框（用户 HIL 反馈：单击直接取消导致误触）

- 适用所有确认弹窗：长按关机确认（OledRender.cpp:1539 "2x=OK 1x=CANCEL"）、切 Wi-Fi 确认（:1559 "1 CANCEL 2 CONFIRM"）、配对确认、以及其他同类弹窗（全部排查）。
- 交互：弹窗内两个按钮 [Cancel] [OK]（或 [No] [Yes]），**单击 = 移动选择框到下一项（循环）**，**双击 = 确认当前选中项**；单击不再直接取消。
- 默认选中：安全选项（Cancel/Reject）。关机确认可保留超时自动取消（现 5 s）；配对确认保持 30 s 超时 = 拒绝。
- 选择框移动沿用 OL4 的弹簧/挤压动画；遵循 OL5 的帧节拍与局部刷新（只重绘弹窗区域页）。
- 长按在弹窗中的行为保持现状（关机路径/8 s 强制关机不变）。
- 测试：单击 N 次后选中项正确循环；双击确认 Cancel → 关闭弹窗无动作；双击确认 OK → 执行动作；超时 → 安全选项；模拟器 golden 截图更新。

## B. COMM 页 Wi-Fi 模式显示 SSID 与 IP

- Wi-Fi（SoftAP）模式下 COMM 页显示：SSID（来自 systemStatus.deviceName，与 WebModule startSoftApWifi 一致，如 P2-85EE）、IP 192.168.10.12（取 WebModule 实际配置常量/netif，不要硬编码第二份），可选已连接客户端数。
- 数据经现有快照/DataCenter 只读传递给 OLED，不新增跨任务锁；BT 模式保持现状。
- 文本放不下时按现有 sanitize/截断规则，不得溢出。
- 测试：ViewModel 填充（Wi-Fi 模式含 SSID/IP，BT 模式不显示）；模拟器 Wi-Fi 场景截图。

## 约束与验证

- DRAM ≤ 256 B 增量，IRAM 0；不改 docs/protocol、ws-protocol。
- gates：`run_gates.sh --prefix ol14 --base 113f186` 全通过；main_host_check 全通过。
- 报告追加 docs/reports/OL6-oled-dialog-wifi.md；commit 后单独一行输出 OL6-FINISHED。
