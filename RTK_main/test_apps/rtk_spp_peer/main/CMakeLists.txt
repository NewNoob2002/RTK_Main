# HIL 对端主组件：peer.c（真实 SPP 服务端）+ reconnect_bridge.cpp（复用 bt_reconnect 状态机）。
idf_component_register(
    SRCS "peer.c" "reconnect_bridge.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash device_proto bt_reconnect esp_driver_uart esp_timer esp_hw_support)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough>)

# 调试阶段告警即错误（cmake/strict_warnings.cmake）
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
