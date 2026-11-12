# 通用多协议字节流分流器（ADR-0004）。纯 C11，无 IDF/FreeRTOS 依赖，必须能被 host gcc 与任意 MCU 工具链编译。
idf_component_register(
    SRCS "rtk_demux.c" "rtk_demux_decoders.c"
    INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
    -Wimplicit-fallthrough -Wswitch-default -Wundef -Wstrict-prototypes -Wmissing-prototypes)
