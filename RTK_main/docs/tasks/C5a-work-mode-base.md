# C5a 工作模式与基站 RTCM

依据：ADR-0014 §2.4（模式切换后自动 SAVECONFIG）、§2.5（`gnss out` Hz）、§2.6（工作模式状态机，2026-11-18 批准）；ADR-0010、ADR-0012（驱动扩展点）、ADR-0006（RTCM 角色）、ADR-0013（命名/长度）。
基线：`ee1b73c`。实施者：agy（gemini-4-hydrogen-high）；Architect（Claude）指导与验收。smartBase 留给 C5b，本卡只预留 `smartbase` 模式值（设置时返回 `ERR not supported yet`）。

## 0 先读

`AGENTS.md`、`docs/CODING-STYLE.md`、ADR-0014 全文、ADR-0012 §2.4–§2.8、C2 的实现（`components/gnss/src/ControllerMaintenance.cpp`、`main/modules/gnss/GnssModuleMaintenance.cpp`、`GnssCmdsMaintenance.cpp`）——模式下发**复用** C2 的 `kMaintenance` 分步机制，不另开串口通道。参考实现（只读）：`../SparkFun_RTK_Everywhere_Firmware/Firmware/RTK_Everywhere/`（`States.ino` 基站流程、`GNSS_UM980.ino` surveyInStart/fixedBaseStart、`GNSS_UM980.h` / `GNSS_ZED.h` RTCM 缺省表）。

## 1 工作模式状态机（main/modules/gnss）

- 新类（建议 `WorkMode`，文件 `main/modules/gnss/GnssWorkMode.{hpp,cpp}`），由 GnssModule 持有，kIo 任务 `poll()` 驱动；**不放进** `gnss::Controller` 的 State 枚举。
- 状态：`kRover`、`kBaseConfig`、`kSurveyWait`（等 GST 水平精度 ≤ 阈值）、`kSurvey`（接收机自测平均）、`kBaseTx`、`kFallback`（一次性，打 W 后回 kRover）。
- 唯一入口 `requestMode(Mode, now)`：控制台/后续蓝牙/smartBase 只提交请求；Controller 不在 `kRunning` 时请求挂起到可执行为止（一个挂起槽，后到覆盖先到）。
- 流程：
  - base+survey：`kBaseConfig`（RTCM 报文集合 + 必要模式命令）→ `kSurveyWait`（精度 ≤ `svy_acc`）→ 下发 survey 命令（Unicore `MODE BASE TIME <s>`，u-blox CFG-TMODE3 survey-in，ComNav 等价或 `ERR not supported`）→ `kSurvey`，平均时长到且接收机开始输出 1005 → `kBaseTx`。
  - base+fixed：下发固定坐标命令（Unicore `MODE BASE <lat> <lon> <h>`；u-blox TMODE3 fixed LLA）→ `kBaseTx`。
  - 总超时 `svy_to`（从请求开始算，含等精度）到 → 下发回移动站命令（Unicore `MODE ROVER`）→ `kFallback`：W `"<mode> Survey-in timed out after %u s, back to rover."`，**NVS 中模式不改**。
  - rover：下发移动站命令，关闭基站 RTCM 输出。
  - 模式切换成功（进入 `kBaseTx` 或回到 `kRover` 的用户请求）后自动 SAVECONFIG 一次（复用 C2 `requestSave`）；`kFallback` 不保存。
- 开机：Controller 首次 `kRunning` 后按 NVS 模式自动 `requestMode`。
- 发布：`data` 新增 Slot（如 `GnssWorkModeStatus{mode, state, elapsedS, hAccCm, lastErr}`），供 `gnss status`、后续蓝牙读取。
- 日志：状态变化 I 级一句话（`<mode>` 标签，见 ADR-0009 §2.12），例：`"<mode> Base survey started, 20 s, accuracy 1.5 m."`、`"<mode> Base transmitting RTCM."`。

## 2 驱动扩展（components/gnss，授权改 frozen 头）

- `Receiver` 新虚函数（模仿 `saveStep`）：`modeStep(const ModeRequest&, uint8_t idx, ConfigStep&)`（rover / survey / fixed），以及基站 RTCM 报文开关的步骤；不支持的驱动返回 false → 上层 `ERR not supported`。
- Unicore：RTCM 输出命令写法、`MODE BASE TIME`、`MODE BASE <lat> <lon> <h>`、`MODE ROVER` 的确切语法与应答，参照 SparkFun Unicore 库/`GNSS_UM980.ino` 并在报告中列出依据；u-blox：CFG-VALSET TMODE3 + MSGOUT_RTCM。ComNav 可只返回不支持。
- GST 解析：`NmeaParser` 解析 GST 纬度/经度标准差，`Fix` 新增水平精度（cm，无效为 0xFFFF 或明确的 invalid 标志）。
- 授权修改的 frozen 头：`components/gnss/include/gnss/{Receiver,Controller,Types,NmeaParser}.hpp` 与三个驱动族头文件。

