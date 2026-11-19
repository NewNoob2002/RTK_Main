# G-baud460k 实施报告：探测后默认将 GNSS 串口切至 460800（运行态，不写 Flash）

- 分支：`feat/g-baud460k`
- 基线：`3129422`
- 构建前缀：`builds/gb-*`（日志位于 `builds/gb-logs/`）
- 冻结输入：`sha256sum -c docs/frozen.sha256` 全部 PASS，未修改任何冻结头或文档。

---

## 1. 改动文件清单

| 文件 | 改动说明 |
| --- | --- |
| `main/Kconfig.projbuild` | 在 `menu "GNSS (ADR-0010)"` 新增 `RTK_GNSS_TARGET_BAUD`（int，默认 `460800`，范围 `9600–921600`，`depends on RTK_GNSS_ENABLE`），与首次探测起点 `RTK_GNSS_DEFAULT_BAUD`（默认 `115200`）解耦。 |
| `main/AppConfig.hpp` | 新增编译期常量 `app::kGnssTargetBaud`（由 `CONFIG_RTK_GNSS_TARGET_BAUD` 推导，未定义时回退 `460800U`）。 |
| `main/app/BootConfig.hpp` | 更新 `BootConfig::gnssBaud` 注释说明：`0` 表示无用户覆盖，启动后切至 `RTK_GNSS_TARGET_BAUD`。 |
| `main/def/GnssSubDef.hpp` | 在 `GnssSubTrace` 末尾追加 `kBaudFail = 13`（`val0 = 目标波特率`，`val1 = gnss::MaintenanceResult`），不修改冻结的 `main/def/GnssDef.hpp`。 |
| `main/modules/debug/TraceText.cpp` | 注册 `GnssSubTrace::kBaudFail` 的文本模板 `<cfg> startup baud {baud} failed (result={result})`。 |
| `main/data/AppData.hpp` | 在 `DataCenter` 新增无锁原子计数 `std::atomic<uint32_t> gnssBaudFailCount{0U}`（附 `static_assert(fw::kAtomicUsable<uint32_t>)`），供 `status gnss` 与 `gnss status` 读取，不修改冻结的 `GnssStatus`。 |
| `main/modules/gnss/GnssModule.hpp` | 新增 `targetBaud()`、`syncBootHint()`、`completeBootBaudMaintenance()`、`baud_fail_count_`；`tickMaintenance(nowMs)` 与 `alignBootBaud(nowMs)` 透传当前时间戳以便同轮发出切速命令。 |
| `main/modules/gnss/GnssModule.cpp` | 增加 `static_assert(gnss_store::validUserBaud(kGnssTargetBaud))`；`startController()` 每次启动固定置位 `boot_baud_pending_ = true` 并清零 `baud_fail_count_` 与 `gnssBaudFailCount`；`poll()` 调用 `tickMaintenance(nowMs)`。 |
| `main/modules/gnss/GnssModuleMaintenance.cpp` | 实现优先级 `targetBaud() = (preferred_baud_ != 0) ? preferred_baud_ : kGnssTargetBaud`；目标等于 `detected().baud` 时跳过切速；启动切速成功仅刷新 `gnss.cache` 与 `setProbePreferences`，**不写 `gnss/baud` 且不发 `SAVECONFIG`**；手动 `gnss baud <n>` 保持原 `persistBaudAndSave()`（写 `gnss/baud` + `gnss.cache` + `SAVECONFIG`）；启动切速失败递增 `baud_fail_count_`、`gnssBaudFailCount`，发 `kBaudFail` trace 与 W 日志，且每次启动最多尝试 1 次；`gnss freset yes` 擦除用户 `gnss/baud` 覆盖键，使下次启动回到 `RTK_GNSS_TARGET_BAUD`。 |
| `main/modules/gnss/GnssModuleProbe.cpp` | `persistProbeHint()` 补齐 `userNvsReady` 保护，避免 NVS 未就绪时写盘。 |
| `main/modules/system/SystemStatusCmds.cpp` | `status gnss` 与 `status` 概览的 `state` 行已显示 `baud=`（切换后的 `gnss.hostBaud`），补充显示 `baud_fail=`。 |
| `main/modules/gnss/GnssCmdsStatus.cpp` | `gnss status` 的 `baud` 行同步显示 `baud_fail=`。 |
| `main/modules/storage/ConfigCmds.cpp` | `config build` 的 `gnss_uart` 行同时显示 `probe`（`RTK_GNSS_DEFAULT_BAUD`）与 `target`（`kGnssTargetBaud`）。 |
| `tools/main_host_check/CMakeLists.txt` | 为 `gnss_output_check` 与 `gnss_stream_check` 定义 `CONFIG_RTK_GNSS_TARGET_BAUD=460800`。 |
| `tools/main_host_check/GnssResetCheck.cpp`、`GnssOutputCheck.cpp` | 更新 C2 启动对齐不再发 `SAVECONFIG` 及 `freset` 擦除用户 `gnss/baud` 的既有断言；新增 `testGnssTargetBaudPolicy()` 覆盖默认冷启切 460800（无 SAVECONFIG + hint 记录 460800）、ESP32 软复位首候选命中且不切换、模块掉电冷启由 115200 命中再切 460800、用户 NVS `115200`/`230400` 覆盖 Kconfig、切速超时回退一次 + trace + `baud_fail=1`、切速 NACK 保持旧速率 + trace + `baud_fail=1`。 |
| `test/gnss/TestGnssUnicoreReceiver.cpp` | 新增 `testCm680BaudScriptNoSave()`，验证 CM680 `baudStep(1, 460800)` 与 `roverStep` 生成 `CONFIG COM1 460800\r\n` 且不含 `SAVECONFIG`。 |
| `test/gnss/TestGnssController.cpp` | 新增 `testMaintenanceBaudNackAndUnsupported()`，验证切速收到 NACK 或驱动不支持 `baudStep` 时保持旧速率与 `kRunning`，不卡死。 |

