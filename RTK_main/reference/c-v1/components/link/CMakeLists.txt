idf_component_register(SRCS "link_mgr.c"
                       INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wundef -Wswitch-default -Wsign-compare
    -Wfloat-equal -Wimplicit-fallthrough -Wvla)
