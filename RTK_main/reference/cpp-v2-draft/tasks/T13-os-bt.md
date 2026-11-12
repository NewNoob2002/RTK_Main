# T13 rtk_os 与 rtk_bt：RTOS 包装与 SPP 多会话服务端

```yaml
task: T13-os-bt
observable_outcome: >
  rtk_os 实现 esp_status.hpp，并提供自己设计的 RAII 包装（头文件放在 components/rtk_os/include/rtk/os/ 下的新文件中，
  不修改已冻结的 esp_status.hpp）；rtk_bt 实现 spp_server.hpp；idf.py build 0 warning；
  报告给出每个回调中执行的操作清单，并说明它们都不会阻塞。
frozen_inputs:
  - components/rtk_os/include/rtk/os/esp_status.hpp
  - components/rtk_bt/include/rtk/bt/spp_server.hpp
  - components/rtk_core/include/rtk/core/transport.hpp
  - reference/c-v1/components/bt_spp/bt_spp.c、reference/c-v1/docs/reports/T04.md（IDF 核实结论）
  - ~/esp/esp-idf/examples/bluetooth/bluedroid/classic_bt/bt_spp_acceptor（只读参考）
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - components/rtk_os/{esp_status.cpp, 其他 .cpp}、components/rtk_os/include/rtk/os/<新头>.hpp
  - components/rtk_bt/spp_server.cpp
  - docs/reports/T13.md
affected_checks: [idf.py build, idf.py size-components, clang-format, 人工审查清单（见下）]
stop_conditions:
  - 需要修改冻结头文件
  - 发现 IDF v6.1 行为与 T04 的核实结论不一致
hardware_gap: 连接、吞吐、拥塞、scatternet 只能由 HIL 证明
```

rtk_os 需要提供（命名、形式自定，必须满足约束）：
- `StaticTask`：栈与 TCB 都是静态存储（模板参数指定栈字节数）；`start(name, prio, core, entry, arg)` 返回 `Status`；入口用 `noexcept` 静态蹦床。
- `StaticQueue<T, N>`、`StaticStreamBuffer<N>`、`StaticEventGroup`：内部存储为成员，构造 `constexpr`，显式 `init()`。
- `CriticalSection`：持有 `portMUX_TYPE&` 的 RAII 守卫（任务上下文用）；ISR 中不使用。
- `from_esp_err` 按头文件注释中的映射表实现；`now_ms` 用 `esp_timer_get_time() / 1000`，窄化处加注释。

rtk_bt 要点：
- 驱动状态（会话槽、StreamBuffer、事件队列、拥塞事件组、portMUX）全部放在 `spp_server.cpp` 匿名命名空间的 `s_` 静态存储中；Bluedroid 的 GAP/SPP 回调是 `noexcept` 静态函数，只做三件事：拷贝数据、置标志、非阻塞投递。
- 初始化顺序与失败时的逆序释放沿用 C 版（T04 报告 §3）；不使用 `goto`。
- 设备名校验按头文件注释；PIN 用 `kLegacyPin`（"1234"）。
- 会话表满时立即 `esp_spp_disconnect` 新连接并投递 kClose 事件，同时记录日志。

人工审查清单（写进报告并逐条回答）：回调中没有阻塞调用；临界区内没有 FreeRTOS API；所有 `esp_err_t` 都检查了；write 的等待有上界；初始化失败时资源被释放；没有堆分配。
