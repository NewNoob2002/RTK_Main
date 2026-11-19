# W1 交叉评审（omp）

- 评审对象：worktree `RTK_main-w1` 未提交改动（`git diff` + 新增 `main/app/RadioMode.hpp`、`main/app/RadioMode.cpp`、`tools/main_host_check/RadioModeCheck.cpp`）。
- 对照：`docs/tasks/W1-radio-mode.md`、`docs/adr/ADR-0017-wifi-web-config.md`、`AGENTS.md`。
- 方式：只读代码审阅 + 复用作者已构建产物做独立核对（§4）。未改源码，未 commit，未烧录，未跑 HIL。

## 1 结论

| 级别 | 数量 | 内容 |
|---|---|---|
| block | 0 | 未发现阻断合入的问题 |
| should | 2 | S1 `radio` 命令在 console-only 被误门控；S2 wifi 模式不存在白名单/配对窗口写入路径（W2 依赖，需 Architect 决策） |
| suggest | 5 | U1 功能关闭构建不清除 RTC 记录；U2 OK 已打印但 drain 失败时不重启；U3 释放 BTDM 后的 API 约束未写明；U4 `SystemModule.cpp` 文件头注释过时；U5 G5 格式门禁不含未跟踪文件 |

## 2 should

### S1 `radio` 在 console-only 阶段被门控，wifi 降级后没有文档中的返回路径

- 位置：`main/app/ConsoleRegistration.hpp:22-25`（`isSafe` 只有 status/log/sys/config）、`main/app/ConsoleSetup.cpp:150-160`（`kConsoleOnly && !gate.safe` 直接拒绝）、`main/app/BootFlow.cpp:161-183`（`stepRadio` 失败即降级，此时 `radioMode` 已置 `kWifiConfig`，RTC 记录已在 164 行被消费清除）。
- 具体失败场景：`radio wifi` 重启后 `esp_bt_controller_mem_release()` 返回非 `ESP_OK`（`BootFlow.cpp:171-177`，kDegrade），或 `debug/storage/system/link/io` 任一步失败 → `bootPhase = kConsoleOnly`。设备处于“BT 未起、Wi-Fi 未起（W1 还没有 SoftAP）”的状态；操作者按任务卡 §2/HIL 步骤 5 输入 `radio bt`，得到 `ERR unavailable: console-only`。实际可用的只有未写入 HIL 步骤的 `sys reboot`（`main/modules/system/SystemCmds.cpp:311-317`，`sys` 属于 safe）或断电。
- 影响：正常的 wifi 启动（未降级）不受影响；但 W1 自己定义的降级路径恰好落在无法用 `radio` 返回的状态，与“`radio bt` 是退出方式”的文档口径不一致。
- 建议：把 `radio` 加入 `isSafe`（同步修改 `tools/main_host_check/ConsoleProfileCheck.cpp` 的 `kSafe` 断言）。风险很低：`cmdRadioStatus` 只读 `radioMode/radioReason`，`cmdRadioSwitch` 只写 RTC 记录并 `esp_restart()`（`main/app/ConsoleSetup.cpp:39-73`），不依赖任何模块、缓冲或执行器。

### S2 wifi 模式没有白名单/配对窗口的写入路径（W2 前置缺口）

