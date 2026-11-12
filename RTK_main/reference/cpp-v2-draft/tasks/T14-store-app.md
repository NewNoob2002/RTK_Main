# T14 rtk_store 与应用组合根、链路任务、测试控制台

```yaml
task: T14-store-app
observable_outcome: >
  idf.py build 0 warning；app ≤ 1.2 MB；`cpp_cost.py check build/rtk_main.elf --tier T2` 中 exceptions、rtti 为 0；
  自有目标文件（build/esp-idf/{rtk_*,main}/）的符号中没有 operator new/delete、malloc、free；host ctest 回归 PASS。
frozen_inputs: [全部公共头, docs/adr/ADR-0002-cpp-esp-idf.md, docs/protocol/phone_protocol.md §6, reference/c-v1/main/*（行为参考）]
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - components/rtk_store/settings_store.cpp
  - main/{app_main.cpp, app_link.cpp, app_link.hpp, app_console.cpp, app_console.hpp, Kconfig.projbuild, CMakeLists.txt}
  - docs/reports/T14.md
affected_checks: [idf.py build, idf.py size, cpp_cost.py, nm 检查, host ctest, clang-format]
stop_conditions: [需要修改冻结头文件]
hardware_gap: HIL-1..HIL-6 全部需要实板
```

rtk_store：NVS 句柄做成 RAII（析构时 `nvs_close`）；所有返回值都检查；blob 长度校验按头文件注释。

启动顺序（app_main）：
1. 记录复位原因。`SettingsStore::init()` 失败时记录错误并继续（白名单为空，SN 用默认值）。
2. 读 SN；没有时按 phone_protocol.md §6 用 MAC 生成。设备名为 `E1-Lite-<SN>`。
3. 读白名单 → `LinkManager::init(policy, ...)`。policy 由 `main/Kconfig.projbuild` 中的 choice `RTK_LINK_POLICY` 决定，默认 CONCURRENT。
4. 构造 `CommandDispatcher` 并填 DeviceInfo（sn、fw_ver = kAppVersion）。
5. `SppServer::instance().init(name)`。失败时记录错误，不启动链路任务，控制台照常启动。
6. 用 `StaticTask` 启动链路任务：core 1，优先级 10，栈 6144 字节（ADR-0001 §5；IDF 栈单位为字节）。
7. 启动 esp_console UART REPL。

全局对象（LinkManager、AlarmSource、CommandDispatcher、LinkService、SettingsStore、任务和队列包装）用 `constinit` 文件作用域对象，或在 app_main 中按顺序构造的静态存储。不使用函数局部静态的动态初始化。

链路任务循环（`for (;;)`）：
- `poll_event(evt, 20)`，按事件类型调用 kOpen → `LinkService::on_open`、kClose → `on_close`、kData → 循环 `read` 直到返回 0 并逐段 `on_data`、kRxDrop → `on_rx_dropped`。
- 每 100 ms 调用一次 `LinkService::tick(now_ms())`。
- `consume_whitelist_dirty()` 为 true 时调用 `SettingsStore::save_alarms(link.whitelist())`，失败只记录日志。
- 处理控制台命令队列（`StaticQueue`，单一所有者）：控制台任务不直接访问 LinkService 等对象。
- 订阅任务看门狗（TWDT），只在一轮循环确实执行完之后喂狗（RTOS-005）。

控制台命令（UART0 115200）：

| 命令 | 作用 |
| --- | --- |
| `status` | 会话表（handle、角色、MAC）、白名单、配对状态、报警状态、解析错误计数、LinkStats、最近心跳、空闲堆与最小空闲堆、链路任务栈高水位 |
| `alarm set <state 0-2> <level 0-3>` / `alarm clear` | 测试覆盖（经命令队列） |
| `pair` | 打开配对窗口 |
| `wl clear` | 清空白名单并持久化 |
| `sn <str>` | 写 SN（重启后生效） |
| `policy` | 打印链路策略 |

控制台需要读的状态由链路任务每次循环发布一个快照结构，用 portMUX 保护拷贝；不直接读链路任务内部的对象。