---

## 2. CM680 `CONFIG COMn <baud>` 核对与全驱动审查

### 2.1 CM680 协议 PDF V1.2.8 核对

对照文档：`/home/gtc/Desktop/workspace/ESP32_PROJ/P2 手持式GNSS接收机项目/CM680系列高精度模组产品数据接口协议V1.2.8.pdf`（内封面标题：Optisens《CM6XX 系列高精度模组产品数据接口协议》，Rev 1.0，Date 2024-07-16）：

1. **命令语法与大小写**：
   - 第 9 页 **§1 表 1-1 常用指令集**：指令支持简化 ASCII 格式，不区分大小写，无校验位；`config com1 115200` 用于设置串口波特率，可分别对 `com1`、`com2`、`com3` 设置为 `9600、19200、38400、57600、115200、230400、460800、921600` 等常用速率（亦支持 `1000000、2000000、4000000` 等大波特率）。
   - 第 19 页 **§4 配置指令**、第 20 页 **§4.1 串口配置（表 4-3）**：格式 `CONFIG [串口设备号] [串口属性参数]`，示例 `CONFIG COM1 115200`（适用产品明确列有 `CM680、CM682、CM660、CM682-INS`）；串口支持 `COM1`/`COM2`/`COM3`；数据位/奇偶校验/停止位为可选参数且当前产品仅支持 `8 / N / 1`，省略即 8N1。
   - 第 21 页 **表 4-4 串口支持的波特率**：COM1、COM2、COM3 均明确支持 `9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000, 4000000`。
2. **应答与验证**：
   - 手册 §4.1 未规定 `CONFIG COMn <baud>` 在旧波特率下必回 `$command,...,response: OK`；由于端口切换立即生效，旧速率无法可靠接收应答。现有 `UnicoreReceiver::baudStep()` / `stepPortBaud()` 设置 `expectReply = false`、`switchBaud = baud`，由 `Controller` 在发完命令后等待 `100 ms`（期间若收到 `$command,...,response: ERROR` 直接判 `kNack` 不切速）→ `uart_wait_tx_done` 排空 TX → 主机切到目标波特率 → 发 `VERSIONA\r\n` 探测（`700 ms` 超时），必须收到 `#VERSIONA,...,"CM680",...` 才确认切换成功；否则回退旧速率并重新探测一次。
