# Generated-by: pi + codex/gpt-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 组合根与组件注册（ADR-0002 §4）。C 驱动 + C++ 应用（ADR-0003）。
# esp_hw_support：esp_read_mac；esp_system：复位原因/堆/TWDT；console：UART0 REPL。
# esp_driver_gpio：用户授权的 GPIO26 电源保持。
idf_component_register(
    SRCS "app_main.cpp"
         "data/app_data.cpp"
         "support/console_support.cpp"
         "modules/bt_link/bt_link_module.cpp"
         "modules/bt_link/bt_link_cmds.cpp"
         "modules/bt_link/async_tx.cpp"
         "modules/rtcm/rtcm_module.cpp"
         "modules/rtcm/rtcm_cmds.cpp"
         "modules/rtcm/rtcm_forwarder.cpp"
         "modules/storage/storage_module.cpp"
         "modules/storage/storage_cmds.cpp"
         "modules/system/system_module.cpp"
         "modules/system/system_cmds.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_fw rtk_proto rtk_core rtk_os rtk_bt rtk_store rtk_console stream_demux console esp_system esp_hw_support esp_driver_gpio freertos log)

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，本项目对其他依赖 IDF 的组件
# 同样不开启这三项；其余告警按 AGENTS.md 全开。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)
