idf_component_register(SRCS "alarm_msg.c"
                       INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wundef -Wswitch-default -Wsign-compare
    -Wfloat-equal -Wimplicit-fallthrough -Wvla)