- 位置：`main/modules/bt_link/BtLinkCmds.cpp:216-231`（白名单增删命令 target 为 `kBtLink`）、`main/modules/bt_link/BtLinkCmds.cpp:103-118`（配对窗口同样走 `kBtLink`）、`main/def/StorageDef.hpp:14`（`StorageVerb` 只有 `kSaveSn`，存储模块不接受白名单命令）、`main/modules/system/FactoryCmds.cpp:151,197,218`（wifi 下显式返回 unavailable）、`main/support/ConsoleSupport.cpp:108-115`（任何 `kBtLink`/`kRtcm` 目标在 wifi 下被 `requireBluetooth()` 拒绝）。
- 具体失败场景：ADR-0017 §2.6 要求网页在 wifi 模式修改“白名单增删、下次蓝牙启动时打开配对窗口”，并按 §2.5 通过 `fw::Command` 提交。W2 若复用现有动词，提交 `LinkVerb::kWhitelistAdd` 会得到 `ERR unavailable in wifi mode`（且 `bt_link` 根本不在 wifi 模块表里，`moduleOk` 也会拒绝）；`StorageModule` 没有对应动词。结果是 W5 的网页白名单功能无法实现。另：配对窗口只有运行期状态（`setPairing`），NVS 中没有“下次启动开窗”的键，ADR 的“下次蓝牙启动时打开配对窗口”目前无实现载体。
- 说明：W1 任务卡 §1.4 明确要求 factory 白名单在 wifi 下返回不可用，所以这段代码不算违反 W1 卡；这是提给 Architect 的跨卡依赖：需要在 W2 前指定“wifi 模式的白名单写入动词（存储/NVS 层）与配对窗口持久化键”，否则 §2.6 无法按现有命令总线实现。

## 3 suggest

### U1 `RTK_WIFI_CONFIG_ENABLE=n` 构建不清除 RTC 记录

- 位置：`main/app/RadioMode.cpp:21-24`（`s_radio_record` 仅在功能开启时声明）、`main/app/RadioMode.cpp:114-121`（关闭时 `consumeRadioRequest()` 直接返回 `{}`，不读写 RTC）。
- 具体场景：wifi 固件写下有效请求后，在走到 `stepRadio` 之前失败/复位（例如 `CONFIG_RTK_BOOT_FAULT_STEP` 注入 `banner/earlyConfig`、或电源保持失败），RTC 记录保留；随后烧写 wifi-off 固件，该构建永远不清除记录；再烧回 wifi 固件时，若首次启动被报告为 `ESP_RST_SW`，`consumeRadioRecord` 会接受这条陈旧请求并进入 wifi 模式。台架上在 W 系列之间来回烧写时有人工触发可能。
- 建议：无论 Kconfig 取值都声明 `.rtc_noinit` 记录（+4 B RTC），启动时先清零再判断；或至少在 `#else` 分支也清零。

### U2 `OK` 已打印但 UART 排空失败时不重启

- 位置：`main/app/ConsoleSetup.cpp:67-71`。
- 具体场景：控制台驱动缺失/端口不匹配或 TX 卡住时，`uart_wait_tx_done` 超时（500 ms）；操作者看到 `OK` 紧接 `ERR UART drain failed: ...`，设备留在原模式。只盯着 `OK` 的自动化/操作员会误判切换已开始；与 `sys reboot`（`SystemCmds.cpp:313-316`，固定 100 ms 后无条件重启）行为不一致。
- 建议：把“排空失败”按警告处理并仍然 `requestRadioMode`（请求已表达），或调整顺序使 `OK` 只在确定重启时出现。

### U3 释放 BTDM 后不得再调用 `esp_bt_*` 的约束没有写下

- 位置：`main/app/BootFlow.cpp:171`；当前 wifi 路径确实没有其它 `esp_bt_*` 控制器调用（`BootFlow.cpp:464-482` 的 `esp_bt_dev_get_address()` 只在 BT 分支，`components/spp_server/SppServerInitBt.c:59-75` 只在 BT 模式）。释放后 `btdm_controller_status`、控制器 BSS/data 已归还堆，任何 `esp_bt_controller_*`/`esp_bt_dev_*` 调用都是 use-after-free。
- 具体场景：W2/W6 在 wifi 路径为了 SSID 或绑定信息调用 `esp_bt_dev_get_address()`/`spp_server_init()` → 读已释放控制器内存 → assert/崩溃（ADR-0017 §1 记录的旧固件同类问题）。
- 建议：在 `stepRadio` 或 `RadioMode.hpp` 的模式选择注释里写明“`kWifiConfig` 释放后禁止任何 `esp_bt_*` 控制器 API”，并保持 `startBt`/`printBluetoothSummary` 的门控（现状正确）。

