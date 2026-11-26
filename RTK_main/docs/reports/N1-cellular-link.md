# N1 实施报告：4G EC600U 驱动与链路状态机（ADR-0021 第一期，复审修复后）

- **任务卡**：`docs/tasks/N1-cellular-link.md`；设计依据 `docs/adr/ADR-0021-cellular-ec600u.md`（已批准）
- **分支 / 基线**：`feat/n1-cellular`（门禁基线 `32a0dd7`），未合并 main、未 push
- **提交**：首轮 `d3800af`；本报告对应复审修复提交（审查项 #1–#17 全部处理，裁决见 Architect 的 N1 复审裁决文件）
- **Profile**：`esp-idf`，C++20，严格告警 + `-Werror`，零异常/RTTI，运行期零堆分配
- **门禁**：`tools/ci/run_gates.sh --prefix n1 --base 32a0dd7`，结果见 §3
- **未执行**：板上 HIL（由 Architect 与用户执行，步骤见 §8）；任务栈高水位与启动后堆余量未实测（见 §4、§5）

---

## 1. 改动清单

### 1.1 新增

| 路径 | 内容 |
| --- | --- |
| `components/cellular/`（`AtEngine`、`AtParse`、`LinkFsm`、`Policy`、`Types`） | 纯逻辑、无 IDF 依赖：AT 命令队列（单在途）/行分帧/`OK`·`ERROR`·`+CME ERROR`/URC 前缀分发/`>` 提示与定长数据读/每命令超时；超时或 UART 丢字节后进入**显式重同步**（旧响应永不归属新命令）；链路状态机 Off→PowerOn→Sync→SimCheck→Register→PdpUp→Ready，L1/L2/L3 分级恢复与退避；完成回调带操作/连接/恢复轮次令牌，过期结果丢弃；`QIOPEN` 超时先 `QICLOSE` 再允许重开；逐槽连接快照与无归属 `recv` 丢弃计数 |
| `components/cellular/include/cellular/Policy.hpp` | 全部时间常量、退避表、AT 超时表（`constexpr`，单一来源） |
| `main/def/NetDef.hpp` | `NetConfigView{enable,passSet,restartPending,available,apn,user}`、`NetVerb{kPowerOn,kPowerOff,kReset}` |
| `main/modules/net/NetModule.{hpp,cpp}` | UART1 驱动、GPIO26(PWREN) 控制、kNet 执行器模块、DataCenter 发布、PW1 关机参与者；`Runtime` 全部大对象放在启动期分配的 `NetBlock`（见 §4） |
| `main/modules/net/NetMailbox.{hpp,cpp}` | 控制台 AT 邮箱：请求令牌归属、关机后拒绝、过期结果丢弃、白名单思路拒绝表（`;`/控制字符、`ATE`/`AT+IPR`/`AT+QISWTMD`/`AT+QICSGP` 查询与设置、`CFUN` 除 0/1/4、`QISEND` 等数据提示命令、`QIOPEN` access_mode≠0）、响应凭据行过滤 |
| `main/modules/net/NetPolicy.hpp` | `NetPowerGate`：电池 <10% 仅禁止**新**上电（已在线保持，15% 迟滞）、配网/关机断电（纯逻辑，可主机测试） |
| `main/modules/net/NetStore.{hpp,cpp}` | `net.*` NVS 读写（`save*` 仅允许 `StorageModule` 调用；`load`/`loadEnable` 启动期读） |
| `main/modules/net/NetCmds.cpp` | 控制台 `net status`（所有档位；显示配置、restartPending、未运行原因）；DEV 档 `net power on\|off`、`net reset`、`net at <cmd>` |
| `main/app/NetBlock.hpp` | 启动期分配的 4G 运行态块（执行器 + 控制块 + `Runtime`） |
| `tools/main_host_check/NetLogicCheck.cpp` | 邮箱生命周期/过期令牌/拒绝表/凭据过滤/门控迟滞 |
| `test/cellular/`（`TestAtEngine`、`TestLinkFsm`） | 主机测试，普通与 ASan/UBSan |

### 1.2 修改