## 3 基站 RTCM 输出

- 报文集合与速率：`gnss out rtcm <id> <hz>`、`gnss out rtcm msm 4|7`（ADR-0014 §2.5 语法，Hz，<1 Hz 只允许 1/n）；存 NVS `gnss.base` 组。
- 缺省（参照 SparkFun）：1005 1 Hz、1074/1084/1094/1124 1 Hz、1033 0.1 Hz（Unicore）/ 1230 0.1 Hz（u-blox）。MSM7 时换成 1077/1087/1097/1127。
- 只在基站状态下发；rover 时关闭。
- 去向：接收机 COM2 上的 RTCM3 整帧（GNSS 侧 demux 已有 `rtcm3::kDecoder`）→ 推给已连接的 **PHONE 角色**蓝牙 SPP 会话（原始 RTCM 字节，复用现有推送/背压通道，满则整帧丢弃并计数）。统计进 `rtcm stats`（新增 `base_tx frames/bytes/drop`）。基站模式下手机发来的 RTCM 照旧计入 `rejected_role` 之外的新计数 `rejected_mode` 并丢弃（不得写回接收机）。

## 4 NVS 与命令

- 新组 `gnss.base`（StoreCatalog 显示名可读）：`mode`（rover/base/smartbase）、`base_type`（survey/fixed）、`svy_s`=20、`svy_acc_cm`=150、`svy_to_s`=120、`fix_lat`/`fix_lon`（1e-9 度整数或 double blob，说明理由）、`fix_h_mm`、RTCM 速率表、`msm`=4。
- 命令：`gnss mode [rover|base|smartbase]`（无参显示当前模式/状态/进度）；`gnss base`（显示参数）；`gnss base survey <s> <m> [timeout_s]`（范围：s 5..3600，m 0.1..10，timeout ≥ s+10 且 ≤ 3600）；`gnss base fixed <lat> <lon> <h>`（范围检查）。参数只写 NVS；若当前正处于基站则提示 `use 'gnss mode base' to apply`。
- `gnss status` 增加 `mode` 行；`config` 显示 `gnss.base` 组。
- `gnss freset yes` 同时恢复 `gnss.base` 缺省。

## 5 测试与门禁

- 主机测试（test/gnss、main_host_check）：状态机全路径（survey 成功、等精度超时、平均超时→fallback 不改 NVS、fixed、rover、Controller 非 kRunning 挂起、重复请求覆盖）；Unicore/u-blox 模式步骤字节；GST 解析；命令参数边界；基站 RTCM 转发（PHONE 会话、无会话、满丢弃）。
- 门禁：idf `builds/c5a` 0 warning；所有主机套件普通 + ASan（main_host_check、t13、services、app_fw、gnss 默认与 all-drivers `-DRTK_GNSS_DRV_COMNAV=ON -DRTK_GNSS_DRV_UBLOX=ON`、nvs_store 等）；G5b tidy（`tools/tidy_check/tidy_check.py`，含 --nvs-build）0；G7b（`tools/g7b_check/g7b_check.py --build builds/c5a`）；clang-format；`tools/doc_check` `--base ee1b73c` 0；golden 字节不变；新增函数 ≤60 行、认知复杂度 ≤25（ADR-0013 §2.4）；frozen 仅 §2 授权头可失败（不要重算 frozen.sha256，由 Architect 重算）；bin 增量写进报告。
- 禁止：`uvx`/系统 pip、堆分配、阻塞 UART 等待、修改与本卡无关的代码、git 提交。
- Architect 将执行的 HIL：`gnss mode base`（survey 20 s / 1.5 m）→ `kBaseTx`、手机收到 RTCM、`rtcm stats` base_tx 增长；`gnss base fixed ...`；不可达精度时 120 s 回退且 NVS 模式仍为 base；`gnss mode rover`；重启后自动进基站；`gnss freset yes`。

## 6 交付

报告 `docs/reports/C5a.md`：改动清单、设计取舍、接收机命令语法依据（文件:行）、门禁实测输出摘要、bin 大小、未完成项。最后一行单独打印 `C5a DONE` 并停止。
