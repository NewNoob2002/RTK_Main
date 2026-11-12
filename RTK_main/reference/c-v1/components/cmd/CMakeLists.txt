idf_component_register(SRCS "cmd_disp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES proto link alarm)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wundef -Wswitch-default -Wsign-compare
    -Wfloat-equal -Wimplicit-fallthrough -Wvla)
