# T14 rtk_store（C）与应用组合根、链路任务、控制台（C++）（ADR-0003 修订版）

```yaml
task: T14-store-app
observable_outcome: >
  idf.py build 0 warning；app ≤ 1.2 MB；`python3 -I tools/g7b_check/g7b_check.py --build <构建目录>` PASS（G7b 2026-11-11 修订：自有目标文件（build/esp-idf/{rtk_*,main}/）
  无异常/RTTI 及 operator new/delete、malloc、free 符号；全 ELF 只允许 IDF NVS 经 libstdc++ 引入的 6 个白名单符号且拉入链固定）；host ctest 回归 PASS。
frozen_inputs: [全部公共头, docs/adr/ADR-0002-cpp-esp-idf.md, docs/adr/ADR-0003-c-cpp-split.md, docs/protocol/phone_protocol.md §6, reference/c-v1/components/store/store_nvs.c, reference/c-v1/main/*（行为参考）]
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - components/rtk_store/{rtk_store.c, CMakeLists.txt}
  - main/{app_main.cpp, app_link.cpp, app_link.hpp, app_console.cpp, app_console.hpp, Kconfig.projbuild, CMakeLists.txt}
  - docs/reports/T14.md
affected_checks: [idf.py build, idf.py size, cpp_cost.py, nm 检查, host ctest, clang-format]
stop_conditions: [需要修改冻结头文件]
hardware_gap: HIL-1..HIL-6 全部需要实板
```

rtk_store（C）：由 reference/c-v1 的 store_nvs.c 移植，符号改为 `rtk_store_*`；每次 `nvs_open`→操作→`nvs_close`（所有路径都关闭句柄），写后 `nvs_commit`；长度与 blob 校验按头文件注释。

main（C++）：
- 启动顺序、链路任务循环、控制台命令、快照发布、TWDT 要求与 reference/cpp-v2-draft/tasks/T14-store-app.md 相同，以下几处按 ADR-0003 调整：
  - 持久化直接调用 C 接口 `rtk_store_*`；`std::span<const BtAddr>` 与 `uint8_t[][6]` 的转换写在 `app_link.cpp` 的一个小函数中，用 `static_assert(sizeof(BtAddr) == 6)` 等锁定布局。
  - 传输层：`rtk::bt::SppTransport` 传给 `LinkService`；事件循环调用 `rtk_bt_spp_poll_event`/`rtk_bt_spp_read`。
  - 任务、队列使用 rtk_os 的 RAII 包装；全局对象 `constinit` 或 app_main 中按顺序构造的静态存储，不用函数局部静态的动态初始化。
- 链路任务：core 1、优先级 10、栈 6144 字节，静态创建；控制台只经命令队列与快照访问链路状态（单一所有者）。