| 路径 | 变更 |
| --- | --- |
| `components/settings/include/settings/Types.hpp`（**冻结**） | 见 §2 |
| `components/settings/src/Registry.cpp` | 注册 `net.enable/apn/user/pass`（`Owner::kStorage`，`KeyKind::kText`，`access::kConsoleInternal`；`net.pass` 读回 `***`/空；`Caller::kMqtt`/web/phone 对 `net.*` locked） |
| `main/def/StorageDef.hpp` | `StorageVerb::kSetNetEnable/Apn/User/Pass = 8..11` |
| `main/modules/storage/StorageModule.{hpp,cpp}` | `net.*` 持久化归属（与其他设置同一路径）：先提交 NVS，成功后才更新视图并递增 `configRev`；写失败返回 `kIoError` 且视图不变；`net.enable` 变更置 `restartPending` |
| `main/app/BootConfig.{hpp,cpp}` | 启动期早读 `netEnable` |
| `main/app/BootBuffers.cpp` | `hasNet = 正常启动 && kCellular && uart>=0 && net.enable && 非配网`；仅此时分配 `task.net` 栈与 `net.block` |
| `main/AppConfig.hpp`、`main/AppContext.hpp`、`main/module_list.inc` | `ModuleKind::kNet`、`ExecKind::kNet`、`BootTask::kNet`、`NetBlock *netBlock`/`netExecutor`（指向块内） |
| `main/app/Executors.cpp`、`App.hpp`、`App.cpp`、`BootFlow.cpp`、`BootStepList.inc`、`BootSteps.hpp`、`BootReasons.hpp` | kNet 执行器配置（core 0、prio 8、静态栈 4096 B）、`startNet(block)`、未分配时跳过启动步；停机路径与看门狗断言含 kNet |
| `main/app/SettingsBackend.cpp` | `net.*` 走 kStorage；`static_assert` 保证 settings 动词与 `StorageVerb` 一致；读回取自 `data.netConfig` |
| `main/app/ConsoleRegistration.hpp`、`ConsoleSetup.cpp` | 注册 `net` 命令/动词组；PRODUCTION 档的 help/usage 只列 `status` |
| `main/board/BoardProfile.{hpp,cpp}`、`BoardStartup.cpp` | `CellularPins`（UART1 RX=GPIO32/TX=GPIO33、PWREN=GPIO26）；启动安全输出表对 kCellular 板跳过 PWREN，由 NetModule 优雅关机后拉低 |
| `main/data/AppData.{hpp,cpp}` | `netStatus`、`netConfig` 快照槽（`netConfig` 不含密码，仅 `passSet`） |
| `main/modules/power/PowerModule.{hpp,cpp}` | `ShutdownMilestone::kNetPower`；`ShutdownParticipant` 末尾新增可空 `onTimeout`，阶段超时恰调用一次（用于 4G 硬切 PWREN）；非冻结文件 |
| `tools/ci/run_gates.sh`、`tools/tidy_check/tidy_check.py` | 纳入 `test/cellular` 与 `components/cellular/src/*.cpp` |
| `tools/main_host_check/*` | CMake 接入 `NetStore.cpp`/`NetLogicCheck`；`net_settings_persistence` 用例；PW1 `onTimeout` 断言；跨层 fake 期望同步 |
| `test/settings/*` | 新键编号稳定性（`testPublishedKeyIdsStable`）、`kText`、`net.pass` 读回掩码 |

---

## 2. 冻结文件变更（请 Architect 验收后重算 `docs/frozen.sha256`；实现者未改该文件）

`docs/frozen.sha256` 中受影响的只有一个文件，相对基线 `32a0dd7` **全部**变更（仅追加，无既有数值变化）：

| 文件 | 变更 |
| --- | --- |
| `components/settings/include/settings/Types.hpp` | ① `kVerbStorageSetNetEnable/Apn/User/Pass = 8..11`；② `kNetTextMaxBytes = 31`；③ `KeyKind::kText`；④ `access::kConsoleInternal = 0x09`；⑤ `KeyId::kNetEnable/kNetApn/kNetUser/kNetPass = 22..25`（`kCount=20` 与 `gnss.cutoff=21` 不变，既有 KeyId 数值零变化）。**未**新增 `Owner` 成员（首轮的 `Owner::kNet` 已移除，`net.*` 归 `Owner::kStorage`） |

当前 SHA-256：`032ac0f650c08954cbe0d6dc64e915df1db0764220dcee1146fdbc24becc7f42`。G1-frozen 因此失败（仅此一个文件），属预期。

**与任务卡的关系**：卡中“不改冻结协议头”指协议头；`Types.hpp` 属 `components/*/include/**` 公共头。`KeyId`/`KeyKind`/`access` 位没有为 4G 键留扩展点，故做了最小增量修改。

---

## 3. 门禁结果

