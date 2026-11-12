# nvs_store（ADR-0003）：NVS 持久化的 C 实现，只依赖 IDF 的 nvs_flash/log。
# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，本组件只开 AGENTS.md 规定的 C 告警组。
idf_component_register(
    SRCS "nvs_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes>)
