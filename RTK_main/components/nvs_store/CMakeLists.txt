# nvs_store（ADR-0003）：NVS 持久化的 C 实现，依赖 IDF 的 nvs_flash/log 与框架基座 app_fw（仅 fw/LogC.h，AGENTS.md §3）。
# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，本组件只开 AGENTS.md 规定的 C 告警组。
idf_component_register(
    SRCS "NvsStore.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log app_fw)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes>)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