3. **非易失性与掉电恢复**：
   - 第 29 页 **§4.12 Saveconfig（表 4-17）**：只有执行 `SAVECONFIG` 才把当前配置保存到非易失性存储器。
   - 第 9 页 **表 1-1** 与第 28 页 **§4.10 FRESET（表 4-15）**：出厂默认波特率为 `115200`。因此启动只发 `CONFIG COM1 460800\r\n` 而不发 `SAVECONFIG` 时，仅本次上电生效，模块掉电后回到 115200，不磨损模块 Flash。

### 2.2 全驱动波特率切换步骤审查

| 家族 / 型号 | 驱动方法 | 发出字节 | 持久化层 | 不支持/失败行为 |
| --- | --- | --- | --- | --- |
| Unicore（UM980 / UM981 / UM982 / **CM680**） | `UnicoreReceiver::baudStep`（CM680 经 `forModel` 路由至 `Um980Receiver`） | `CONFIG COM<port> <baud>\r\n`（P2 COM1，E1 COM2） | 仅运行态 RAM（不发 `SAVECONFIG`） | NACK 保持旧速率；超时回退旧速率重探 1 次 |
| ComNav（K803，实验性） | `ComnavReceiver::baudStep` | `COM COM<port> <baud>\r\n` | 仅运行态 RAM（不发 `SAVECONFIG`） | `<ERROR` 保持旧速率；超时回退旧速率重探 1 次 |
| u-blox（ZED-F9P） | `UbloxReceiver::baudStep` | UBX `CFG-VALSET` v0，`layer = 0x01`（`kLayerRam`），键 `CFG-UART1-BAUDRATE`（`0x40520001`）或 `CFG-UART2-BAUDRATE`（`0x40530001`） | **仅 RAM 层**（不写 BBR/Flash） | `port` 非 1/2 返回 `false` → `kUnsupported` 保持旧速率不卡死；超时回退重探 1 次 |
| 其他 / 未知驱动（`Receiver` 基类、`NullReceiver`） | `Receiver::baudStep` | 返回 `false` | 无 | 立即返回 `kUnsupported`，记 `kBaudFail` trace 与 `baud_fail+1`，保持当前波特率继续运行 |

### 2.3 两种重启场景探测时序核对

候选顺序为 `hint_`（来自 `gnss.cache`）排第 0 位，其后 `enabledReceivers() × bauds_`（无用户 NVS 时 `bauds_[0] = CONFIG_RTK_GNSS_DEFAULT_BAUD = 115200`）。单个候选超时 `kTimingProbeTimeoutMs = 400 ms`，共 `3` 轮：

1. **ESP32 单独复位（模块未掉电、仍在 460800，`gnss.cache` 记 460800）**：
   - 第 0 候选 `(family, 460800)` 直接命中（主机测试断言 `probesSent == 1`），`detected.baud == targetBaud() == 460800`，不发切速命令、不发 `SAVECONFIG`。
2. **模块掉电冷启动（模块回出厂 115200，`gnss.cache` 仍记上次 460800）**：
   - 第 0 候选 `(family, 460800)` 400 ms 超时 → 第 1 候选 `(family, 115200)` 立即命中（仅增加 400 ms，远小于单轮上限）→ 配置并进入 `kRunning` → `alignBootBaud()` 切至 460800 并以 `VERSIONA` 验证（~120 ms，主机测试断言 `probesSent == 3`），不发 `SAVECONFIG`，`gnss.cache` 保持 460800（无多余 NVS 写）。

---

## 3. 与任务卡契约的偏离

