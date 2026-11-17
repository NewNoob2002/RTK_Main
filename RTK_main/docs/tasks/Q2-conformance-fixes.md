# Q2：Q1 审计结论落地（代码修复）

状态：ISSUED 2026-11-17，基线 `feat/gnss` HEAD（见 `git log -1 -- docs/tasks/Q2-conformance-fixes.md`）。
实施：omp + workbuddy/deepseek-v4.1-flash；规划/验收：Claude。

## 0 背景与已定决定

Q1 审计（`docs/reports/Q1.md`）的 7 条待决提议已由用户确认（2026-11-17），文档部分已在本卡同一提交里落地（Q2-T0）：

| 提议 | 决定 | 文档位置 |
| --- | --- | --- |
| P1 | `app_fw` 是框架基座，任何自有组件可依赖 | AGENTS.md §3 |
| P2 | Registry 地址表改常量初始化；g7b 禁止自有目标文件引用 `__cxa_guard_*` | docs/acceptance.md G7b、CODING-STYLE §10 CPP-009 |
| P3 | `static_cast` 注释只要求窄化/符号变化/指针↔整数/枚举↔整数/`void *` 恢复；存量不批量补 | CODING-STYLE §4.5 |
| P4 | 生成文件不参加格式门禁，首行写明生成脚本 | CODING-STYLE §9、acceptance G5 |
| P5 | `docs/frozen.sha256` 只由 Claude 重算 | AGENTS.md §4 |
| P6 | 本期不做 OTA，分区表不动 | ADR-0002 §9 |
| P7 | 无托管组件，不生成 `dependencies.lock` | .gitignore 注释 |

**本卡只做代码。** `AGENTS.md`、`docs/**`（`docs/reports/` 除外）、`components/*/include/**`、`partitions.csv`、`sdkconfig.defaults`、
`.clang-*`、`.gitignore`、`docs/frozen.sha256` 一个字节都不改（AGENTS §4）。不 `git add/commit/stash/reset`。

先读：`AGENTS.md`、`docs/CODING-STYLE.md`（§2 文件头、§4.5、§5.3、§6.2 doxygen、§10）、`docs/acceptance.md`、`docs/reports/Q1.md`。
定位代码先用 `codegraph explore "<符号>"`。

## 1 任务

### T1 GNSS 驱动表常量初始化（Q1-A07，CPP-009）

现状：`components/gnss/src/Registry.cpp:48` `enabled_receivers()` 内
`static const std::array<const Receiver *, kEnabledCount> kList{&accessor()...}`：accessor 不是 constexpr，
所以是动态初始化，生成 `__cxa_guard_acquire/release`，与文件头"编译期生成"矛盾。

做法：
- 新增私有头 `components/gnss/src/DriverInstances.hpp`（不冻结），在 `namespace gnss::detail` 里按启用开关声明
  `extern const UnicoreReceiver kUnicoreReceiver;`、`extern const Um982Receiver kUm982Receiver;`、
  `extern const UbloxReceiver kUbloxReceiver;`、`extern const ComnavReceiver kComnavReceiver;`（命名与 `DriverList.inc` 的 `type` 参数一致：`k##type`）。
- 三个驱动 `.cpp` 把匿名命名空间里的 `constinit const X s_xxxReceiver{}` 改成 `detail` 命名空间里的
  `constinit const X kXxxReceiver{}`（外部链接），accessor 返回它；accessor 签名、公共头不变。
- `Registry.cpp`：kList 移到命名空间作用域 `constinit const std::array<const Receiver *, kEnabledCount> kList{ &detail::k##type, ... }`
  （X-macro 用 `type` 参数拼名，不再调用 accessor），`enabled_receivers()` 返回它。修正文件头/注释使其与实现一致。
- `DriverList.inc` 是冻结公共头，**不改**。所有驱动组合（all/unicore-only/ublox-only/comnav-only 等 test/gnss 现有选项）都要能编。
- 验证：`xtensa-esp32-elf-nm builds/q2/esp-idf/gnss/CMakeFiles/__idf_gnss.dir/src/Registry.cpp.obj` 不出现 `__cxa_guard` 与 guard 变量（`_ZGV`）。

### T2 g7b 增加 `__cxa_guard` 检查（P2）

`tools/g7b_check/g7b_check.py`：对自有目标文件（现有 `object_sources()` 映射到的仓库源文件）统计 `__cxa_guard_acquire/__cxa_guard_release/__cxa_guard_abort` 的未定义引用，
有则 FAIL 并列出源文件。输出风格与现有异常/RTTI 检查一致。
负向验证：在报告中给出一次"故意引入函数内动态静态 → g7b FAIL"的证据（用临时目录或临时文件做，做完恢复，最终 diff 不得残留）。
如果现在除 Registry 外还有别的自有目标文件命中，逐个列出并修掉（改成常量初始化）；修不了停下写进报告。

### T3 控制台输出锁一次性初始化（Q1-A06，C-001、ESP-001）

现状：`components/cli/src/Out.cpp:35-58` `OutLock` 用 `SemaphoreHandle_t volatile handle_` 双重检查，并且在
`portENTER_CRITICAL` 里调用 `xSemaphoreCreateRecursiveMutexStatic`（临界区内调用 FreeRTOS API，违反 ESP-001）。

