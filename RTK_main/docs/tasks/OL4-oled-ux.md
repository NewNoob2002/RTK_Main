# OL4 · OLED 交互与排版（参考 GNSS_Panel）

分支 `feat/ol2-oled-ui`（基于 75f3c43，继续在 `../RTK_Main-ol2`），门禁前缀 `ol10`，完成后单独一行输出 `OL4-FINISHED`。

参考：`/home/gtc/Desktop/workspace/HC32_PROJ/GNSS_Panel/docs/ui_style_and_page_review.md`、`docs/images/ui-pages.png`、`App/UI/`（Startup、Dialplate、SystemDash、SaveConfig、Spring、numberFlow）。只借鉴版式与交互，不拷贝代码与位图；P2 为 128×64 单色 SH1106、单按键。

## 用户反馈（2026-11-25）
1. 充电待机页太简陋，没有按键提示；关机没有触发提示和确认，容易误触。
2. 表盘子页面排版要优化，选项切换要有过渡动画。
3. 其余部分参考相关设计一起分析优化。

## A 长按关机：进度提示 + 确认（防误触）
- 正常模式按下按键 ≥ 600 ms 起，在当前页上方叠加“长按进度”浮层：电源图标 + 环形/条形进度 + `HOLD TO POWER OFF`，进度按 600→2500 ms 线性填充；在 2500 ms 前松开 → 浮层收起（150 ms 淡出/收缩），不产生短按、不关机。
- 到 2500 ms 不直接关机，进入确认页 `POWER OFF?`：`2x=OK  1x=CANCEL`，带 5 s 倒计时条，超时自动取消；双击确认才调用 `power::requestShutdown(kKey)`。确认页期间配对弹窗优先级低于确认页（排队）。
- 低电、console、（未来）网页/手机来源不走确认页，直接进入关机页，但关机页首行显示原因（`LOW BATTERY` / `CONSOLE` / `KEY`）。
- 需要电源模块把“按键按住中 + 按下起点”以快照字段暴露给 UI（只读、无锁原子），长按事件改为“长按到达”通知 UI 而不是直接关机；PowerModule 的 kKey 关机改由 UI 确认后发起。无 OLED（Feature::kOled 关闭）时保持现有行为：长按 2.5 s 直接关机。
- 更新 ADR-0015 按键段与 ADR-0020（已授权仅改这两份 ADR 的按键/关机段落）。

## B 充电待机页重做
参考 GNSS_Panel Startup：左侧大号电量百分比（数字滚动）+ 电池条 + 状态行（`CHG` / `FULL` / `USB`，可用时显示电压），右侧电源图标外圈进度环 + `HOLD 2s ON` 提示；底部一行 `FW V3.x  HW V1.41`。
- 按住按键时右侧环按开机所需保持时间实时填充（来源：standbyWake 现有保持判定，暴露起点/阈值给 UI），松开环回落。
- 充电中电池条有缓慢“流动”动画（≤ 2 fps 刷新，静止时零写入规则对充电动画放宽，限 2 fps）；充满时停止。
- 仿真器增加充电待机场景（数字键 `9`，或 `--boot standby`）和 headless golden。

## C 子页面排版统一
参考 GNSS_Panel 左主卡 + 右行列表 + 末尾 Back 箭头：
- 统一子页框架：顶部 10 px 状态条（卫星数、解状态、BT/Wi-Fi、电量，与表盘一致）；下方左侧 40×52 主卡（大图标 + 一个关键值），右侧最多 3 行可滚动列表行（图标 + 名称 + 右对齐值），超出用滚动指示条。
- 位置页：主卡显示解状态（FIX/FLOAT/DGPS/SPS/NONE）+ 精度；行：纬度、经度、高程、HDOP/AGE。
- 卫星页：保持 4 柱，柱顶数值、柱底系统名，柱高按可见/使用分两段，标题行显示 `used/visible`。
- 通信页：主卡 BT/Wi-Fi 当前模式图标；行：模式切换、配对窗口、已连设备名、Back。
- 设备页：主卡设备图标 + 型号；行：FW、SN、BT 名、运行时间、Back。
- 电源页：主卡电池图标 + 百分比；行：电压、充电状态、USB、`Power off`（双击进入 A 的确认页）、Back。
- Back 统一为左箭头图标项。

## D 选择与切换动画
- 选择框弹簧跟随（复用 OledAnim FixedSpring），行间移动 ≤ 180 ms；列表滚动时内容平滑位移而非跳行。
- 选中按下“挤压/回弹”反馈：双击确认瞬间选择框收缩 2 px 再回弹（≤ 120 ms）。
- 子页进入：表盘图标位置展开到全屏（沿用 OL3 expand），内容行 40 ms 错峰上移 6 px；返回时收拢回原图标。
- 数值变化用 NumberRoll；静止时零 I2C 写入保持。

## E 其他打磨（分析后执行，逐条写入报告）
- 表盘：时间/UTC 显示（有 GNSS 时间时），解状态大字随状态切换带 NumberRoll/淡变；底部 dock 选中项上方显示名称标签。
- 关机页：首行原因，进度环外加阶段名，S/R/P 改为三个小图标勾选。
- 开机自检页：逐项打勾动画（沿用 OL2 自检逻辑，仅改视觉）。
- 页面无操作 60 s 自动回表盘（可在常量里改）。

## 约束
- DRAM 增量 ≤ 1 KB（相对 75f3c43），不得引入堆分配；IRAM 0；固件增量报告。
- 每页 I2C 写锁沿用 8 ms 参数预算；30 fps 上限；静止零写入（充电动画例外，≤ 2 fps）。
- 只改 `main/modules/oled/`、`main/modules/power/`、`main/board/PowerLogic.*`（按键/待机暴露字段）、`main/def/OledDef.hpp`、`tools/oled_sim/`、测试与 golden、上述两份 ADR、`docs/reports/OL4-oled-ux.md`。冻结文件不得改。
- 测试：长按 <2.5 s 松开不关机；2.5 s 后单击取消、双击确认、5 s 超时取消；无 OLED 构建保持直接关机；待机环进度；子页 golden；全部门禁 `bash tools/ci/run_gates.sh --prefix ol10 --base 75f3c43`。
- 报告 `docs/reports/OL4-oled-ux.md`：逐条对照 A–E、截图（仿真器 PNG 放 `docs/reports/ol4/`）、尺寸、未实测项。不要 git commit，不要 stash。
