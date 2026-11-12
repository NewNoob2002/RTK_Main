# T10 C++ 工程骨架

```yaml
task: T10-skeleton
observable_outcome: >
  在 RTK_main 下 `idf.py set-target esp32 && idf.py build` 成功（0 warning、0 error）。
  app_main 打印 "RTK_main <版本> boot" 与复位原因（esp_reset_reason，FAULT-001）后返回；
  六个组件的 CMakeLists 按 AGENTS.md §3 的依赖与告警组注册。
frozen_inputs: [AGENTS.md, docs/adr/ADR-0002-cpp-esp-idf.md, sdkconfig.defaults, partitions.csv, components/*/include/**]
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - CMakeLists.txt（已有，不改）
  - components/{rtk_proto,rtk_core,rtk_os,rtk_bt,rtk_store}/CMakeLists.txt
  - 各组件的 .cpp 文件：先写只含 #include 的空实现，保证本任务能构建，后续任务补全
  - main/CMakeLists.txt、main/app_main.cpp、main/app_version.hpp
  - docs/reports/T10.md
affected_checks: [idf.py build, idf.py size]
stop_conditions:
  - 需要修改冻结文件才能构建
  - IDF 导出脚本失败
hardware_gap: 无（不烧录）
```

要求：
1. 纯组件源文件名必须与 `test/host/CMakeLists.txt` 一致：
   - rtk_proto：`status.cpp bytes.cpp frame.cpp alarm_codec.cpp`
   - rtk_core：`alarm_source.cpp link_manager.cpp command_dispatcher.cpp link_service.cpp`
2. rtk_bt 的 CMakeLists：只有 `CONFIG_SOC_BT_CLASSIC_SUPPORTED` 时编译 `spp_server.cpp`；源文件开头写 `#if !SOC_BT_CLASSIC_SUPPORTED` → `#error`，说明原因（ADR-0002 §7）。
3. `main/app_main.cpp`：`extern "C" void app_main(void)`；版本放在 `main/app_version.hpp`，定义 `inline constexpr std::string_view kAppVersion = "6.1.0-dev";`。
4. 报告记录 `idf.py size` 的 Flash/DRAM/IRAM。