### U4 `SystemModule.cpp` 文件头注释过时

- 位置：`main/modules/system/SystemModule.cpp:1` 仍写“prepare() 解析 SN、生成设备名并初始化蓝牙”，行为已拆到 `prepare()` + `startBt()`（`:69-93`，调用点 `main/app/BootFlow.cpp:314-317`）。按 AGENTS“更新契约注释”要求改一行即可。

### U5 G5 格式门禁不含未跟踪文件

- 位置：`tools/ci/run_gates.sh:77-80` 的 `fmt_check` 只用 `git ls-files`（仅已跟踪），而本卡三个新文件当前都是 `??`（`main/app/RadioMode.{hpp,cpp}`、`tools/main_host_check/RadioModeCheck.cpp`）。作者报告里的 G5 PASS 实际没有覆盖它们。
- 我手工补跑：`clang-format --style=file --dry-run --Werror` 对全部改动 + 新文件通过（§4）。建议把 `fmt_check` 改为 `git ls-files --cached --others --exclude-standard`，否则后续每张卡的未提交新文件都会漏检。

## 4 独立核对（复用作者产物，只读命令）

| 检查 | 结果 |
|---|---|
| `cmake --build builds/w1-main_host_check` + `ctest` | no work to do；64/64 PASS，含 `radio_mode`（Case 60） |
| `python3 -I tools/doc_check/doc_check.py --base 08a9be8` | 改动函数 50 个，不合格 0 |
| `clang-format --style=file --dry-run --Werror`（改动 + `git ls-files --others`） | PASS；`git diff --check` clean |
| `sha256sum -c --quiet docs/frozen.sha256` | PASS（冻结输入未改） |
| `builds/w1-idf/sdkconfig` / `builds/w1-wifi-off/sdkconfig` | 默认 y、wifi-off 为 `# CONFIG_RTK_WIFI_CONFIG_ENABLE is not set` |
| `.rtc_noinit`（map + elf） | 默认 `NOBITS 0x50000000 size 4`（RadioMode.cpp.obj）；wifi-off size 0；elf 段为 NOBITS，不被 boot 加载 |
| `strings rtk_main.bin \| grep 'radio \[wifi\|bt\]'` | 默认 1 条，wifi-off 0 条 |
| 镜像大小 | `w1-idf` 872,096 B，`w1-wifi-off` 871,088 B（与报告一致） |
| 构建产物与源码时间 | `RadioMode.cpp.obj`/`rtk_main.bin`（12:03）晚于源码（12:02），产物对应当前树 |
| G3 构建日志 | `builds/w1-logs/*.build.log` 中 `warning:`/`error:` 计数均为 0（`G3-*.log` 为空是 `idf_build` 把输出重定向到 build log 的设计） |

## 5 重点项核对结论（未发现问题）

