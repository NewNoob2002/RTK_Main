# SV1：Executor 异步命令总线实施报告

- **任务卡**：`docs/tasks/SV-settings-service.md` §0、§1、§4（依据 `docs/adr/ADR-0018-settings-service.md` §2.2）
- **基线**：`8c28078`（分支 `feat/sv-settings`，当前 HEAD `4bbefdc` 仅含任务卡文档）
- **构建前缀**：`builds/sv1-*`（基线对照构建 `builds/sv1-base`）
- **工具链**：ESP-IDF v6.1（`/home/gtc/esp/esp-idf-v6.1`）、Python 3.13（`idf6.1_py3.13_env`）、`uvx lizard` 1.26.1
- **范围确认**：仅完成 SV1；未启动 SV2/SV3，未删除 `phonePpp`/`phoneSat`/`webGnss` 专用通道，未执行 git commit/烧录/串口操作。

---

## 1. 改动文件清单

### 新增文件

| 文件 | 用途 |
| --- | --- |
| `tools/main_host_check/ExecutorAsyncCheck.cpp` | 多线程执行器异步总线主机回归（链接真实 pthread 版 `FakeRtos.c`）：覆盖 FIFO 顺序、4 路多生产者并发 `post/poll`（含即时与延迟完成）、3 路并发阻塞 `call()`、`call()` 延迟超时取消与迟到 `complete()` 释放、3 路并发 `post/cancel` 压力及 8 槽零泄漏校验 |
| `docs/reports/SV1.md` | 本任务实施报告 |

### 修改文件

| 文件 | 改动摘要 |
| --- | --- |
| `components/rtos/include/rtos/Executor.hpp` | **授权修改的冻结头文件**：在 `namespace fw` 新增 `kDetailDeferred`（`std::numeric_limits<int32_t>::min()`）；在 `namespace rtos` 新增 `kMaxPending = 8`、`kInvalidSlot`、`Caller`、`Ticket`、`PollState`、`PendingState`、`PendingSlot`；`ExecutorStorage`/`ExecutorStorageView`/`viewOf()` 以静态完成表 `std::array<PendingSlot, kMaxPending> pending` 替换 `resultArea`/`resultQueue`；`rtos::Executor` 新增 `post()`/`poll()`/`cancel()`/`complete()` 公开接口及私有辅助方法与 `friend struct ExecutorTestAccess`；更新 `call()` 契约注释（支持多非本执行器任务并发调用，超时调用 `cancel()`） |
| `components/rtos/Executor.cpp` | 移除 `result_q_` 创建与读写；实现完成表临界区管理、`allocSeqLocked()`（单调递增跳过 0 及在途冲突序号）、`post()`/`poll()`/`cancel()`/`complete()`/`processCommand()`/`finishCommand()`；延迟受理判定 `result.detail == fw::kDetailDeferred`；`call()` 改建于 `postInternal()` + `ulTaskNotifyTake()` + `poll()`，超时执行 `cancel()` 并返回 `kTimeout`；`complete()` 校验 `seq != 0` 且仅允许本执行器任务调用 |
| `main/AppContext.hpp` | `ExecutorControlStorage<QueueDepth>` 与 `app::viewOf()` 同步将 `resultArea`/`resultQueue` 替换为 `std::array<rtos::PendingSlot, rtos::kMaxPending> pending{}`（全零静态初值保持 `.bss`） |
| `tools/main_host_check/ExecutorCheck.cpp` | 适配完成表接口（补齐 `xTaskNotifyGive`/`ulTaskNotifyTake` 替身、`view.pending` 校验）；新增 `async` 场景覆盖未启动/未知目标、5 种 `Caller`、FIFO 顺序、表满/队满 `kBusy` 与槽位回滚、取消未执行/延迟中/已完成、延迟完成与跨任务 `complete()` 拦截、自 `post()`、`seq` 回绕（含冲突序号跳过）及 `call()` 延迟完成/超时取消 |
| `tools/main_host_check/TemplateCheck.cpp` | 补齐主机桩 `ulTaskNotifyTake()` 在 `s_pump_receive` 模式下驱动 `pump()`，保持原有 `TemplateModule` 命令回归语义 |
| `tools/main_host_check/MainHostCheck.cpp` | `factoryReset()` 场景初始化并绑定主线程 `s_host_task` 到 `fake_current_task`，使 `Executor::call()` 的真实 pthread 任务通知等待生效 |
| `tools/main_host_check/CMakeLists.txt` | 注册 `executor_async` 确定性场景及多线程目标 `executor_async_check`（`multi_producer_post`、`concurrent_call`、`cancel_stress` 三个 CTest 用例） |

---

## 2. 冻结文件清单（供 Architect 验收后重算 `docs/frozen.sha256`）

本次仅改动任务卡明确授权的 **1 个**冻结文件，其余全部冻结文件 0 字节变更：

