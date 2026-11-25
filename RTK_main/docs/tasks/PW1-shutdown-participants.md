# PW1 · 关机收尾参与者登记与关机来源

基于 main（OL1–OL6 已合入）开分支 `feat/pw1-shutdown`。

## 用户决定（2026-11-25）
- 关机必须等所有需要保存和未关闭的部分完成收尾后才能断电；关机请求可由不同模块发起（低电、长按、webserver、手机 app 等）。
- 低电关机：电量 < 10% 持续 **2 分钟**（现为 30 s，`main/board/PowerLogic.cpp:148-149`）。
- 协议：手机/WS 关机命令**暂不实现，只预留**（新增 `ShutdownReason::kWeb` 枚举，`kPhone` 保留；不改 phone_protocol.md / ws-protocol.md）。
- 同意：参与者登记表、保存超时 1 s → 3 s。

## A 收尾参与者登记表
- 静态注册表（编译期数组或启动期一次性登记，无堆）：`{name, begin(stage), isDone(), stage(kSave|kQuiesce)}`。
- 现有硬编码检查迁移为参与者：storage fence + pending writes + NVS inflight；GNSS flush fence + gnssPendingNvs；bt_link 待提交设置；web 待提交设置；射频暂停（BT/Wi-Fi）；安全 GPIO；日志排空；OLED 终帧（OL3 A）。
- PowerModule 只遍历表；超时日志按 name 列出未完成项；OLED 关机进度从表派生（替代 shutdownDoneMask 硬编码位，保持 UI 显示不退化）。
- 行为不变性测试：现有关机主机用例全部保持通过；新增“新登记参与者未完成则不断电直到超时”“超时降级列出名字”。

## B 超时与阈值
- `kShutdownSaveTimeoutMs` 1000 → 3000（注明原因：spiffs 上 NVS 大 blob 多页写）；30 s 断电兜底不变。
- `kLowBatteryHoldMs` 30000 → 120000；阈值仍 10%。可选：电压低于硬件保护阈值时立即关机（阈值需先在 CM680/F9P 板实测，未测前不启用，留 TODO 与来源）。
- 更新 ADR-0015 对应段落。

## C 关机来源
- `ShutdownReason` 增 `kWeb`（仅预留，无调用方），trace/OLED 文案覆盖所有 reason。
- 不改协议文档，不加 WS/手机命令。

## 验收
- 全量门禁、HIL：长按/console/低电（可用 console 注入电量或缩短常量的测试构建）三种来源关机，关机页阶段可见、终帧后断电，USB 下终帧后重启。

## D 遗留合并
- OL3 边缘：电池关机终帧保持期间插入 USB，共享 finalFrameReady_ 标志可能跳过 RESTARTING 帧 → 分开标志并加主机用例。
- 保持 OL4 长按确认弹窗（两选项）与 8 s 强制关机（kForcePowerOffHoldMs）行为不变。

## 约束与流程
- 基线 6b54fc4（main），分支 feat/pw1-shutdown，worktree ../RTK_Main-pw1。不 stash，不改 main。
- 授权修改：docs/adr/ADR-0015 关机相关段落、ADR-0020 关机进度段落、docs/reports/PW1-shutdown-participants.md（新建）。不改 docs/protocol、docs/web。
- DRAM 增量 ≤ 256 B，IRAM 0，无堆。
- gates：`tools/ci/run_gates.sh --prefix pw1 --base 6b54fc4` 全通过；main_host_check 全通过（含 ASan）。
- 低电 HIL 需要测试手段：console 注入电量或 Kconfig 测试构建，不得改变产品默认值。
- 完成后 commit（含报告：改动、资源对比、测试、NOT_RUN 项），并单独一行输出 PW1-FINISHED。
