# B1 板级识别、配置表与 P2 电源保持

- 依据：ADR-0015 §2.1–§2.5、§3（P2 v1.41 引脚表）。
- 基线：290c9c2。实现：codex；Architect（Claude）评审、验收、提交、重算 frozen。
- 只读参考：P2 V2.7 Arduino 固件（路径见 ADR-0015 抬头），只看行为，不复制代码。

## 1 目标

1. **配置表（纯 C++，无 IDF 头）**：新增 `main/board/BoardProfile.hpp/.cpp`，命名空间 `app::board`。
   - 类型与字段按 ADR-0015 §2.3；表 `constexpr`，两项：`kP2V141`（窗口 [1500, 1800] mV，GNSS UART2 RX16/TX17、
     无 GNSS 电源脚，`powerHold = kKeyEmulate`{keyGpio 4, senseGpio 25, senseOnMv 1600, holdTimeoutMs 5000}，
     `safeOutputs` 按 §2.5，`features` = Cellular|Oled|Imu|Charger|Beeper|Motor|Leds），`kE1DevKit`（窗口 0/0，
     GNSS UART2 RX16/TX17、电源 GPIO25 高有效，`powerHold = kLatchGpio`{GPIO26 高}，safeOutputs 空，features 0）。
   - `const Profile *matchById(uint32_t mv) noexcept`（无匹配返回 nullptr；E1 不参与匹配）、
     `const Profile *byId(BoardId) noexcept`、表遍历接口（供 host 测试检查窗口规则）。
2. **识别步骤 `identify`**：`kBootSteps` 第一项，`OnFail::kDegrade`。
   - 把 `installDirectLog()` 从 `stepPower` 移到这里，先装日志再读 ADC。
   - ADC1 oneshot 读 GPIO35（ADC1_CH7），12 dB、12 bit、line fitting 校准；丢首次，8 次平均，换算 mV；读完释放 ADC 句柄
     （或说明为何保留）。校准不可用 → 理想线性换算 + 一行 W。
   - Kconfig `RTK_BOARD`（choice，`main/Kconfig.projbuild`）：`RTK_BOARD_AUTO`（默认）/ `RTK_BOARD_FORCE_E1_DEVKIT` /
     `RTK_BOARD_FORCE_P2_V141`。强制时仍读一次并打印 mV，不据此判断。
   - 成功：打 I `board <name> id=<mV> mV`（强制时加 `(forced)`），把 `const board::Profile *` 存入 `AppContext`，
     设置 `safeOutputs`。失败：E `board unknown id=<mV> mV` 或 `board id adc failed: <err>`，返回 kFail。
   - 访问函数：`const board::Profile &app::board::current() noexcept`（启动后调用；未识别时调用视为编程错误，断言 +
     返回值安全，说明选择）。
3. **电源步骤 `power`**：按 `powerHold` 分派。
   - `kLatchGpio`：保持现有行为（E1）。
   - `kKeyEmulate`：ADR-0015 §2.4 开机流程（ADC2 读 GPIO25、每 10 ms、连续 3 次 ≥ senseOnMv、超时 kFail，任何退出路径
     都把 keyGpio 拉低）。判定逻辑抽成纯函数（输入 mV 序列/时间，输出 继续/成功/超时），放 `BoardProfile.cpp` 或同目录纯文件，
     便于 host 测试。
   - 每次读数不刷屏：成功时打一行 I `power hold ok sense=<mV> mV after <ms> ms`；超时打 E 带最后读数。
4. **GNSS 引脚改读配置表**：删除 Kconfig `RTK_GNSS_RX_GPIO`/`RTK_GNSS_TX_GPIO`/`RTK_GNSS_POWER_GPIO`/
   `RTK_GNSS_POWER_ACTIVE_HIGH`，所有引用（`GnssModuleProbe.cpp`、`GnssModuleCmds.cpp`、`ConfigCmds.cpp`、
   `tools/main_host_check/CMakeLists.txt` 等，`rg` 全量查）改用 `board::current().gnss`。`RTK_GNSS_UART_NUM` 保留；
   配置表 UART 号与之不等 → `identify` 返回 kFail 并打 E（host 测试同时静态检查表项）。
5. **显示**：启动横幅与 `status` 各加一行/一字段显示板名；`config` 中 gnss 引脚显示来源于配置表。蓝牙名不变。
6. **文档**：`BootSteps.hpp` 中步骤数注释同步为实际值（现已漂移：写 13 实为 14，加 identify 后 15）。
7. 依赖：`main/CMakeLists.txt` 增加 `esp_adc`（IDF 自带组件，不是联网安装）。
8. 不改：任务优先级/核心/栈、分区表、NVS 键、蓝牙名、`components/*/include/**`、`sdkconfig.defaults`。
   需要改冻结文件时停下，在报告中写明理由。

## 2 测试

- 主机（`tools/main_host_check` 新增 `board_profile_check`，加入 ctest）：
  - 表规则：窗口互不重叠、间隔 ≥ 100 mV、都在 [150, 2450]；`kE1DevKit` 不被 `matchById` 命中；每项 UART 号 ==
    `CONFIG_RTK_GNSS_UART_NUM`；safeOutputs 不含 GNSS 引脚、不含 GPIO34–39（仅输入）、无重复。
  - `matchById`：1499/1500/1650/1800/1801/0/3300 边界。
  - 电源保持判定：直接成功、读数抖动（2 次达标后跌落再达标）、恰在超时边界、全程不达标 → 超时。
  - `BootStepsCheck`：步骤顺序 identify→power→banner…；identify 失败走降级路径。
- 门禁（`tools/ci/run_gates.sh` 全部）：idf `builds/b1` 0 warning；host 全部套件普通 + ASan/UBSan PASS；G5b、G7b PASS；
  clang-format；doc_check `--base 290c9c2` 0 不合格；bin 大小记录（增量 >4 KB 说明）。
- 计划中的 HIL（Architect 执行，Developer 不烧录）：
  1. P2 v1.41：打印 ID mV 落入窗口；电池供电下松开按键仍保持供电；记录 sense mV 与保持耗时，校准 `senseOnMv`。
  2. P2 上 4G/蜂鸣器/马达/LED 启动后保持关闭。
  3. `RTK_BOARD_FORCE_E1_DEVKIT` 构建在 E1-5.4 上行为与 290c9c2 一致（GNSS 正常、电源保持正常）。
  4. 断开分压（或 USB 供电下）验证未知 ID → console-only，无任何输出脚被驱动。

## 3 交付

- 报告 `docs/reports/B1.md`：改动文件清单、设计取舍（`current()` 未识别处理、ADC 句柄生命周期、纯判定函数接口）、
  门禁命令与结果（PASS/FAIL/NOT_RUN/BLOCKED，如实）、bin 大小、未覆盖项。
- 不提交、不改 `docs/frozen.sha256`；完成后单独一行输出 `B1 DONE`。
