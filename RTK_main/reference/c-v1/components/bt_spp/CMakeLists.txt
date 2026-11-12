idf_component_register(SRCS "bt_spp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt)

# 注：不启用 -Wundef。IDF v6.1 公共头（esp_log*.h 等）使用
# `#if BOOTLOADER_BUILD` 这类未定义宏，-Wundef 会把 IDF 头变成错误。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wswitch-default -Wsign-compare
    -Wfloat-equal -Wimplicit-fallthrough -Wvla)
