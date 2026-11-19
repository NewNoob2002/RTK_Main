# D1 控制台构建档位（DEV / PRODUCTION / NONE）

- 依据：ADR-0016 §2.1–§2.5；相关现状见 ADR-0009 §2.8、§2.10、§2.12。
- 基线：dd00492。实现：codex；Architect（Claude）评审、验收、提交、重算 frozen。

## 1 目标

1. **Kconfig**：`main/Kconfig.projbuild` 新增 choice `RTK_CONSOLE_PROFILE`（`_DEV` 默认 / `_PRODUCTION` / `_NONE`），help 用中文说明三档差异
   并引用 ADR-0016。`main` 内提供一个 `constexpr` 档位枚举（放 `main/app/` 私有头），业务判断用它，不散布 `CONFIG_` 宏。
2. **命令登记**（`main/app/ConsoleSetup.cpp` `registerCommandTables()`/`registerVerbGroups()`）：
   - DEV：现状不变。
   - PRODUCTION：只登记 `status`、`log`、`sys`、`config`、`factory`（隐藏）、`freset`，以及对应 help 动词组
     （`status log sys factory`）；`gnss`、`rtcm`、`bt`、`alarm`、`debug` 不登记。不拆实现文件，不改 `components/cli/include/**`。
   - `kConsoleOnly` 门控（safe = status/log/sys/config）不变。
3. **NONE 启动与降级**（`main/app/BootFlow.cpp`）：
   - `console` 步骤保留在 `main/app/BootStepList.inc`；NONE 下步骤体打 I `console disabled (profile none)` 并返回 kOk，不调用 `initConsole()`。
   - 成功路径：打完启动摘要、`attachDebugLog()` 后不调用 `runConsole()`。
   - 失败路径：打 E（同现在），置 `kConsoleOnly`，不调用 `initConsole()/runConsole()`，不重启。
   - 确认 NONE 构建 ELF 中无 `esp_console_new_repl_uart`、`linenoise` 符号（`nm`/`idf.py size-components` 任选，写进报告）。
4. **显示**：`stepBanner` 加一行或一个字段 `console profile dev|production|none`；`config build` 输出同一字段。
5. **叠加文件**：新增 `configs/production.defaults`、`configs/console-none.defaults`（内容见 ADR-0016 §2.5）。
6. **门禁**：`tools/ci/run_gates.sh` 增加 `G3-idf-production`、`G3-idf-console-none`（写法同 `G3-idf-trace-off`，用 `configs/*.defaults`）。
   `.github/workflows/gates.yml` 的 idf 作业走 `run_gates.sh --only-idf`，自动覆盖，无需改。
7. **实测**：填 ADR-0016 §3 的表到报告（不改 ADR，Architect 抄回）：三档的 DRAM 静态、flash code/data、镜像大小
   （`idf.py size`），空闲堆列写 NOT_RUN（需上板，Architect 测）。
8. 不改：任务参数、分区表、NVS 键、蓝牙名、`sdkconfig.defaults`、`components/*/include/**`、factory 密钥默认值。
   需要改冻结文件时停下，在报告中写明理由。

## 2 测试

- 主机：`tools/main_host_check` 中与登记清单相关的检查（若已有 ConsoleSetup/命令表检查，扩展之；没有则新增
  `console_profile_check`）：对三档分别断言登记的命令名集合与 ADR-0016 §2.3 一致、PRODUCTION 不含 `gnss/rtcm/bt/alarm/debug`、
  safe 集合不变。登记清单抽成可在 host 编译的纯函数（返回名字表）以便测试，固件与测试共用同一份清单。
- `BootStepsCheck`：NONE 下步骤表仍含 `console`，失败路径不调用 console 钩子（用现有 hook 方式）。
- 门禁（`tools/ci/run_gates.sh` 全部）：三档 IDF 构建均 0 warning；host 普通 + ASan/UBSan PASS；G5、G5b、G6（`--base dd00492`）、G7b PASS。
- 计划中的 HIL（Architect 执行，Developer 不烧录）：
  1. P2 v1.41 + DEV：与 dd00492 行为一致（`help` 列表、`gnss status`）。
  2. P2 + PRODUCTION：`help` 只列 status/log/sys/config/freset（factory 隐藏）；`gnss status` → `ERR unknown command`；
     `factory unlock` 后 `factory sn` 可写；GNSS/蓝牙正常运行；记录空闲堆。
  3. P2 + NONE：启动日志完整输出到 UART0、无提示符、输入无回显；GNSS/蓝牙正常；记录空闲堆。
  4. NONE + 强制某步失败（`CONFIG_RTK_BOOT_FAULT_STEP`）：打 E 后不重启，无提示符。

## 3 交付

- 报告 `docs/reports/D1.md`：改动文件清单、登记清单的组织方式、三档实测表、门禁命令与结果（PASS/FAIL/NOT_RUN/BLOCKED，如实）、未覆盖项。
- 不提交、不改 `docs/frozen.sha256`；完成后单独一行输出 `D1 DONE`。