`tools/ci/run_gates.sh --prefix n1 --base 32a0dd7` 最终一次运行：**44 通过 / 1 失败**，唯一失败项为 **G1-frozen（仅 `components/settings/include/settings/Types.hpp`，预期，见 §2）**。G2/G2b（含 `cellular`、`settings`、`main_host_check` 的 ASan/UBSan）、G5-format、G6-doc_check、G3 全部 IDF 配置组合、G3b、G5b-tidy、G7b-runtime、G7c 均通过；`rtk_main.bin` 1 623 872 B。

### 3.1 GATES_RESULT

```
FAIL G1-frozen            (Types.hpp 预期)
44 passed, 1 failed
```

修复过程中暴露并已解决的门禁问题：`NetMailbox` 的 `substr` 拉入 `__throw_out_of_range_fmt`（G7b，改用 `cellular::subview`）；ASan 抓到主机测试构造超长命令越界（测试构造器已按 `args.size()` 钳制）；tidy/doc_check 的魔数、注释、复杂度与特殊成员项。

过程经验（供审查参考）：
- G7b：`std::string_view::substr` 越界路径引入 `__throw_out_of_range_fmt`，使用本组件内不抛异常的 `cellular::subview`。
- G5b：`NetModule.cpp` 不要引入 `<iterator>`（clang 对 xtensa 工具链头误解析主机 `/usr/include`）；`LinkFsm.cpp` 平铺分派函数用具名 `NOLINTBEGIN/END`，理由写在注释。

---

## 4. 资源（逐项；基线 = `main` 的 `builds/main-idf` 数据，本次 = `builds/n1-idf`）

`esp_idf_size`：

| 项 | 基线 | 首轮 N1 | 本次 | 相对基线 |
| --- | --- | --- | --- | --- |
| DRAM（静态） | 98 803 | 102 467 | 99 219 | **+416 B**（禁用态预算 ≤ 1 KB ✔；首轮 +3 664 B） |
| IRAM | 116 139 | 116 139 | 116 139 | 0 ✔ |
| Flash Code | 1 149 284 | 1 168 444 | 1 173 752 | +24 468 B |
| Flash Data | 276 348 | 279 132 | 279 996 | +3 648 B |
| `rtk_main.bin` | 1 595 376 | 1 620 960 | 1 623 872（分区剩余 17%） | +28 496 B |

> Flash 代码+数据合计 +28.1 KB 超过任务卡“Flash ≤ 25 KB”上限（首轮 +21.9 KB）。增量来自复审要求的重同步/令牌/邮箱过滤/PW1 硬切/按槽快照，请 Architect 裁决是否放宽该上限；本轮未做体积削减。

**禁用态（`net.enable=0`、E1、配网模式）**：静态 DRAM 仅 +416 B（`s_app` 由首轮 30 536 B 降到 27 440 B 范围内，4G 运行态不再静态驻留）；不安装 UART1、不分配栈/块、不创建 kNet 执行器；GPIO26 不被触碰（`NetModule::init` 因 `rt_==nullptr` 直接返回）。

**启用态（P2 v1.41、`net.enable=1`）逐项**（目标 ≤ 10 KB，含静态增量与启动期分配）：

| 项 | 字节 | 来源 |
| --- | --- | --- |
| 静态 DRAM 增量 | 416 | `esp_idf_size` 实测 |
| kNet 任务栈 | 4 096 | 启动期 BootArena `task.net` |
| `NetBlock`（`sizeof` 编译期实测 3 136；其中 `Runtime` 2 428） | 3 136 | BootArena `net.block` |
| UART RX 环形缓冲 | 2 048 | `uart_driver_install`（启动期一次） |
| UART TX 环形缓冲 | 0 | 改为 0（写入只阻塞到 FIFO，另有 `uart_wait_tx_done` 50 ms 上限） |
| UART 事件队列 16×`uart_event_t` | ≈ 224 | 同上 |
| 驱动内部（锁/句柄） | ≈ 300（估计，非实测） | IDF |
| **合计** | **≈ 10 220（估计）** | |

合计略高于 10 KB 目标（约 +220 B，估计值）。若 HIL 的栈高水位显示余量充足，可把 `AppContext::kNetStackBytes` 降到 3072（−1 KB）使合计 ≈ 9.2 KB；本轮保持 4096 以免无数据下收紧。该降栈动作留待 HIL 后由 Architect 决定。**启动后 free/min 堆未采集**（无板），见 §8 第 9 步。

---

## 5. 任务栈高水位估计

