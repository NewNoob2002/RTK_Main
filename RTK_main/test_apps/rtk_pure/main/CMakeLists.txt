# WHOLE_ARCHIVE 保留 C/C++ TEST_CASE 的静态注册，避免未直接引用的用例被链接器丢弃。
idf_component_register(
    SRCS "test_main.cpp" "test_proto_c.c"
    PRIV_REQUIRES device_proto link_core unity
    WHOLE_ARCHIVE)

# 显式追加方言选项：IDF 在全局编译选项中指定默认标准，仅设置 CMake STANDARD 属性不会覆盖它。
# IDF/Unity 宏会触发 -Wold-style-cast、-Wconversion、-Wundef，按 AGENTS.md §3 不开启这三项。
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=c11 -Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20 -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
        -Wimplicit-fallthrough>)

# 调试阶段告警即错误（cmake/strict_warnings.cmake）
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
