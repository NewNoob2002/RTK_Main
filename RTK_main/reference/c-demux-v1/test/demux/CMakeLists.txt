# 通用多协议字节流分流器黑盒测试（ADR-0004，G3 交付）。
# 独立 host CMake 工程，不修改冻结的 test/host/。
cmake_minimum_required(VERSION 3.22)
project(rtk_demux_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(DEFINED ENV{IDF_PATH})
    set(IDF_DIR $ENV{IDF_PATH})
else()
    set(IDF_DIR $ENV{HOME}/esp/esp-idf)
endif()
set(UNITY_DIR ${IDF_DIR}/components/unity/unity/src)
if(NOT EXISTS ${UNITY_DIR}/unity.c)
    message(FATAL_ERROR "Unity not found at ${UNITY_DIR}")
endif()

set(RTK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# 严格 C11 告警集（G3-demux-tests.md / ADR-0004 §8）
set(RTK_DEMUX_C_STRICT
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align
    -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_library(rtk_demux_sut STATIC
    ${RTK_ROOT}/components/rtk_demux/rtk_demux.c
    ${RTK_ROOT}/components/rtk_demux/rtk_demux_decoders.c
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_frame.c
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_demux.c)
target_include_directories(rtk_demux_sut PUBLIC
    ${RTK_ROOT}/components/rtk_demux/include
    ${RTK_ROOT}/components/rtk_proto/include)
target_compile_options(rtk_demux_sut PRIVATE ${RTK_DEMUX_C_STRICT})

add_library(demux_gen STATIC demux_gen.c)
target_include_directories(demux_gen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTK_ROOT}/components/rtk_proto/include)
target_link_libraries(demux_gen PUBLIC rtk_demux_sut)
target_compile_options(demux_gen PRIVATE ${RTK_DEMUX_C_STRICT})

enable_testing()

foreach(name test_demux_gen test_demux_engine test_demux_decoders test_demux_stream)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} rtk_demux_sut demux_gen unity)
    target_compile_options(${name} PRIVATE ${RTK_DEMUX_C_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