kNet 栈 4096 B。最深调用链为 `poll → LinkFsm::poll → AtEngine::submit`，状态机/引擎缓冲均在 `NetBlock` 成员中，栈上仅有小型临时量；`StorageModule` 的 NVS 写已移出 kNet（写在 kStorage 的执行器里），因此 kNet 栈不再包含 NVS 写路径。**未实测**，估计在 1–2 KB 量级，属推测；HIL 时用 `sys tasks` 读高水位（余量 > 1 KB 则按 §4 降到 3072）。

---

## 6. AT 超时表与恢复表（均在 `components/cellular/include/cellular/Policy.hpp`）

| 命令 | 超时 |
| --- | --- |
| `AT`/`ATE0`/`AT+CMEE`（同步） | 300 ms |
| `AT+CSQ`/`AT+GSN` | 300 ms |
| `AT+IPR`/`AT+QCCID`/`AT+CPIN?`/`AT+CEREG?`/`AT+QICSGP`/`AT+QIRD` | 1000 ms |
| `AT+QIACT` | 150 000 ms |
| `AT+QIDEACT` | 40 000 ms |
| `AT+QIOPEN` 结果 URC | 150 000 ms |
| `AT+QICLOSE` | 10 000 ms |
| `AT+QISEND` | 5 000 ms |
| `AT+QPOWD=1` / 等 `POWERED DOWN` | 1000 ms / 1500 ms |
| 控制台 `net at` 缺省 | 5000 ms（阻塞等待上限 +1000 ms） |

| 恢复级别 | 触发 | 退避 |
| --- | --- | --- |
| L1 连接 | 单连接失败 | 2 s 起翻倍，封顶 60 s；连续 5 次升 L2 |
| L2 承载 | `QIACT` 失败、`pdpdeact` | 10 s 起翻倍，封顶 120 s；`QIACT` 连续 3 次失败升 L3 |
| L3 模块 | AT 无响应（3 次）、SIM 异常、注册超时 180 s、**Ready 中注册丢失持续 180 s**、手动 `net reset` | PWREN 低 ≥ 2 s 后重上电；30/60/120/300 s，之后保持 300 s |

其他：同步窗口 10 s、重试 500 ms、连续 3 次失败换另一波特率探测；SIM/注册轮询 2 s；Ready 巡检 30 s；启动 115200，`AT+IPR` 切 460800。

---

## 7. 复审项处理与对 ADR/任务卡的偏离

**复审项对照**：#1 重同步/#2–#3 引擎与 FSM 令牌与轮次/#4–#5 `QIOPEN` 超时先 `QICLOSE`/#6 PW1 同一绝对截止 + `onTimeout` 硬切 + 检查 `gpio_set_level`/#7 低电只禁新上电/#8–#9 邮箱归属、关机后拒绝、原子相位镜像、拒绝表与凭据过滤/#10 先提交 NVS 后 `configRev`、读回（`net.pass` 为 `***`/空）/#11 仅启用时分配/#12 持久化归 kStorage、UART 写有界/#13 跨层回归测试/#14 §2 补全/#15 PRODUCTION 隐藏 DEV 动词/#16 逐槽快照/#17 注册丢失 180 s→L3。

**偏离与待裁决**：