- **RTC 记录 / 复位原因**：`consumeRadioRecord` 先读后清零再返回（`RadioMode.cpp:98-103`），只接受 `ESP_RST_SW`；对 `ESP_RST_POWERON`/`BROWNOUT`/`PANIC`/`TASK_WDT`/`INT_WDT`/`WDT`/`DEEPSLEEP`/`EXT` 一律回落 bt，比任务卡（只点名 POWERON/BROWNOUT）更严格，符合 ADR §2.2。核对 IDF：`components/esp_system/port/soc/esp32/reset_reason.c:17-50`（CPU0_SW→SW，panic/WDT/brownout 走 hint）、`task_wdt.c:380`（TWDT 置 hint）；`esp_restart()` 走 CPU0_SW→`ESP_RST_SW`。`.rtc_noinit` 为 NOBITS、软件复位不清除；`constinit` 不会把清零写入启动流程。
- **启动顺序 / 降级**：`radio` 在 `earlyConfig` 之后、`contexts` 之前（`BootStepList.inc:7`），先于 `stepAlloc`（`BootBuffers.cpp:56` 读取 `radioMode`）、`stepBind`、`stepSystem` 的 `startBt`；BTDM 释放在任何 `esp_bt_*` 控制器调用之前，释放的内存先回到堆再由 BootArena 分配。释放失败 → kDegrade → console-only，不初始化 BT；`sys reboot`/断电可回 bt（记录已清）。
- **SPP 拆除/绑定**：`bindBootBuffers` 的 `spp_server_bind_storage`、`rollbackBootBindings` 的 unbind、`stopBootTasks` 的 `spp_server_stop_boot` 都按模式跳过（`BootBuffers.cpp:157,181`、`Executors.cpp:218`），`spp.*` 与 `task.spp_worker` 在 wifi 下不分配（`BootBuffers.cpp:56-120`）。W0 分配检查 `BootAllocCheck::testWifiAllocationPlan` 覆盖。
- **kLink 执行器**：wifi 下 `maxMs != 0` 时 `vTaskDelay(20 ms)`，`pending` 时 `maxMs=0` 不等但每轮末尾仍有 `vTaskDelay(yieldTicks=1)`（`Executors.cpp:104-117`），不会忙等/触发 TWDT（每轮 `esp_task_wdt_reset`）；控制台命令延迟 ≤20 ms，远小于 `kCommandTimeoutMs=1000`（`AppConfig.hpp:17`）。`awaitBoot` 停止路径保留（`:106-109`）。
- **模块启用表**：第 7 列同时接入 `AppContext` 的计数/成员/`bindContexts`（`AppContext.hpp:89,120,140-146`）与 `Executors.cpp` 的两个 `consteval` 表；`bt_link`/`rtcm` 为 `kBtOnly`，wifi 表为 storage/system/gnss/debug（`RadioModeCheck.cpp` 静态断言两种集合）。alloc/bind 由 `moduleEnabled` 和 `BootBuffers` 覆盖，init/start/poll 由执行器表覆盖。
- **wifi 下触 BT 的控制台路径**：`status`（overview/link/rtcm/all）在 wifi 输出 `unavailable in wifi mode` 并继续 sys/gnss；`bt`、`alarm`、`rtcm`、`factory wl-*`/`pair`/`alarm-*` 均显式 `requireBluetooth()`，`submitRaw` 对 `kBtLink`/`kRtcm` 目标再兜底一次（`ConsoleSupport.cpp:108-115`）。`config link` 不是问题：`policy` 来自编译期 `policyFromConfig()`，白名单由 `StorageModule::init` 发布（`StorageModule.cpp:253`），wifi 下显示正确。`debug report` 在 wifi 下 link/prod/tx 计数为 0（DEV 诊断，无崩溃）。
- **静态内存/无堆**：新增只有 4 B `.rtc_noinit`；模块表 `constexpr` 落 flash；`RadioMode.*`、`ConsoleSetup` 的 radio 命令、`RadioModeCheck` 均无 `new/malloc`；无异常/RTTI/iostream。
- **`RTK_WIFI_CONFIG_ENABLE=n`**：`radio` 命令与 verb 组不注册（`ConsoleSetup.cpp` 的 `kWifiConfigEnabled` + `ConsoleProfileCheck` 断言），`stepRadio` 恒 bt，`s_radio_record`/`.rtc_noinit` 为 0，镜像不出现 radio 帮助串；`w1-wifi-off` 构建 0 warning。

## 6 限制

- 未复跑 IDF 构建：本 shell 的 IDF Python venv 缺失（`~/esp/esp-idf-v6.1/export.sh` 报 `idf6.1_py3.14_env ... not found`）；`builds/w1-idf` 的 obj/bin 时间戳晚于源码，且 G3 build log 无告警，判断产物对应当前树。作者报告的 34 PASS 未逐项重跑。
- P2 HIL（RTC 跨重启、BT 释放后空闲堆、`radio wifi/bt` 回执、断电回落）按卡由 Architect 执行。

W1R DONE
