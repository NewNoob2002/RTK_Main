# OL7 · 关机弹窗单选项与电量格数显示

基于 main `8c6ef0e`（PW1 已合入）开分支 `feat/ol7-dialog-battery`，worktree `../RTK_Main-ol7`。

## 用户决定（2026-11-25）
- 关机确认弹窗只保留一个确认选项：弹窗已有 5 s 超时自动关闭并取消关机，无需“取消”项。
- OLED 不再显示具体电量百分比，改为 **5 格**电量（0–20、20–40、40–60、60–80、80–100），用滞回抑制充电与负载引起的电压跳变。

## 现状（实施前核对）
- 弹窗：`main/modules/oled/OledUi.cpp:582-605` `handleModalClick`，初始焦点“取消”，单击切换焦点、双击选中；倒计时 `kPowerOffConfirmMs = 5000U`（`OledUi.hpp:49`），超时取消（`OledUi.cpp:464`）。渲染 `OledRender.cpp:1579` 起及 `:1643` 倒计时条。
- 电量：`board::batteryPercent`（`main/board/PowerLogic.cpp:127`，旧固件 11 点电压表线性插值），PowerModule 四点中值（`PowerModule.cpp:417-433`），无滞回。OLED 在 `OledRender.cpp:477-488`（头部百分比+图标）、`:836-856`、`:1300-1411`（待机/电池页）显示百分比。

## A 关机弹窗单选项（仅 `UiModal::kPowerOffConfirm`）
- 只绘制一个确认项（如 `POWER OFF`）及 5 s 倒计时条；单击或双击均确认关机（`OledAction::kShutdownKey`）；5 s 无操作关闭并取消，行为同现状。
- Wi-Fi 切换与配对弹窗保持两选项不变。
- 打开弹窗仍需长按；8 s 强制关机（`kForcePowerOffHoldMs`）、弹窗期间再次长按的现有行为不变。
- 单击判定须遵循既有双击消歧逻辑（ADR-0020 §5.2）；确认后的关机进度显示不退化。

## B 电量格数与滞回
- 新增纯逻辑（建议 `main/board/PowerLogic.*` 中 `BatteryBars` 或同等位置，便于主机测试），输入百分比/有效性/是否充电/USB 在位/时间，输出 0–5 格与闪烁标志：
  - 映射：首个有效样本直接定格；格边界 20/40/60/80%。
  - 放电**下降**：低于当前格下界 3 个百分点并持续 30 s 才降一格；**上升**：高于上界 5 个百分点并持续 60 s 才升一格（负载回落后的电压回弹不得立即升格）。
  - 充电中：显示从当前格到满格的循环填充动画，不用充电电压直接定格；充满（现有 full 判定）显示 5 格常亮。
  - 拔出 USB 后保持原格数 60 s，再以新样本重新定格（跳过滞回一次）。
  - < 10%（与低电关机阈值一致）：最后一格（或空框）闪烁。
  - 未知：沿用现有未知图标。
  - `power simbat` 会话变化时立即重新定格，保证 HIL 可注入观察。
- 常量全部具名并注明依据（锂电 3.6–3.9 V 平台区纯电压估算误差约 ±10–15%，充电端电压抬高约 100–200 mV）。
- OLED 所有位置去掉百分比数字，改为格数图标；电池详情页可保留电压（mV）作为诊断信息，并显示 `CHG/FULL/USB/BAT` 状态。
- **不改**：内部 0–100 百分比、低电关机逻辑（10%/120 s）、手机心跳 `batteryPct`、网页显示、`docs/protocol/`、`docs/web/`。

## 验收
- 主机测试：弹窗单击/双击确认、5 s 超时取消、其他弹窗不变；格数映射边界、下降 30 s / 上升 60 s 滞回（含边界前后 1 ms）、电压回弹不升格、充电动画与满格、拔 USB 60 s 保持后重定格、<10% 闪烁、simbat 会话变化重定格。
- OLED 模拟器（`tools/oled_sim`）截图对比：头部、待机页、电池页、弹窗。
- 全量门禁：`tools/ci/run_gates.sh --prefix ol7 --base 8c6ef0e` 全通过（含 ASan）。
- HIL（用户）：长按 → 单项弹窗 → 单击关机；5 s 不操作取消；`power simbat 5/50/95` 与插拔 USB 观察格数与闪烁。

## 约束与流程
- 授权修改：`main/modules/oled/*`、`main/board/PowerLogic.*`、`main/modules/power/*`（仅为提供格数输入）、`tools/main_host_check/*`、`tools/oled_sim/*`、`docs/adr/ADR-0020-oled-display.md`（§5.2/§5.3 弹窗与电量显示段落）、`docs/reports/OL7-dialog-battery-bars.md`（新建）。
- 不改冻结文件、`docs/protocol/`、`docs/web/`，不改产品其他默认值。
- DRAM 增量 ≤ 128 B，IRAM 0，无运行期堆；报告附资源对比。
- 不 push，不改 main；完成后提交（含报告：改动、资源、测试、NOT_RUN 项），单独一行输出 OL7-FINISHED。
