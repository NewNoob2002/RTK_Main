# IDF 依赖组件（rtos，ADR-0002 §4）。IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，
# 因此这组不开启；其余告警按 AGENTS.md §3 全开（-Werror）。
idf_component_register(
    SRCS "esp_status.cpp" "executor.cpp" "boot_alloc.cpp"
    INCLUDE_DIRS "include"
    REQUIRES device_proto app_fw freertos esp_timer log esp_system
    # esp_status.hpp 要求映射 ESP_ERR_NVS_NOT_FOUND；该宏只定义在 nvs.h，
    # 因此仅对实现文件暴露 nvs_flash（不进入公共依赖）。
    PRIV_REQUIRES nvs_flash heap)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