| 冻结文件 | 变更说明 | 新 SHA-256 |
| --- | --- | --- |
| `components/rtos/include/rtos/Executor.hpp` | ADR-0018 §2.2 / 任务卡 §1 授权：新增 `fw::kDetailDeferred`、`rtos::kMaxPending`、`rtos::kInvalidSlot`、`Caller`/`Ticket`/`PollState`/`PendingState`/`PendingSlot`；以完成表替换 `resultArea`/`resultQueue`；新增 `Executor::post/poll/cancel/complete` 并更新 `call()` 契约 | `41934fc5e27b819d6f8add1dbc1799f7aacaba73db1815e8357220fd718ec081` |

### 签名与测试落位说明（任务卡 §1.1、§1.6）

1. **接口签名**：与任务卡 §1.1 完全一致（`Caller`、`Ticket`、`PollState`、`post`/`poll`/`cancel`/`complete`/`call` 参数与返回类型无改动）。补充说明两点实现细节：
   - `fw::kDetailDeferred` 取 `std::numeric_limits<int32_t>::min()`，与 `0`、`-1` 及全部 ESP-IDF `esp_err_t` 错误码不冲突；
   - `Ticket::slot` 缺省值用命名常量 `rtos::kInvalidSlot`（值为 `0xFF`，消除魔数告警）；`PendingState::kFree = 0` 与 `PendingSlot` 全零缺省值保证 `AppContext` 静态存储继续落入 `.bss`。
2. **主机测试落位**：按现有门禁结构放在 `tools/main_host_check/`（该目录已纳入 `tools/ci/run_gates.sh` 的 `G2-main_host_check` 与 `G2b-main_host_check-asan`），拆为确定性单线程回归（`ExecutorCheck.cpp`，4 场景）和真实多线程回归（`ExecutorAsyncCheck.cpp`，3 场景，链接 `tools/t13_host_check/shim/FakeRtos.c`）。

---

## 3. 门禁与验证结果

执行命令：

```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
. "$IDF_PATH/export.sh"
uvx lizard --version
tools/ci/run_gates.sh --base 8c28078 --prefix sv1
```

总计：**35/36 PASS，1 EXPECTED FAIL（仅 `G1-frozen` 的 `components/rtos/include/rtos/Executor.hpp`）**。

| 门禁项 | 结果 | 详情 |
| --- | --- | --- |
| `G1-frozen` (`sha256sum -c docs/frozen.sha256`) | **FAIL（预期）** | 仅 `components/rtos/include/rtos/Executor.hpp: FAILED`（已授权，待 Architect 重算 `docs/frozen.sha256`） |
| `G2-*` / `G2b-*-asan`（12 组 × 普通 + ASan/UBSan） | **PASS（24/24）** | `main_host_check` **81/81**（普通与 ASan/UBSan 均 0 失败、0 泄漏），含 `executor_{lifecycle,calls,async,invalid}` 与 `executor_async_{multi_producer_post,concurrent_call,cancel_stress}` |
| `G5-format` (`clang-format --style=file --dry-run --Werror`) | **PASS** | 全部 C/C++ 文件符合仓库 `.clang-format` |
| `G6-doc_check` (`doc_check.py --base 8c28078`) | **PASS** | `doc_check: 改动函数 16 个，不合格 0 个` |
| `G3-idf` / `G3-idf-trace-off` / `G3-idf-wifi-off` / `G3-idf-production` / `G3-idf-console-none` | **PASS（5/5）** | 全部 0 warning |
| `G3b-rtk_pure-esp32` / `G3b-rtk_pure-esp32c3` | **PASS（2/2）** | 全部 0 warning |
| `G5b-tidy` (`tidy_check.py`) | **PASS** | `文件 139 个，发现 0 条` |
| `G7b-runtime` (`g7b_check.py --build builds/sv1-idf`) | **PASS** | 128 目标文件，0 未白名单堆符号，0 `__cxa_guard`，6/6 允许符号 |
| `git diff --check` | **PASS** | 无尾随空白/冲突标记 |
| P2 台架 HIL | **NOT_RUN** | 依 `AGENTS.md` §6 执行者禁止烧录与连串口，交由 Architect 执行 |

---

## 4. 内存与镜像大小对比（`idf.py size`）

对比基线 `builds/sv1-base`（`8c28078` 代码，变动前原样构建）与 `builds/sv1-idf`（SV1 改动后）：

| 指标 | 基线 (`sv1-base`) | SV1 (`sv1-idf`) | 差值 |
| --- | ---: | ---: | ---: |
| IRAM Used (B) | 114,715 | 114,715 | `0 B` |
| IRAM Remain (B) | 16,357 | 16,357 | `0 B` |
| DRAM Used (B) | 97,483 | 97,515 | `+32 B` |
| DRAM `.data` (B) | 51,099 | 51,131 | `+32 B` |
| DRAM `.bss` (B) | 46,384 | 46,384 | `0 B` |
| DRAM Remain (B) | 27,097 | 27,065 | `-32 B` |
| Flash Code `.text` (B) | 1,113,040 | 1,113,816 | `+776 B` |
| Flash Data `.rodata` (B) | 252,284 | 252,284 | `0 B` |
| Total image size (B) | 1,531,178 | 1,531,986 | `+808 B` |
| `rtk_main.bin` (B) | 1,531,296 | 1,532,096 | `+800 B` |

