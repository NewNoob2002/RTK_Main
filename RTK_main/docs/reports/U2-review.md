# U2 交叉评审（Architect 执行）

原计划由 omp neon 交叉评审，该会话因 Antigravity 配额耗尽中断；Codex 启动时报 `workspace routing discovery timed out`。评审改由 Architect 直接完成。

## 发现

| ID | 严重度 | 位置 | 问题 | 处理 |
| --- | --- | --- | --- | --- |
| RU2-01 | major | `components/gnss/src/ControllerConfigure.cpp` `configAwait` 降级分支 | 5 键端口 VALSET 被 NAK 后降级为 3 键，降级分支把 `step_tries_` 清零；若 3 键也被 NAK，`configRetryOrFail` 重试会回到 `configSend` 重新生成 5 键帧，再 NAK → 降级 → 清零，永不进入 `kFailed`，控制器卡在配置态 | 已修：不清零 `step_tries_`，降级失败计入重试预算。回归测试 `testF9pControllerPortConfigFallbackNakFails`：修复前 400 轮后仍未失败（`Expected 5 Was 3`），修复后进入 `kFailed` 且端口 NAK ≤ 4 次 |
| RU2-02 | minor | `main/modules/gnss/GnssModuleProbe.cpp` 改频率保存 | 改频率时先存 rate 键再存 out 键；若 rate 存成功而 out 存失败，RAM 回滚到旧频率，NVS 却留着新频率 + 旧周期，下次开机可能遇到非整数倍周期被 `buildOutput` 拒绝 | 未改。NVS 写失败本身少见；建议后续把两键放入一次 `saveKeys` 原子提交，记入 PROGRESS |

## 已核对无问题

- 整数倍规则：caps（`fillOutputPeriodDomain`）、控制台 `gnss out`（`isF9pPeriodValid`）、设置/网页（`SettingsBackend.cpp` 对 `kZedF9p` 走同一周期域）、`buildOutput` 拒绝非整数倍。
- 频率回滚 `requestReconfigure` 恢复 CFG-RATE-MEAS；`sat_replay_` 只在 `kProbing → kConfiguring` 置位，`requestReconfigure` 仅有改频率与回滚两个调用方，星座不会被重放。
- 7 位小数 GGA 约 92 B，远低于 NMEA 解码上限 `kMaxFrame = 256`；`NmeaParserSatellites` PRN 上限 255。
- `frozen.sha256` 只改 `UbloxReceiver.hpp` 一行。

## 结论

修复 RU2-01 后 **PASS**（门禁结果见 `U2-ublox-rate.md` 补充段）。实板项仍 NOT_RUN。
