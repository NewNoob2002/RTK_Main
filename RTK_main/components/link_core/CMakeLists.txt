# link_core 纯 C++ 组件（ADR-0002 §4，ADR-0007 §3.5）：依赖 device_proto、app_fw，支持 host g++。
# 告警组按 AGENTS.md §3（纯组件全开）。
# ADR-0007 §3.5：旧 CommandDispatcher 已移入 reference/cpp-v3-dispatcher/，不参与构建。
idf_component_register(
    SRCS "AlarmSource.cpp" "LinkManager.cpp" "LinkService.cpp"
         "CommandRouter.cpp"
         "services/DeviceService.cpp" "services/ConfigService.cpp"
         "services/AlarmService.cpp" "services/PairService.cpp"
         "services/GnssService.cpp"
    INCLUDE_DIRS "include"
    REQUIRES device_proto app_fw)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough -Wswitch-default -Wundef)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
