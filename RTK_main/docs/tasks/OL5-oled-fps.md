# OL5 — OLED 帧率优化（固定节拍、列级脏区、事务合并、局部动画）

分支：feat/ol2-oled-ui（OL4 HIL 修复提交之后的 HEAD），worktree ../RTK_Main-ol2。不 stash，不改 main。

## 背景（HIL 实测，2026-11-25，builds/ol11-idf）

- I2C 400 kHz（`<oled> SH1106 ready at 0x3C (400 kHz)`）。整帧 8 页 i2c_us max 28.5 ms，frame_us max 37 ms。
- 连续按键动画期间 `oled` fps 仅 6.5–16，实际约 13 fps；单帧 frame_us 12–25 ms，总线空闲 → 瓶颈在帧间调度，不在总线。
- 最近一帧 frame_us − i2c_us ≈ 8 ms（渲染耗时偏高，需要量化）。

## 目标与硬约束

1. 动画期间板上实测稳定 ≥ 25 fps（目标 30 fps，上限 30 fps）；整屏转场 ≥ 20 fps。
2. **CPU 占用**：静态页 0 帧、0 I2C；动画期间 oled 任务 CPU ≤ 10%（单核 Core 1，按 FreeRTOS run-time stats 或任务内 esp_timer 累计计算，`oled` 命令输出 cpu_pct）；渲染单帧 ≤ 4 ms（超出要在报告中给出热点与优化）。
3. **不影响业务**：任务优先级保持 1、Core 1 不变；不得持有 I2C 锁跨帧；每页锁持有时间约束（OL2/OL3 的 8 ms 参数预算）保持；I2C 等待期间任务阻塞（中断+信号量），不得忙等；不新增高频 notify 到其他任务；动画结束后 ≤ 1 帧回到事件驱动休眠。
4. DRAM 增量 ≤ 512 B（不做双缓冲）；IRAM 0；不加新依赖。I2C 保持 400 kHz，不改总线速度。

## 工作项

1. **固定节拍刷新**：动画/过渡/进度环/滚动等任一活动时，以 33 ms 绝对节拍（vTaskDelayUntil 或等价，按键通知可提前唤醒但不重置节拍）出帧；无活动时回到纯事件驱动等待。找出当前只有 ~13 fps 的真实原因（等待超时、动画步进只在事件后推进、dt 计算等），在报告中写明并修复。
2. **列级脏区（原第 3 项）**：每页记录变化的列范围 [x0,x1]，SH1106 设置列地址（注意 132 列偏移 2）只发送变化部分；整页变化时按整页发送。主机测试：单数字变化时发送字节数显著少于 132。
3. **事务合并（原第 4 项）**：每页的页地址/列地址命令与数据尽量合并或减少事务数；评估“每帧一次加锁”是否违反单页锁持有上限——若违反则保持逐页加锁，只合并命令。
4. **动画设计（原第 5 项）**：转场优先选择只改变部分页的效果（例如内容区竖向推入、状态栏不动、展开框只重绘变化行）；状态栏在转场中不重绘。保持 OL4 视觉风格，模拟器截图前后对比。
5. **观测**：`oled` 命令增加 fps_avg（最近 2 s）、frame_interval_ms（last/max）、wakeups/s、bytes/frame、render_us（last/max）、cpu_pct；增加 `oled bench <seconds>` 自动循环播放转场并输出统计（用于 HIL 无需按键），期间暂停 oled 自身日志输出。

## 验证

- gates：`run_gates.sh --prefix ol13 --base 9ad775a`，全部通过；main_host_check 全通过；新增测试覆盖节拍、列脏区字节数、bench 统计。
- 报告 docs/reports/OL5-oled-fps.md：原因分析、前后数据（主机计数 + 预期板上）、CPU 估算、size 增量。
- 板上由我验收：`oled bench 10` 前后对比，`sys tasks` 栈与 CPU。
- 完成后 git commit（feat(oled): OL5 frame pacing ...，末尾 Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>），单独一行输出 OL5-FINISHED。
