# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
idf_component_register(
    SRCS "peer.c" "reconnect_bridge.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash rtk_proto bt_reconnect esp_driver_uart esp_timer esp_hw_support)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough>)
