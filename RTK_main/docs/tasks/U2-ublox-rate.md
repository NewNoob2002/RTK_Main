# U2：ZED-F9P 定位频率、语句输出周期、NMEA 高精度

- 分支：`feat/u2-ublox-rate`（基线 main `3bbd6d2`）
- 依据：`docs/reports/U2-ublox-gap.md`（SparkFun RTK Everywhere `GNSS_ZED.ino` + u-blox GNSS v3 3.1.15 对比）、ADR-0010、ADR-0012、ADR-0017、ADR-0018
- 实板：P2 v1.41 + ZED-F9P HPG 1.12（`docs/reports/U1-f9p-driver.md` §10）

## 目标

1. F9P 上 `gnss.rate` 与 `gnss.out.{gga,gsa,gsv,gst,rmc}` 在控制台、网页可读可改，掉电保持，与 CM680 行为一致。
2. 接收机输出 7 位小数 NMEA（CFG-NMEA-HIGHPREC）与扩展卫星编号（CFG-NMEA-SVNUMBERING）。

## 范围（P0）

1. `UbloxReceiver` 覆写 `navRatesHz()`/`outputPeriodsMs()`：
   - ZED-F9P 定位频率 `{1, 2, 5}` Hz（四星座 RTK 依 HPG 1.12 数据手册上限 8 Hz；10 Hz 待室外 HIL 后另议）。
   - 输出周期（ms，0=关）：GGA/GST/RMC `{0,200,500,1000}`，GSA `{0,1000}`，GSV `{0,1000,5000}`。
   - 合法规则：`period == 0` 或 `period % Tnav == 0 且 1 ≤ period/Tnav ≤ 255`（CFG-MSGOUT 是每 n 历元一次）。caps 与命令入口都按此过滤；`buildOutput` 遇非整数倍返回失败，不得静默取整（现在 5 Hz + 500 ms 会变成 600 ms）。
   - 修正 `caps()` 中 F9P 的 `cap::kRate20Hz` 声明（与上限不符），说明替代位或去掉的影响。
2. 改频率连带问题（报告 §2 的 4 个隐藏缺陷）：
   - `makePlan`（`GnssModuleProbe.cpp`）、`SettingsBackend.cpp` `currentPeriod`、`restoreOutputPeriods` 增加 ZED-F9P 分支：改频率后把各语句周期对齐到新的合法集合（不合法则取不小于原值的最近合法值，0 保持 0），并持久化。
   - `tickPendingApply` 频率回滚须同时恢复 CFG-RATE-MEAS（不只 `requestOutput`）。
   - 进入 `kConfiguring` 时只在从 `kProbing` 进入（冷启动/重探测）时置 `sat_replay_`/`ppp_replay_`，改频率不重放星座。
3. NMEA 高精度：在 `cmdConfigurePort`（`UbloxUbx.cpp`）的串口配置 VALSET 追加 `CFG-NMEA-HIGHPREC=1`、`CFG-NMEA-SVNUMBERING=1`，不改 `cmdConfigureReceiver`，保持 `test/gnss/golden/GoldenUblox.inc` 不变；若 HPG 1.12 NAK 则降级为非致命并记 W 日志。确认 NMEA 解析与手机/网页透传在 7 位小数（GGA 更长）下长度上限够用。

## 冻结文件

- 允许改：`components/gnss/include/gnss/drivers/UbloxReceiver.hpp`（只加两个 override 声明及注释），同步重算 `docs/frozen.sha256` 对应行。其他冻结文件不改；需要改的写进报告由 Architect 处理。

## 测试（每项修复配一个修复前会失败的回归测试）

- `test/gnss/TestGnssUbloxReceiver.cpp`：F9P 频率/周期集合、整数倍规则、`buildOutput` 拒绝非整数倍、串口配置帧含两新键（字节级）。
- `tools/main_host_check/GnssResetCheck.cpp`（F9P 模拟）：caps 含 `gnss.rate`/`gnss.out.*`；`config set gnss.out.gsv 0` 成功并下发 VALSET；频率 1→5 Hz 后 500 ms 周期被对齐且持久化；频率回滚恢复 CFG-RATE-MEAS；改频率不触发星座 VALSET。
- settings / web 相关测试按需更新。

## 门禁与报告

- 全量门禁：`ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix u3 --base 3bbd6d2`，记录结果（t13 ASan 偶发超时已知，单独重跑确认）。
- 干净基线体积对比（rtk_main.bin、IRAM、DRAM）。
- 报告 `docs/reports/U2-ublox-rate.md`：改动清单、设计取舍、测试映射、门禁结果、体积、未验证项、HIL 步骤（Architect 执行）。
- 不提交、不烧录、不连串口。
