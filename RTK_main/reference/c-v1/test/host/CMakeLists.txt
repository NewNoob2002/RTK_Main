# Host 单元测试（Unity，取自 ESP-IDF 自带副本，不额外安装）。
# 冻结输入：实现方不得修改本目录。
cmake_minimum_required(VERSION 3.16)
project(rtk_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
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

# BARR-C 推荐告警（assets/barr-c/COMPILER_FLAGS.md），只作用于被测代码与测试。
set(RTK_WARN
    -Wall -Wextra -Werror -Werror=implicit-function-declaration
    -Werror=parentheses -Wfloat-equal -Wsign-compare -Wshadow -Wundef
    -Wswitch-default -Wimplicit-fallthrough -Wvla)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_DIR})

add_library(rtk_pure STATIC
    ${RTK_ROOT}/components/proto/proto_frame.c
    ${RTK_ROOT}/components/link/link_mgr.c
    ${RTK_ROOT}/components/alarm/alarm_msg.c
    ${RTK_ROOT}/components/cmd/cmd_disp.c)
target_include_directories(rtk_pure PUBLIC
    ${RTK_ROOT}/components/proto/include
    ${RTK_ROOT}/components/link/include
    ${RTK_ROOT}/components/alarm/include
    ${RTK_ROOT}/components/cmd/include)
target_compile_options(rtk_pure PRIVATE ${RTK_WARN})

enable_testing()
foreach(name test_proto_frame test_link_mgr test_alarm_msg test_cmd_disp)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} rtk_pure unity)
    target_compile_options(${name} PRIVATE ${RTK_WARN})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