1. **重同步探针**：裁决 #1 写的是“一次成功的 `AT` 同步握手”；实现用 `AT+CMEE?` 作探针（`kProbe`）——它只读、无副作用，且其应答行带有可校验的 `+CMEE:` 前缀，能区分“旧命令的迟到 `OK`”与“探针自己的 `OK`”。语义等价（仍是“必须收到本探针的完整应答才恢复”），但字面不同，请确认。
2. **`net.enable` 运行期不生效**：写入成功后 `net status` 显示 `restart pending`，重启后才分配/创建/释放；`net power on` 在未分配时提示 `set net.enable=1 and reboot`。
3. **ADR-0015 “USB 在位关机关 4G（GPIO26 低）”**：现为 PW1 `kQuiesce` 参与者先 `QICLOSE → AT+QPOWD=1 → 等 POWERED DOWN → PWREN 低`，总预算沿用 PW1 起点的绝对截止（≤ 2 s），到点仍未完成由 `onTimeout`/kNet 自身硬切 PWREN；`BoardStartup` 安全输出表对 kCellular 板跳过 PWREN。ADR-0015 文字需 Architect 更新。
4. **`NetVerb::kAt` 不是命令动词**：`net at` 经 `NetModule::submitAt` 邮箱，原因是应答文本 > `fw::kCommandArgBytes`。
5. **Flash 体积超卡上限**（见 §4）与**启用态合计略超 10 KB**（估计，见 §4）待裁决。
6. **量产开通渠道（开放问题，N1 不实现）**：PRODUCTION 档控制台只剩 `net status`，`net.*` 对 web/phone/MQTT 均 locked（`access::kConsoleInternal`）。量产机如何写入 APN/用户名/密码尚无通道（候选：工厂工位串口 `config set`、手机私有协议新键、一次性配网页），需另行决策。
7. **回归测试在基线 `d3800af` 上失败**：对首轮提交的临时 worktree 验证，新版 `test/cellular`（`TestAtEngine`/`TestLinkFsm`）与 `test/settings`（`TestSettingsRegistry`/`TestSettingsService`）因新增 API（`AtCommand` 令牌、`cellular::isCpinLine/isRegQueryLine`、`AtStats` 新字段、`kVerbStorageSetNet*`）**编译失败**，两个套件均为 0/2 通过；`NetLogicCheck`、`net_settings_persistence`、PW1 `onTimeout` 断言依赖新代码，同样无法在 `d3800af` 上构建。这是“编译即失败”级别的证据，不是行为断言级；行为级断言（迟到终结行、同前缀 URC、长度越界、令牌过期、`QIOPEN` 超时 `QICLOSE`、PW1 起点延迟、门控、邮箱过期、设置读回/写失败）均随新测试提交，请审查者按需在基线上单独回放确认。
8. `net status` 仍不含 `QIOPEN` 之外的连接统计细节（N1 无连接使用者），仅暴露逐槽快照字段。

---

## 8. HIL 步骤（P2 v1.41 + 串口 + 已开通 SIM；由 Architect 与用户执行）

```
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
idf.py -B builds/n1-idf build flash monitor        # dev 档位，串口控制台
```

1. 首次启动（`net.enable` 默认 0）：日志无 `net: ready`；`net status` 显示 `disabled; set net.enable=1 and reboot`；万用表确认 GPIO26 低；`net power on` 提示需启用并重启。
2. `config set net.apn <APN>`（及 `net.user`/`net.pass`）→ `config set net.enable 1`；`config get net.pass` 应得 `***`；`net status` 显示 `restart pending`；**重启**。启动日志应出现 `net: ready uart1 tx=33 rx=32 pwren=GPIO26 en=1`。记录：上电→AT 可用→Ready 的时间；`net status` 应见 IMEI、ICCID（部分打码）、CSQ、注册、PDP。
3. `net at ATI`、`net at AT+QGMR`：记录型号与固件（ADR-0021 Q5）；验证 `net at ATE1`、`net at AT+IPR=9600`、`net at AT+QISWTMD=1`、`net at AT+QICSGP=1`、`net at AT+CFUN=1,1` 与含 `;` 的命令被拒绝；`net at AT+CMEE?` 的应答中确认文本类 CME ERROR 分类（CMEE=2）。
4. 注册丢失：拔天线观察注册丢失→L2/L3；**先恢复天线→应回到 L2 重试并最终 Ready**；Ready 中注册丢失持续 180 s 应进入 L3。
5. SIM：**拔 SIM → 等待状态变为 SIM 异常/L3（PWREN 低 ≥ 2 s，退避 30 s 起）→ 再插回 SIM**，观察恢复到 Ready。
6. 460800 稳定性（Q2）：Ready 连续 ≥ 30 min，观察 `net:` 日志无通道失效/溢出。
7. 关机：`power off`（或长按），逻辑分析仪量 GPIO26 下降沿相对触发的时间，应 ≤ 2 s 且收到 `POWERED DOWN`；USB 在位关机与低电关机同样验证；人为让模块无响应时，应在 PW1 截止时刻看到硬切 PWREN（日志 `PW1 timeout, hard-cut PWREN`）。
8. 发射峰值与低电（Q3）：电池放至 ~15% 并保持注册，观察是否掉压/复位；**在线时降到 <10% 模块应保持**，此时断电重启或 `net reset` 后 <10% 应不再上电。
9. 堆与栈：启动后 `sys heap`（free/min）与 `sys tasks`（kNet 栈高水位），回填 §4 与 §5；与 `net.enable=0` 启动的堆对比得出启用成本。
10. 回归：`net.enable 0` 后重启，确认 GPIO26 低、`net status` 为 disabled；E1-5.4 启动日志无 `net:` 行、GPIO26 仍为电源保持；配网模式（Wi-Fi 配置）下 4G 不启动。
