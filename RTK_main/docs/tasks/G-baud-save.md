# G-baud-save：启动切速成功后保存一次到模块 Flash（修订 G-baud460k）

- 依据：用户 2026-11-19 同意“启动不发 SAVECONFIG”规则过于保守。
- 分支：`feat/gnss-optisens-q1`（在 G-optisens、Q1 之前或之间完成均可，报告单列）
- 构建前缀：`builds/gq-*`

## 1 现状问题

G-baud460k 启动切速成功后不发 `SAVECONFIG`，模块掉电回出厂 115200。每次冷上电：缓存 460800 首候选超时 420 ms → 115200 命中 → 再切 460800，约多 0.6 s。

## 2 要求

1. **何时保存**：仅当本次启动**实际发生了切速**（`alignBootBaud()` 发出了切速命令，即探测到的波特率 ≠ 目标）且切速验证成功时，
   紧接着请求一次模块保存（复用现有 `controller().requestSave()` / 驱动 `saveStep()` 路径，不新增命令拼装）。
   - 探测时已是目标速率（ESP32 单独复位、已保存过的冷上电）→ 不保存。
   - 结果：每块板只在首次（新板、`gnss freset` 之后）写一次模块 Flash。
2. **不写 NVS**：仍不写 `gnss/baud`（保持“用户未覆盖”语义，以后改 Kconfig 目标值仍能生效）；`gnss.cache` 照旧记录目标速率。
3. **保存失败**：NACK/超时只记日志与 trace（可复用现有 save 失败文本），**不**计入 `baud_fail`，不回退波特率，下次冷上电会再走一次切速+保存。
4. **与其它维护互斥**：保存走现有维护序列，不得与 rate/output/mode 配置交叠；启动 PPP/模式恢复等等待 `boot_baud_pending_`/`in_flight` 的逻辑需同时等待这次保存完成（或证明无需等待并在报告说明）。
5. **驱动**：Unicore/CM680 `SAVECONFIG`；ComNav 与 u-blox 使用各自已有的保存步骤（u-blox 若现有保存写 BBR/Flash 即沿用）；不支持保存的驱动返回 unsupported 时静默跳过。
6. **用户命令不变**：`gnss baud N` 仍写 `gnss/baud` + 保存；`gnss freset yes` 仍擦除 `gnss/baud` 与缓存、恢复模块出厂（115200）。

## 3 测试

- main_host_check：冷启 115200 → 切 460800 → 恰好 1 次 `SAVECONFIG`，且在切速验证之后；模块已在 460800（缓存命中）→ 0 次 `SAVECONFIG`；
  保存 NACK → `baud_fail` 不变、波特率保持 460800、状态 running。
- 更新 G-baud460k 中“启动不发 SAVECONFIG”的断言。

## 4 验收

- 门禁全 PASS（G1 例外仅限 G-optisens 授权文件）。报告 `docs/reports/G-baud-save.md`。
- HIL（Architect）：`gnss freset yes` 后 ESP32 复位 → 看到 `CONFIG COM1 460800` + `SAVECONFIG`；之后冷上电 → 首候选 `detected CM680 at 460800`，无切速、无 SAVECONFIG。
