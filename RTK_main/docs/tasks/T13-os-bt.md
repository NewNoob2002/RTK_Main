# T13 rtk_os（C++ RAII）与 rtk_bt（C SPP 驱动 + C++ 适配器）（ADR-0003 修订版）

```yaml
task: T13-os-bt
observable_outcome: >
  rtk_os：完成并自查 components/rtk_os/include/rtk/os/ 下已起草的 RAII 头（static_task、static_queue、
  static_stream_buffer、static_event_group、critical_section）与 esp_status.cpp；
  rtk_bt：C 驱动 rtk_bt_spp.c 实现 rtk_bt_spp.h，C++ 适配器 spp_transport.cpp 实现 spp_transport.hpp；
  idf.py build 0 warning；报告逐条回答下方审查清单。
frozen_inputs:
  - components/rtk_os/include/rtk/os/esp_status.hpp
  - components/rtk_bt/include/rtk_bt_spp.h、components/rtk_bt/include/rtk/bt/spp_transport.hpp
  - components/rtk_core/include/rtk/core/transport.hpp
  - reference/c-v1/components/bt_spp/bt_spp.c、reference/c-v1/docs/reports/T04.md
  - ~/esp/esp-idf/examples/bluetooth/bluedroid/classic_bt/bt_spp_acceptor（只读）
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - components/rtk_os/*（esp_status.cpp、RAII 头的修正）
  - components/rtk_bt/{rtk_bt_spp.c, spp_transport.cpp, CMakeLists.txt}
  - docs/reports/T13.md
affected_checks: [idf.py build, idf.py size-components, clang-format, host ctest 回归, 人工审查清单]
stop_conditions:
  - 需要修改冻结头文件
  - IDF v6.1 行为与 T04 核实结论不一致（先复核三条结论再写代码）
hardware_gap: 连接、吞吐、拥塞、scatternet 只能由 HIL 证明
```

rtk_bt C 驱动要点：
- 由 reference/c-v1 的 bt_spp.c 移植：符号改为 `rtk_bt_spp_*`、IDF 风格、`s_` 静态；PIN_REQ 应答改为 `RTK_BT_SPP_LEGACY_PIN`（"1234"）；重复 init 返回 `ESP_ERR_INVALID_STATE`；新增 `rtk_bt_spp_dropped_events()`。
- 会话槽、StreamBuffer（`xStreamBufferCreateStatic`）、事件队列（`xQueueCreateStatic`）、拥塞事件组、`portMUX_TYPE` 全部是文件作用域静态存储；临界区用 `taskENTER_CRITICAL(&s_mux)`，临界区内不调用 FreeRTOS API（ESP-001）。
- 回调只做：拷贝数据、置标志、非阻塞投递（超时 0）。
- 初始化失败按逆序释放，不用 `goto`；`CMakeLists` 在 `CONFIG_SOC_BT_CLASSIC_SUPPORTED` 时才编译驱动，源文件对不支持的目标 `#error`。
- C 驱动不包含任何 C++ 头。

SppTransport：无状态；`write`/`disconnect` 转调 C 函数，`std::span` 拆为指针+长度（空 span 返回 `kInvalidArg`，不调用驱动），错误经 `rtk::os::from_esp_err` 映射。

人工审查清单（报告逐条回答，附文件:行号）：回调中没有阻塞调用；临界区内没有 FreeRTOS API；所有 `esp_err_t` 都检查；write 等待有上界且按剩余时间递减；初始化失败释放资源；没有堆分配（IDF 内部除外）；RAII 包装的构造函数 constexpr、不访问 RTOS。
