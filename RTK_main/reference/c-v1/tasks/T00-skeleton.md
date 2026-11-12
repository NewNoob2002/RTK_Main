# T00 工程骨架

```yaml
task: T00-skeleton
observable_outcome: >
  `idf.py set-target esp32 && idf.py build` 在 RTK_main 下成功，0 warning；
  app_main 打印 "RTK_main <版本> boot" 后正常返回（任务在后续任务中加入）。
frozen_inputs: [AGENTS.md, sdkconfig.defaults, partitions.csv, docs/adr/ADR-0001-architecture.md]
owner: omp/DeepSeek-V4.1-Flash
outputs:
  - CMakeLists.txt（顶层，project(rtk_main)）
  - main/CMakeLists.txt, main/rtk_main.c, main/app_version.h
  - components/{proto,link,alarm,cmd}/CMakeLists.txt（此时 SRCS 指向 T01–T03 将创建的 .c；可先放只含 #include 的空实现以便本任务构建通过）
  - docs/reports/T00.md
affected_checks: [idf.py build, idf.py size]
stop_conditions:
  - 需要修改 sdkconfig.defaults 或 partitions.csv 才能构建
  - IDF 导出脚本失败
hardware_gap: 无（不烧录）
```

要求：
1. 顶层 `CMakeLists.txt` 只包含 IDF 标准三行，不添加 Arduino 或任何 managed component。
2. 纯 C 组件（proto/link/alarm/cmd）的 `CMakeLists.txt`：`idf_component_register(SRCS ... INCLUDE_DIRS include REQUIRES <依赖的纯 C 组件>)`，并 `target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wshadow -Wundef -Wswitch-default -Wsign-compare -Wfloat-equal -Wimplicit-fallthrough -Wvla)`。cmd 依赖 proto、link、alarm。不得 REQUIRES 任何 IDF 组件。
3. `main/app_version.h` 定义 `APP_VERSION_STR "6.1.0-dev"`。
4. `sdkconfig` 由构建生成，不提交冲突修改；`build/` 不纳入报告。
5. 报告中记录 `idf.py size` 的 Flash/DRAM 数字。
