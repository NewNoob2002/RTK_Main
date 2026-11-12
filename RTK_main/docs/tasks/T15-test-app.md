# T15 test_apps/rtk_pure：双架构构建门禁（ESP-008）

```yaml
task: T15-test-app
observable_outcome: >
  test_apps/rtk_pure 分别以 esp32（Xtensa）和 esp32c3（RISC-V）两个目标构建成功，0 warning；
  应用在目标上用 Unity 运行冒烟用例：C 接口（CRC 校验值、组帧后再解析、0x41 心跳编解码）与 C++ 接口（ByteWriter 越界粘滞、LinkManager 一次配对流程）各至少两个；测试文件分 test_main.cpp 与 test_proto_c.c。
  上板运行记 NOT_RUN。
frozen_inputs: [docs/adr/ADR-0003-c-cpp-split.md, components/rtk_proto/include/**, components/rtk_core/include/**, docs/adr/ADR-0002-cpp-esp-idf.md §6-§7]
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - test_apps/rtk_pure/{CMakeLists.txt, sdkconfig.defaults, main/CMakeLists.txt, main/test_main.cpp, main/test_proto_c.c}
  - test_apps/rtk_pure/pytest_rtk_pure.py（pytest-embedded，只写不运行）
  - docs/reports/T15.md
affected_checks:
  - cd test_apps/rtk_pure && idf.py -B build-esp32 set-target esp32 build
  - cd test_apps/rtk_pure && idf.py -B build-esp32c3 set-target esp32c3 build
stop_conditions: [纯组件在 RISC-V 目标上编译失败且原因在冻结头文件中]
hardware_gap: 上板运行需要开发板与烧录授权
```

要求：用 `set(EXTRA_COMPONENT_DIRS ../../components/rtk_proto ../../components/rtk_core)` 只引入两个纯组件，不得引入 rtk_bt、rtk_os、rtk_store。测试代码不使用堆。