- **切速通道复用 `alignBootBaud()` + `Controller::requestBaud()`（而非 `RoverConfig::portBaud`）**：
  - 任务卡 §1.3 建议设 `RoverConfig::portBaud = 目标`。实际代码库在 C2 中已具备完整的开机切速状态机 `GnssModule::alignBootBaud()` → `Controller::requestBaud()` → `Receiver::baudStep()`。
  - 若改走 `kConfiguring` 的 `RoverConfig::portBaud`：`ControllerConfigure.cpp` 在 `switchBaud` 后无 100 ms 稳定/NACK 等待、无 `uart_wait_tx_done`、无切换后 `VERSIONA` 探测确认与失败回退逻辑（且 `TestGnssController::testBaudSwitchStep` 显式断言无延时立即切速；补充回退状态需改冻结头 `components/gnss/include/gnss/Controller.hpp`），同时会与 C2 既有 `boot_baud_pending_` 逻辑冲突。
  - 复用 `alignBootBaud()` + `Controller::requestBaud()` 在**不改动任何冻结头**的前提下，直接复用了各驱动同一份 `baudStep()` 与 `Action::kSetBaud`，并天然满足 §1.3–§1.6 全部行为要求（含 NACK 检测、TX 排空、新速率版本帧验证、失败回退旧速率重探一次、最多尝试一次、不发 `SAVECONFIG`）。
- **`gnss freset yes` 擦除 `gnss/baud` 覆盖键**：
  - 原 C2 的 `resetStoredSettings()` 在恢复出厂后将 `115200` 写入 `gnss/baud`。引入 `RTK_GNSS_TARGET_BAUD` 后，若保留写入 `115200`，会导致出厂复位后的下次启动被当作用户显式覆盖 `115200` 而无法切至 `460800`。现改为擦除 `gnss/baud` 键（恢复为未设置/default），`gnss baud <n>` 的持久化语义不变。

---

## 4. 实际执行的命令与结果

完整门禁命令：
```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
. $IDF_PATH/export.sh >/dev/null
tools/ci/run_gates.sh --base 3129422 --prefix gb
idf.py -B builds/gb-idf size
```

| 门禁 / 检查项 | 结果 | 详情 / 日志 |
| --- | --- | --- |
| `G1-frozen`（`sha256sum -c docs/frozen.sha256`） | PASS | 冻结文件 0 修改，`builds/gb-logs/G1-frozen.log` |
| `G2-host` / `G2b-host-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-app_fw` / `G2b-app_fw-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-services` / `G2b-services-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-stream_demux` / `G2b-stream_demux-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-bt_reconnect` / `G2b-bt_reconnect-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-cli` / `G2b-cli-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-gnss` / `G2b-gnss-asan` | PASS | 8/8 通过（含新增 CM680 切速无 SAVECONFIG、Controller NACK/Unsupported 用例） |
| `G2-gnss-all` / `G2b-gnss-all-asan` | PASS | 9/9 通过（含 ComNav + u-blox 全驱动） |
| `G2-nvs_store` / `G2b-nvs_store-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-web_proto` / `G2b-web_proto-asan` | PASS | 普通 + ASan/UBSan 全过 |
| `G2-main_host_check` / `G2b-main_host_check-asan` | PASS | 72/72 通过（含新增 `testGnssTargetBaudPolicy` 全部 6 个子场景） |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | PASS | 44/44 通过 |
| `G5-format`（`clang-format --style=file --dry-run --Werror`） | PASS | 0 处格式差异 |
| `G6-doc_check`（`doc_check.py --base 3129422`） | PASS | 改动函数 15 个，不合格 0 个，`builds/gb-logs/G6-doc_check.log` |
| `G3-idf`（默认开发构建） | PASS | 0 warning；`rtk_main.bin` = 879,264 B（`Total image size: 879146 bytes`，Flash Code 610,044 B，Flash Data 161,000 B，IRAM 86,287 B / 65.83%，DRAM 75,023 B / 60.22%） |
| `G3-idf-trace-off`（`CONFIG_RTK_TRACE_ENABLE=n`） | PASS | 0 warning |
| `G3-idf-production`（`configs/production.defaults`） | PASS | 0 warning |
| `G3-idf-console-none`（`configs/console-none.defaults`） | PASS | 0 warning |
| `G3b-rtk_pure-esp32` | PASS | 0 warning |
| `G3b-rtk_pure-esp32c3` | PASS | 0 warning |
| `G5b-tidy`（`tidy_check.py`） | PASS | 133 个文件，0 条发现，`builds/gb-logs/G5b-tidy.log` |
| `G7b-runtime`（`g7b_check.py`） | PASS | 121 个自有目标文件，0 堆符号放行，0 `__cxa_guard`，6/6 白名单异常/RTTI，`builds/gb-logs/G7b-runtime.log` |
| 门禁汇总 | **PASS（35/35）** | `35 passed, 0 failed` |
| HIL（P2 冷上电、ESP32 单独复位、20 Hz pipe/app_ring、10 min RTCM `io_exec overruns`） | NOT_RUN | 依据 AGENTS §6 禁止烧录与操作 `/dev/ttyACM0`，留给 Architect 上板验收 |

