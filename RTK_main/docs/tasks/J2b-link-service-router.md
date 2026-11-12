# J2b LinkService 切换到 CommandRouter（ADR-0007 §3.5）

执行方：pi + workbuddy/deepseek-v4.1-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`（§3.5）、
`docs/reports/J1.md`、`docs/reports/J2a.md`，以及已修订的冻结头 `components/rtk_core/include/rtk/core/link_service.hpp`
（构造函数改为 `LinkService(Transport&, LinkManager&, CommandRouter&, const services::AlarmService&)`）。

## 交付
1. `components/rtk_core/link_service.cpp`：按新头实现。`on_data` 用 `router_.handle(...)`，`tick` 用 `alarm_.build_status_push(...)`；
   其余逻辑（白名单脏标志、心跳记录、断开动作、错误计数）保持不变。
2. `main/bt_link_module.cpp`（J1 产物）：去掉 `CommandDispatcher`，静态构造 `DeviceService`、`ConfigService`、
   `AlarmService(s_alarm, s_link_manager, device.info())`、`PairService(s_link_manager)`（**DeviceService 必须先于 AlarmService 构造**），
   用 4 个 `table()` 组成 `RouteTable` 数组 `init` CommandRouter，`init` 失败按模块 init 失败处理并打日志。
   原来写 `s_dispatcher->info()` 的地方改为 `DeviceService::info()`。保持 J1 报告提到的 include 顺序（冻结 OS 头在产品 `rtk::app` 之前）。
3. 旧分发器退役：
   - `components/rtk_core/command_dispatcher.cpp` 与 `components/rtk_core/include/rtk/core/command_dispatcher.hpp` 移到
     `reference/cpp-v3-dispatcher/`（保持文件名），并从 `components/rtk_core/CMakeLists.txt` 删除。
   - `test/host/test_command_dispatcher.cpp` 移到 `reference/cpp-v3-dispatcher/`。
   - `test/services` 的 golden 测试改为从 `reference/cpp-v3-dispatcher/` 编译旧实现（只读引用，不得修改其内容），继续逐字节比对。
4. **本任务特别授权**修改以下冻结文件（只限所列改动，完成后由 Claude 复核并重新生成 `docs/frozen.sha256`；你不要改 frozen.sha256）：
   - `test/host/CMakeLists.txt`：`rtk_pure` 源列表把 `command_dispatcher.cpp` 换成 `command_router.cpp` 与 `services/*.cpp`，加
     `private_include` 目录；测试列表去掉 `test_command_dispatcher`。
   - `test/host/test_link_service.cpp`：只改对象构造（用 4 个服务 + router 替代 dispatcher），**所有断言与测试用例不得删改**。
   - 上述 3 个文件移入 `reference/`（新增目录）。
5. `tools/main_host_check/CMakeLists.txt` 与其源文件：同步替换源列表，两种策略都通过。
6. 报告 `docs/reports/J2b.md`：改动清单、自检逐项 PASS/FAIL、`test/host` 中每个测试的前后数量对比（test_link_service 用例数不能减少）。

## 自检（报告逐项 PASS/FAIL）
1. `sha256sum -c docs/frozen.sha256`：只允许第 4 条列出的文件报 FAILED/缺失，其余必须 OK；报告里列出全部 FAILED 行。
2. `test/host`（普通 + ASan/UBSan）全部通过；`test/services` 普通 + ASan 全部通过；`test/app_fw`、`test/bt_reconnect`、`test/stream_demux` 不回归。
3. `tools/main_host_check` 两种策略普通 + ASan 通过。
4. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py build`：0 warning；报告 DRAM 静态占用与 app 剩余（对比 J1 报告：DRAM 76,709 B，app 剩余 62.37%）。
5. `grep -rn "CommandDispatcher\|command_dispatcher" components main test tools` 只能出现在注释或 `test/services` 引用 reference 的路径里。
6. clang-format 检查改动文件。

## 禁止
修改第 4 条以外的冻结文件；修改 `components/rtk_core/services/*`、`command_router.cpp`（J2a 产物，确有缺陷时停下写进报告）；
修改 `components/rtk_console`、`components/bt_reconnect`、`components/app_fw`；烧录、连接串口、联网安装、git commit。

完成后在报告最后一行写 `J2b DONE` 或 `J2b BLOCKED: 原因`。
