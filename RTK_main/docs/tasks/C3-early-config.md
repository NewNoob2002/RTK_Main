# C3 启动早期配置与启动失败回退

- 依据：ADR-0014 §2.3（两段加载、启动失败计数）、§2.2（优先级/核心/栈不可配置）、§2.7（dbg.* 早期加载）。
- 基线：a2d70a1。实现：codex；Architect（Claude）验收、提交、重算 frozen。

## 1 目标

1. 新增启动步骤 `earlyConfig`，位于 `banner` 之后、`alloc` 之前（`main/app/BootFlow.cpp` kBootSteps）。
   - 调 `nvs_store_init()`（从 `StorageModule::init` 移到这里，后者不再重复初始化；失败仍按“缺省值继续”策略）。
   - 读 `dbg.*`（mask/report/hex/loglv）与 `gnss/baud` 到 `BootConfig`（新增 `main/def/BootDef.hpp` 或合适位置，纯值类型，constinit 存放于 AppContext/AppData），逐项校验，越界用缺省并打一行 W（指出键名与值）。
   - 早期即应用日志级别与 trace 掩码，使后续启动步骤的日志遵从用户设置；`stepDebug`/`stepStorage` 不得用缺省值覆盖早期已应用的值（两条路径只能有一个真源：BootConfig 由早期读取，storage 阶段对这些键不再二次加载或只做一致性校验，二选一并在报告中说明）。
   - `GnssModule` 打开 UART 时用 `BootConfig` 中的首选波特率（替代 C2 中 startController 内的 applyBaud 补丁或与其合并，保持“UART 已处于 bauds.front()”契约）。
2. 启动失败计数 `boot/fails`（u8，用户分区）：进入 `earlyConfig` 时 +1 并立即写入；`stepStarted` 中（boot done 日志之前或之后均可，说明理由）清 0。
   - 读到的旧值 ≥3 → 本次忽略全部用户配置（BootConfig 用缺省，storage 阶段各组也按缺省，不擦 NVS），打 W `Boot failed N times, using default settings`；`status`/`config` 能看到本次处于安全缺省模式（一行即可）。
   - 安全缺省模式下用户修改配置会写 NVS 吗？约定：允许写（用户在修复），下次正常启动生效；报告说明。
   - 计数写失败只打 W，不阻断启动。
3. 存储启动日志保持 C1 格式；新增一句 I（仅当 fails>0 时）`Previous boot did not complete (N)`。
4. 不改：任务优先级、核心、栈（不可配置）；分区表；frozen 公共 C API `components/nvs_store/include/NvsStore.h`（如必须新增函数，先在报告里提出，不要改）。

## 2 测试

- 主机：`tools/main_host_check` 新增用例：早期读取校验（越界→缺省+W 计数）、fails 0/2/3 三档行为、clear 路径、storage 阶段不覆盖早期值、NVS 初始化失败仍能启动。
- 门禁：idf `builds/c3` 0 warning；main_host_check、t13_host_check、test/gnss 8/8 + 全驱动、services、app_fw、nvs_store 全过（普通 + ASan）；G5b（tidy_check，含 --nvs-build）0；G7b PASS；clang-format；doc_check `--base a2d70a1` 0 不合格；bin 大小记录（增量 >2 KB 说明）。
- 计划中的 HIL（Architect 执行）：启动时间对比 C2（预计 <20 ms 增量）；用 `debug` 命令改 loglv/mask 后重启，确认启动早期日志已遵从；人为制造 3 次未完成启动（Architect 可用临时构建在 stepStarted 前 abort），确认第 4 次进安全缺省且之后正常启动清零。

## 3 交付

- 报告 `docs/reports/C3.md`：改动文件、设计取舍（§1.1 真源二选一、§1.2 清零时机）、门禁结果、未覆盖项。
- 不提交、不改 `docs/frozen.sha256`；完成后输出单独一行 `C3 DONE`。