---

## 5. §文档待改（冻结文档，待 Architect 统一更新并重算 `docs/frozen.sha256`）

1. **`docs/adr/ADR-0010-gnss.md`**：
   - §2.7 Kconfig 表（约 L109）：补充 `RTK_GNSS_TARGET_BAUD`（默认 `460800`，探测成功后的默认运行波特率）与 `RTK_GNSS_DEFAULT_BAUD`（默认 `115200`，首次探测起点）的分工。
   - §2.7 NVS 缓存表（约 L150）：旧表仍写 `gnss/baud` 为 cache，实际已拆为 `gnss/baud`（用户覆盖）与 `gnss/cache_baud`（探测缓存，记录切速后的实际运行波特率）。
   - §2.4（约 L58–L62）：补充说明开机默认运行切速经 `Controller::requestBaud()` / `Receiver::baudStep()` 完成、不发 `SAVECONFIG`，以及切速失败单次回退与 `baud_fail` 计数。
2. **`docs/adr/ADR-0012-gnss-receiver-drivers.md`**：
   - §2.3 探测顺序（约 L146）：明确 `gnss.cache` 使用 `gnss/cache_baud` 优先探测（使软复位首候选命中 460800，冷启动回落候选表 115200）。
3. **`docs/adr/ADR-0014-runtime-config.md`**：
   - §2.4（约 L69–L76、L127）：修订“开机探测波特率与 NVS `gnss/baud` 不同时自动对齐并保存”的表述，明确为：**每次启动按 `gnss/baud > RTK_GNSS_TARGET_BAUD` 仅改运行波特率，绝不发 `SAVECONFIG`**；只有用户显式执行 `gnss baud <n>` 时才写 NVS `gnss/baud` 并自动发 `SAVECONFIG`；`gnss freset yes` 擦除 `gnss/baud` 覆盖键。
4. **`docs/adr/ADR-0009-trace-console.md`**：
   - 追踪事件与 `status gnss` 说明：补充 `GnssSubTrace::kBaudFail`（code 13）与 `status gnss` `state` 行新增的 `baud_fail=` 字段。
5. **`docs/acceptance.md`**：
   - §2 HIL 表：补充 GNSS 运行波特率验收项（P2 冷上电与 ESP32 单独复位均 `baud=460800`、`baud_fail=0`、20 Hz 下 `pipe drop=0` 与 `app_ring` 无丢、10 分钟 RTCM 下对比 `io_exec overruns`）。

---

## 6. 未解决问题 / 待办

- **HIL 实机验证（Architect）**：
  - P2 v1.41 + CM680 冷上电：`status gnss` 确认 `state=running model=CM680 baud=460800 baud_fail=0`，且串口日志无 `SAVECONFIG`。
  - P2 v1.41 + CM680 ESP32 单独复位（模块不掉电）：`gnss.cache` 第一候选 460800 直接命中，无切速命令。
  - 20 Hz 输出与 10 分钟 RTCM 注入：验证 `pipe drop=0`、`app_ring` 无丢帧，并对比 `io_exec overruns` 相对 115200 基线（467/26827, max_rnd 44 ms）的改善幅度。
