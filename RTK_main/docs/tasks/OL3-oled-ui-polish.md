# OL3 · OLED UI 打磨、通信页与主机仿真器

分支 `feat/ol2-oled-ui`（在 99d68a4 之上继续，OL2 未合入，OL3 与 OL2 一起合入）。基线门禁 `--base 78e5801`。

## 用户实板反馈（2026-11-25）
自检与表盘正常。问题与需求：
1. 表盘下方入口图标太粗糙，需要更精细。
2. 通信/设置页增加：Wi-Fi / 蓝牙切换（开启 webserver、返回蓝牙通信）。
3. 蓝牙部分增加：打开配对窗口、配对请求确认。
4. 页面切换过渡动画急促、生硬。
5. **Bug**：长按关机后显示关机页，但关机页还没完全显示系统就掉电。
6. 建一个主机 UI 仿真器测试页面（WouoUI 仓库用 EasyX，仅 Windows；我们用 SDL2）。

## A 关机掉电过早（先做，最高优先级）
- 现状：`main/board/PowerLogic.cpp:273` `ShutdownSm::step` 在电池路径进入 kCutPower 后立刻驱动 GPIO4，没有等 OLED 关机页的最后一帧；终帧等待只在 USB 重启路径。
- 要求：电池路径与 USB 路径同样，先等“关机最终画面已刷出”（`kShutdownFinalFlushed`，带截止，沿用 OL2 的绝对截止写法），再断电。关机页的阶段进度需能被肉眼看到：每个阶段在屏上至少保持 `kMinStageVisibleMs`（建议 250 ms，写成常量并注明理由），但不能延长到超过现有 30 s 回退；保存/静默真实耗时更长时按真实耗时。
- 最终画面（"POWER OFF" / USB 时 "RESTARTING"）刷出后再保持 ≥300 ms 才断电/重启，让用户看清。
- 修改 ADR-0015/ADR-0020 中关机时序描述。补主机测试：电池路径断电前必须已终帧；终帧失败或超时仍断电（降级）；trace 时间正确。

## B 主机 UI 仿真器（SDL2，先于 C/D/E 做，后续改动都用它截图验证）
- 主机已装 SDL2 2.32.74（`pkg-config sdl2`）。新增 `tools/oled_sim/`：链接真实的 `OledUi`/`OledRender`/`OledAnim`/WouoUI 绘图代码与 Sh1106 帧缓冲替身，SDL 窗口按 128×64 放大（默认 ×6，单色蓝/白可选）。
- 键盘映射：空格=短按，快速两次=双击（走真实 ClickDetector，不在仿真器里另判），按住≥2.5 s=长按关机；数字键切换注入的场景（无卫星/RTK Fixed/充电/低电/蓝牙已连/Wi-Fi 模式/配对请求中/GNSS 故障）。
- 时间：真实单调时钟驱动，支持 `--fps` 与 `--slow N` 慢放动画；`--headless --script file --dump dir` 无窗口跑脚本输出 PNG 序列（或 PBM），供 CI 与我验收截图。
- 不进入固件构建；仿真器不改生产代码的行为，只允许为可测性抽出纯函数接口。加一个 ctest：headless 脚本跑完自检→表盘→每个子页→关机，断言无崩溃并与 golden 帧（少量关键帧）逐字节一致。
- 文档 `tools/oled_sim/README.md`：构建、按键、场景、截图命令。

## C 图标精细化
- 表盘下方入口图标重画：16×16（或现布局允许的最大尺寸），1 px 线条，保留留白与对齐，不同入口风格一致（卫星、定位、通信、设备/设置、电源）。提供选中态（反白或描边），用仿真器出对比截图（旧/新）放进报告。
- XBM 存放在 `main/modules/oled/OledIcons.hpp`，注明来源（自绘则写自绘）。

## D 通信/设置页：Wi-Fi/蓝牙切换、配对
- 通信页条目：`Mode: BT / WiFi`（切换=进入 web 模式或返回蓝牙，复用 `radio wifi` / 退出 web 的现有路径，不新造），`Pair window`（打开/关闭，复用 `BtLinkModule::openPairing`，显示剩余秒数），状态行（蓝牙已连设备数 / Wi-Fi SoftAP SSID 与客户端数）。
- 配对确认：后端已有 `SPP_SERVER_PAIR_CONFIRM` + `spp_server_pair_decide(bool)`（`components/spp_server/SppServerSession.c:322`，30 s 超时）与 console `bt pair accept/reject`（`main/modules/bt_link/BtLinkCmds.cpp:169`）。OLED 在有挂起请求时弹出确认页：显示对端地址后 3 字节（有名字就显示名字）与 SSP 数字（若可得），双击=接受，单击=拒绝，超时自动消失；任意页面都能弹出，结束后回原页。需要把挂起请求的只读快照经 DataCenter/现有快照路径给 OLED（不在 OLED 任务里直接调用 spp_server 的锁）。
- 配对窗口打开方式：需要确认模式（`window.confirm`）由通信页打开时默认使用确认模式。
- 交互：子页内用单击移动选中、双击执行；返回表盘的操作要保留（例如最后一项 `Back` 或长按 <2.5 s 的方案——不要新增长按语义；建议列表末尾 `Back` 项）。在报告里写明最终按键表。
- 切 Wi-Fi 前在屏上确认一次（双击确认/单击取消），因为会断开蓝牙。

## E 过渡动画
- 现 PageTransition 300 ms + easeOutCubic，反馈急促生硬。改为：时长 380–450 ms，曲线用 easeInOutCubic 或临界阻尼弹簧（参考 GNSS_Panel `cubic-bezier(0.4,0,0.2,1)` 350 ms），帧率保持 ≥30 fps；进入时内容 40 ms 错峰上移 4–6 px（只动位置）；返回与进入对称。
- 用仿真器 `--dump` 输出过渡帧序列，报告中给出每帧位移曲线，确认无跳帧、无大于 1/4 屏的单帧位移。
- I2C 每帧写页数增加时注意持锁预算（OL2 R3OL2-01 结论：8 ms/页是传参预算）。

## 约束
- 不改冻结文件（frozen.sha256），确需修改先停下报告。DRAM 增量 ≤1 KB（对 OL2 99d68a4），仿真器不计。
- 每个修复/新功能补测试并确认删实现会失败；全量门禁 `ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol9 --base 78e5801`。
- 报告 `docs/reports/OL3-oled-ui-polish.md`：改动、按键表、截图路径、门禁结果、bin/DRAM 增量、未实板项。
- 不提交、不烧录、不碰串口。完成后单独一行输出：OL3-FINISHED
