# C4 恢复出厂、工厂擦除与蓝牙配对窗口

- 依据：ADR-0014 §2.7、§2.8；用户决定见 ADR-0014 与 memory（配对窗口方案、始终可发现、首次开机不自动开窗口）。
- 基线：bbc23f0。实现：codex；Architect（Claude）验收、提交、重算 frozen。
- 诊断开关：`dbg.*` 的早期加载已在 C3 完成，`debug save` 已可持久化；C4 只需保证 `freset` 后恢复缺省，不另做功能。

## 1 恢复出厂

1. 顶层命令 `freset`：无参数只打印将清除/保留的内容和用法；`freset yes` 执行。
   - 清除：用户分区 `nvs` 整体擦除（用户配置各组、`dbg.*`、`boot/fails`、gnss.cache、报警器白名单，以及 Bluedroid 存在该分区的蓝牙绑定）。
   - 保留：`nvs_fac`（SN、注册码槽位）、`phy_init`、`coredump`、接收机自身配置（下次启动由 NVS 缺省重写 RAM；C2 的开机波特率对齐负责把接收机拉回缺省 115200 并保存）。
   - 顺序：拒绝新的存储写入（白名单去抖、键保存）→ 在拥有 NVS 的任务上下文擦除 → 打 I `Factory reset done, restarting` → `esp_restart()`。擦除失败打 E、输出 ERR、不重启。
2. `factory erase yes`（需 `factory unlock`）：在 `freset` 基础上额外擦除 `nvs_fac`，然后重启。
3. NVS 擦除放进 `components/nvs_store`：新增 `nvs_store_erase_user()` 与 `nvs_store_erase_factory()`（`include/NvsStore.h` 为 frozen 头，本卡授权新增这两个函数声明，不改已有声明）；`test/nvs_store` 补用例。

## 2 蓝牙配对窗口

1. 统一窗口：现有 `LinkManager::pairOpen` 的 60 s 窗口（`kPairWindowMs`，手机 0x44 / `factory pair` 打开，用于报警器 HELLO 准入）同时作为 GAP 配对授权窗口，只保留一个窗口状态。
2. GAP 授权（`components/spp_server`，SppServerGap.c / SppServerSession.c 的 PIN/CFM 应答）：
   - 窗口打开 → 接受（PIN 1234 / SSP confirm 照旧）。
   - 窗口关闭 → 地址在报警器白名单内则接受，否则拒绝（`esp_bt_gap_pin_reply(false)` / `esp_bt_gap_ssp_confirm_reply(false)`），打一行 I `Pairing rejected from XX:..:XX (window closed)`。
   - 已绑定设备重连不触发 PIN/CFM，不受影响。设备始终可被发现（不改 discoverable 模式）。
   - spp_server 是 C 组件，窗口状态和白名单查询通过一个回调/查询函数注入（不得反向依赖 link_core）；说明线程上下文（GAP 回调在 BTC 任务，回复已经由 ctrl 队列投递）。
3. 控制台 `bt pair`：
   - `bt pair open [s]`：打开窗口，s 取 10..600，缺省 60；`bt pair close`；`bt pair` 显示状态与剩余秒数。
   - `bt pair open [s] confirm`：逐个确认模式。每个请求打印 `Pair request from XX:..:XX, accept with 'bt pair accept'`，30 s 内 `bt pair accept` / `bt pair reject`，超时拒绝；同时只挂起一个请求，第二个直接拒绝。
   - 窗口内配对成功后打印地址，并提示可用 `bt trust <MAC>` 把它加入报警器白名单（新增顶层动词 `bt trust <MAC>` / `bt untrust <MAC>`，复用现有白名单存储与 1–4 上限；`factory wl-*` 保留）。
   - `factory pair` 保留为 `bt pair open` 的别名。
4. 不做首次开机自动开窗口；配对窗口状态不持久化。

## 3 测试与门禁

- 主机：link_core 窗口统一与超时、确认模式状态机（接受/拒绝/超时/第二请求）、白名单旁路；nvs_store 两个擦除函数；main_host_check 覆盖 `freset` 参数与流程（擦除失败不重启）。
- 门禁同 C3：idf `builds/c4` 0 warning；所有主机套件普通 + ASan 通过；G5b（含 --nvs-build）0；G7b；clang-format；doc_check `--base bbc23f0`；frozen 仅 `components/nvs_store/include/NvsStore.h` 可失败；bin 增量记录。
- Architect 将执行的 HIL：`freset yes` 后重启、各组 `0 saved`、SN 保留、旧绑定手机需重新配对；窗口关闭时新手机配对被拒；`bt pair open 60` 后配对成功；`confirm` 模式接受/拒绝；`bt trust`。

## 4 交付

- 报告 `docs/reports/C4.md`（改动、设计取舍、门禁、未覆盖项）；不提交、不改 `docs/frozen.sha256`；完成后输出单独一行 `C4 DONE`。
