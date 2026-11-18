# 组合根与组件注册（ADR-0002 §4）。C 驱动 + C++ 应用（ADR-0003）。
# esp_hw_support：esp_read_mac；esp_system：复位原因/堆/TWDT；console：UART0 REPL。
# esp_driver_gpio：用户授权的 GPIO26 电源保持。esp_driver_uart：debug 模块整行写控制台 UART。
# 模块源码按目录发现；IDF 的依赖扫描以脚本模式运行，不能使用 CONFIGURE_DEPENDS。
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "modules/*.cpp")
    # 模板仅作新增模块参考并由主机测试持续编译，不编入固件。
    list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/_template/")
    # ADR-0009 §2.6：追踪关闭时整个 debug 模块不编译（module_list.inc 与 app_main 同步按 FW_TRACE_ENABLED 排除）。
    if(DEFINED CONFIG_RTK_TRACE_ENABLE AND NOT CONFIG_RTK_TRACE_ENABLE)
        list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/debug/")
    endif()
    # ADR-0010 §2.7：GNSS 关闭时整个 gnss 模块不编译。
    if(DEFINED CONFIG_RTK_GNSS_ENABLE AND NOT CONFIG_RTK_GNSS_ENABLE)
        list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/gnss/")
    endif()
endif()
idf_component_register(
    SRCS "AppMain.cpp"
         "app/App.cpp"
         "app/BootBuffers.cpp"
         "app/BootFlow.cpp"
         "app/BootSteps.cpp"
         "app/ConsoleSetup.cpp"
         "app/Executors.cpp"
         "data/AppData.cpp"
         "data/GnssOutput.cpp"
         "support/ConsoleSupport.cpp"
         "support/ResetReason.cpp"
         ${module_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES espcoredump
    REQUIRES app_fw device_proto link_core rtos spp_server nvs_store cli stream_demux gnss console esp_system esp_hw_support esp_driver_gpio esp_driver_uart freertos log)

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，本项目对其他依赖 IDF 的组件
# 同样不开启这三项；其余告警按 AGENTS.md 全开。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