做法（仅 `ESP_PLATFORM` 分支；host 分支 `std::recursive_mutex` 不动）：
- `std::atomic<uint32_t> state_`：0 未初始化 / 1 初始化中 / 2 就绪；`static_assert(std::atomic<uint32_t>::is_always_lock_free)`。
- `lock()`：`state_.load(acquire) != 2` 时，`compare_exchange_strong(0→1, acq_rel)` 成功者调用 `xSemaphoreCreateRecursiveMutexStatic`
  写普通成员 `handle_`，再 `state_.store(2, release)`；失败者循环 `vTaskDelay(1)` 直到读到 2（让出 CPU，避免同核低优先级创建者被饿死）。
  然后照旧 `xSemaphoreTakeRecursive`。去掉 `portMUX_TYPE init_mux_` 与临界区。
- 静态创建在 storage 非空时不会失败，但仍要处理返回值（不用断言替代检查）：创建者无论结果都 `store(2)`，避免后来者永久等待；
  `handle_ == nullptr` 时 lock/unlock 变为空操作（退化为无锁输出），在注释里说明。
- 更新文件头第 4–6 行与 `OutLock` doxygen。

### T4 GnssDef 魔数（Q1-A08）

`main/def/GnssDef.hpp:108` `std::array<uint32_t, 6U> rx_period_ms{}` → 用 `data::kGnssAppMsgCount`（先确认它在哪个头定义、include 是否成环；成环就把常量移到不成环的位置，或在 GnssDef 定义并让原处引用）。
`main/modules/gnss/GnssModule.cpp:1014` 的 static_assert 保留或按需调整。注释里"App 6 类语句"改成不含魔数的说法。

### T5 原子变量无锁断言（CPP-008）

给以下共享原子补 `static_assert(std::atomic<T>::is_always_lock_free, "...")`（放在声明旁或 .cpp 内）：
`main/AppContext.hpp:67-69`、`components/app_fw/src/Tlog.cpp:31,34`、`components/app_fw/src/Trace.cpp:23,27`、`components/app_fw/src/Log.cpp:27,31,32`。
`BootPhase` 是 `enum class : uint8_t`；若 xtensa 上 `std::atomic<BootPhase>` 不是 always lock free，把底层类型改成 `uint32_t` 并说明。
app_fw 是纯组件，host（x86-64）与 xtensa 两边都要过。

### T6 文件头补齐（Q1-A04，CODING-STYLE §2）

以下 19 个文件文件头不足两行：第一行用途，第二行起写谁调用/可否阻塞/ISR 安全（头文件写线程约束；纯定义头写"只含定义，无运行期状态"之类即可）。只改注释，不改代码。
`main/AppConfig.hpp`、`main/def/DebugDef.hpp`、`main/def/LinkDef.hpp`、`main/def/RtcmDef.hpp`、`main/def/StorageDef.hpp`、`main/def/SystemDef.hpp`、
`main/modules/bt_link/BtLinkModule.cpp`、`main/modules/rtcm/RtcmForwarder.cpp`、`main/modules/rtcm/RtcmForwarder.hpp`、`main/modules/rtcm/RtcmModule.cpp`、`main/modules/rtcm/RtcmModule.hpp`、
`main/modules/storage/StorageModule.hpp`、`main/modules/storage/StoreCatalog.cpp`、`main/modules/system/SystemModule.hpp`、`main/modules/system/SystemModule.cpp`、
`components/link_core/AlarmSource.cpp`、`components/rtos/EspStatus.cpp`、`components/stream_demux/src/Demux.cpp`。
`components/link_core/include/core/Types.hpp` 是冻结公共头：**不改**，在报告中列为"冻结，留给 Claude"。
内容须与代码一致（看实际调用者与线程，不要编）。

### T7 其他

- `components/cli/CMakeLists.txt` 第 3 行说 `src/Out.cpp` "只用 C/C++ 标准库（无 IDF 头）"，与 `ESP_PLATFORM` 分支不符，改准确（cli/nvs_store 的第 1–2 行已由 Q2-T0 改好，不要再动）。
- T1–T6 新增/改动的函数都要 doxygen（§6.2，`tools/doc_check`）。

## 2 门禁（全部跑，报告逐条写命令与结果）

构建目录一律放 `builds/q2*`。
1. host：AGENTS §5 列出的每个目录普通 + ASan/UBSan 各一次；`test/gnss` 覆盖现有全部驱动选项组合；`tools/main_host_check`、`tools/t13_host_check`。
2. `idf.py -B builds/q2 build` 0 warning；关闭 TRACE 再构建一份（与 P3c 报告同法）；镜像字节与 `builds/p3c` 对比。
3. `python3 /home/gtc/.claude/skills/embedded-engineering/scripts/cpp_cost.py --prefix xtensa-esp32-elf- diff builds/p3c/rtk_main.elf builds/q2/rtk_main.elf`（acceptance G4）。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/q2` PASS，且 `__cxa_guard` 引用 0；T2 负向证据。
5. tidy_check（acceptance G5b，构建目录换成 builds/q2*）0 发现。
6. `clang-format --style=file --dry-run --Werror` 改动文件 0 差异；`tools/doc_check` PASS。
7. `sha256sum -c docs/frozen.sha256` 全 OK（证明没碰冻结文件）。
8. golden：`git diff --stat` 不含 `*.inc` golden 与 `test/host/**`。

## 3 交付

报告 `docs/reports/Q2.md`：每个 T 的改动（文件:行）、取舍、门禁结果表（PASS/FAIL/NOT_RUN/BLOCKED）、偏离与未决。
HIL 不做（由 Claude 烧录冒烟）。不提交。完成后终端打印 `Q2 DONE`。