其余构建档位 `rtk_main.bin` 大小：

- `builds/sv1-trace-off/rtk_main.bin`：`1,512,912 B`
- `builds/sv1-wifi-off/rtk_main.bin`：`912,480 B`
- `builds/sv1-production/rtk_main.bin`：`1,491,472 B`
- `builds/sv1-console-none/rtk_main.bin`：`1,442,640 B`

说明：以 8 项 `PendingSlot`（24 B/项 × 8 = 192 B）替换原先的 `resultArea` + `StaticQueue_t`（约 176 B）后，两台执行器静态 RAM 净增仅 `+32 B`，IRAM 零增长，完全符合无堆约束。

---

## 5. 迁移前后行为差异

现有调用方（控制台 `submitRawTo`/`FactoryReset`、手机 `phonePpp`/`phoneSat`、网页 `webGnss`）代码与正常路径行为保持不变；执行器内部语义存在以下有意改进（符合 ADR-0018 §2.2 与任务卡 §1）：

1. **`call()` 超时后自动取消**：旧实现超时后命令仍留在 `cmd_q_` 中并在后续被执行、结果写入 `result_q_` 等待下次排空；新实现超时调用 `cancel(ticket)`：
   - 若命令尚未被执行器取出，取出时直接跳过 `onCommand` 并释放槽位（不再产生无谓副作用，也不计入 `stats.commands`）；
   - 若命令已在执行或处于延迟状态，结果到达（`onCommand` 返回或模块调用 `complete()`）时丢弃结果并释放槽位。
2. **解除单调用者限制**：旧实现因共用单一 `result_q_` 且每次 `call()` 前清空旧结果，同一时刻只允许一个调用者任务；新实现每个在途请求独占完成表槽位并按 `waiterTask` 独立通知，支持多个非本执行器任务并发 `call()` 或混合 `post()`/`call()`。
3. **本执行器自投递语义**：仍禁止从本执行器任务调用 `call()`（返回 `kInvalidState`，防死锁），但允许调用 `post()` 并在后续轮次 `poll()`。
4. **`ExecStats::resultDrops` 统计语义**：旧实现记录“结果队列满丢弃的最旧结果数”；移除 `result_q_` 后，改为记录“完成表未匹配到对应在途槽位而丢弃的结果数”（例如重复 `complete()` 或未知 `seq`），并发出 `ExecTrace::kResultDrop`。正常路径与正常 `cancel()` 均不增加该计数。
5. **在途并发上限**：单执行器同时在途命令数受 `min(QueueDepth, kMaxPending)` 约束（当前 `kMaxPending = 8`），队列满或完成表满均返回 `fw::Result::kBusy` 且不泄漏槽位。

---

## 6. Architect 台架 HIL 验收清单

> 注：本清单聚焦 SV1（异步命令总线与现有控制台/旧通道回归）；网页 `hello.caps`/设置服务切换、手机 0x06/0x33 切服务、NONE 档网页全量设置将在 SV2/SV3 验收清单中展开。

1. **固件启动与基础统计**：烧录 `builds/sv1-idf/rtk_main.bin` 到 P2，串口确认正常启动进入运行态；执行 `sys status`，核对 `exec_rounds`/`io_rounds` 正常递增、`exec_drops=0`、`io_drops=0`，以及 `link_stk_hwm`/`io_stk_hwm` 与基线无明显退化。
2. **控制台命令冒烟（经重构后的 `Executor::call()`）**：
   - `kLink` 目标：执行 `bt status`、`config get`（或 `config` 读项）、`rtcm stats`、`debug status`，确认均正常返回并输出 `OK`，无超时或 `no response`。
   - `kIo` 目标：执行 `gnss status`、`gnss rate`（读写一次现有合法值并恢复），确认命令经 `ioExecutor->call()` 正常回执且 `exec_cmds`/`io_cmds` 递增。
3. **存储/复位命令验证（可选）**：如台架允许，测试 `freset`（不带 `yes` 确认提示，或按需测试确认流程），核对 `StorageModule` 命令响应正常。
4. **未迁移通道回归（确认 SV1 未破坏现有功能）**：
   - 蓝牙模式下手机连接确认现有 0x06/0x33 报文仍正常收发（`phonePpp*`/`phoneSat*` 未改动）；
   - `radio wifi` 进入网页配置模式，打开页面修改一项设置并确认正常生效（`webGnss*` 未改动）。
5. **冻结校验更新**：验收通过后，由 Architect 复核 `components/rtos/include/rtos/Executor.hpp` 变更并重算 `docs/frozen.sha256`，再开启 SV2。
