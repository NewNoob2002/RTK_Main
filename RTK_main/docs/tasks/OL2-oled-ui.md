# OL2：OLED 表盘 UI、开机自检与关机进度、流畅动画

## 背景
OL1（已合入 main）提供 SH1106 驱动、共享 I2C0、只读文字页、≤5 Hz 刷新、短按翻页。用户实板反馈（2026-11-24）：
- 关机提示一闪而过：`OledUi.cpp` 在 `kCutPower` 展示 ≥200 ms 就关屏；USB 路径很快重启。
- 开机生硬无过渡；页面只有文字，单调。
- 希望参考 `/home/gtc/Desktop/workspace/HC32_PROJ/GNSS_Panel`（LVGL，294×126 彩屏）的“表盘 → 子菜单”风格与开机设备检查页。
参考资料（只读）：`GNSS_Panel/docs/ui_style_and_page_review.md`；`App/UI/SystemLoading/*`（逐项非阻塞 poll、每步最短 300 ms、结果停留 150 ms）；`App/UI/Dialplate/*`、`App/UI/SaveConfig/*`、`App/Utils/Spring/*`、`App/Utils/lv_anim_label/numberFlow*`。只借鉴设计，不拷贝 LVGL 代码。

## 用户已定决策
1. 按键方案 A：短按=下一项；双击=进入/返回；2.5 s 长按=关机（不变）。短按需等待双击窗口（≤300 ms，常量可调）才判定。
2. 关机提示不是固定时长：显示关机进度，按真实收尾推进（`ShutdownSm` kAnnounce→kSave→kQuiesce→kCutPower 及各模块 fence），全部收尾完成后才断电/重启；屏幕在断电前保持显示。USB 路径（`esp_restart` 进充电待机）同样在全部完成后才重启。
3. 开机做“设备自检”页，按真实模块就绪推进。

## 要求
### A. 开机自检（SystemCheck 页）
- Logo 过渡（淡入/逐字显现，单色可用抖动或逐列展开）→ 自检列表 → 滑入表盘。
- 自检项（按真实状态，非阻塞，不增加开机耗时，不阻塞任何模块）：电源/电池、存储(NVS)、蓝牙/Wi-Fi 射频、GNSS 探测（型号/固件）、充电芯片（无则显示“-”不算失败）。每项 Pending 动画 → ✓/✗；每项最短展示 ≤300 ms 仅用于可读性，GNSS 探测等待上限沿用现有探测超时；失败项不阻塞，进表盘后状态栏显示告警图标。
- 数据只读现有快照/状态接口（不得在 UI 里调驱动）；如缺就绪信号，以最小改动在对应模块的快照中加只读字段，并说明。
### B. 关机进度（Shutdown 页）
- 进度环/进度条 + 当前阶段文字（保存配置 / 断开连接 / 关闭外设 / 即将断电），由 `powerStatus` 快照中的阶段与 fence 推进；PowerModule 状态机与超时常量不改语义，只在快照中暴露阶段与完成项。
- 无 USB：保持显示直至硬件断电（不再在 kCutPower 主动关屏；仅在真正断电动作前最后一刻发 0xAE 可选）。USB：全部收尾完成 → 画“已关机/充电中” → 再 esp_restart；不得引入固定延时，允许的唯一额外等待是最后一帧 I2C 刷出（≤30 ms）。
- 30 s 超时回退路径保持，屏幕恢复表盘。
### C. 表盘与子菜单（128×64 单色）
- 表盘：顶部状态栏（卫星数/解状态、蓝牙连接、电量图标+百分比、充电闪电）；中部大号解状态（NONE/SPS/DGPS/FLOAT/FIX）或基站模式；底部 3–4 个图标入口（卫星、位置、设备、电源）带选中框。
- 子菜单：卫星（各系统可见/使用柱状图、CN0 条）、位置（经纬高、HDOP、差分龄期）、设备（版本、SN、BT 名、运行时长）、电源（电量、电压、充电状态、USB）。双击进入（从图标展开动画），双击返回（收回动画）。
- 图形：位图图标（XBM，自绘，不用受版权素材），进度条、柱状图、电池图标；数字变化滚动动画。
### D. 动画与性能
- 动画期间目标 30 fps（帧周期 33 ms），静止 0 写；缓动 ease-out cubic / 弹簧（定点数，无浮点 printf，无堆）；页面切换 ~300 ms 滑动视差；选中框弹簧跟随。
- I2C 保持 400 kHz（SH1106 规格上限）；按页刷新，只写脏页；每页持锁 ≤8 ms 不变；Charger 0-tick 尝试加锁不受影响。
- 统计：`oled` 命令显示 fps、每帧 I2C 耗时、脏页数、最大帧耗时；`sys tasks` 栈 HWM。
- 预算：DRAM 静态增量 ≤1 KB（框架状态），oled 栈若需加大须说明并给 HWM；Flash 增量报告（图标/字体）；IRAM 0。
### E. WouoUI
- 继续只用 graph/font 子集；动画/页面框架自写（定点）。如需上游新文件须保持 MPL-2.0 标注。

## 测试
- 主机测试：双击/短按判定（含双击窗口边界、长按不误判）；自检状态机（全部成功、GNSS 失败、充电芯片缺失）；关机进度按阶段推进、USB 路径在全部完成前不重启、超时回退；缓动/弹簧定点函数单调与终值；帧调度（动画 33 ms，静止 0 写）；页面渲染非空与脏页。normal + ASan/UBSan。
- 门禁：`ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol3 --base 78e5801`，43 全过。
- 报告 `docs/reports/OL2-oled-ui.md`：改动、测试、门禁、体积/DRAM/栈、实测 I2C 帧耗时估算、HIL 步骤（NOT_RUN）。ADR-0020 增补 UI 架构与按键方案。

## 约束
遵守 AGENTS.md、docs/CODING-STYLE.md；不改冻结文件；不改 GNSS/Settings 路径；不提交、不烧录、不碰串口。完成后输出 "OL2 DONE"。
