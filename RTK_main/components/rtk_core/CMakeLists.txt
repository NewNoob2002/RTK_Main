# rtk_core 纯 C++ 组件（ADR-0002 §4，ADR-0007 §3.5）：只依赖 rtk_proto，必须能被 host g++ 编译。
# 告警组按 AGENTS.md §3（纯组件全开）。
# services/ 下的 4 个服务共用私有头 private_include/service_support.hpp（不进公共 include 目录）。
# ADR-0007 J2b：旧 CommandDispatcher 已移入 reference/cpp-v3-dispatcher/，不参与构建。
idf_component_register(
    SRCS "alarm_source.cpp" "link_manager.cpp" "link_service.cpp"
         "command_router.cpp"
         "services/device_service.cpp" "services/config_service.cpp"
         "services/alarm_service.cpp" "services/pair_service.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES rtk_proto)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough -Wswitch-default -Wundef)
