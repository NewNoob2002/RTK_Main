# 组合根与组件注册（ADR-0002 §4）。C 驱动 + C++ 应用（ADR-0003）。
# esp_hw_support：esp_read_mac；esp_system：复位原因/堆/TWDT；console：UART0 REPL。
# esp_driver_gpio：用户授权的 GPIO26 电源保持。
# 模块源码按目录发现；IDF 的依赖扫描以脚本模式运行，不能使用 CONFIGURE_DEPENDS。
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "modules/*.cpp")
    # 模板仅作新增模块参考并由主机测试持续编译，不编入固件。
    list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/_template/")
endif()
idf_component_register(
    SRCS "app_main.cpp"
         "data/app_data.cpp"
         "support/console_support.cpp"
         ${module_sources}
    INCLUDE_DIRS "."
    REQUIRES app_fw device_proto link_core rtos spp_server nvs_store cli stream_demux console esp_system esp_hw_support esp_driver_gpio freertos log)

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，本项目对其他依赖 IDF 的组件
# 同样不开启这三项；其余告警按 AGENTS.md 全开。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)
