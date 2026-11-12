# Host 单元测试（冻结输入，实现方不得修改）。
# Unity 取自 ESP-IDF 自带副本（esp-idf profile：IDF Unity；纯逻辑 host 测试）。
# ADR-0003：rtk_proto 的协议核心是 C（C11），C++ 外观与 rtk_core 是 C++20；两种语言的测试都在这里。
# 与目标一致的语言运行时约束：-fno-exceptions -fno-rtti（CPP-001/002）。
cmake_minimum_required(VERSION 3.22)
project(rtk_host_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

# 纯 C++ 组件的严格告警组（assets/cpp/compile_flags.md），对被测代码与测试同样生效。
set(RTK_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

# C 源文件严格告警（IDF 风格 + BARR-C 安全核心中可由编译器检查的部分）。
set(RTK_C_STRICT
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align
    -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes
    -Wfloat-equal -Werror=implicit-function-declaration)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_library(rtk_proto_c STATIC
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_frame.c
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_alarm.c)
target_include_directories(rtk_proto_c PUBLIC ${RTK_ROOT}/components/rtk_proto/include)
target_compile_options(rtk_proto_c PRIVATE ${RTK_C_STRICT})

add_library(rtk_pure STATIC
    ${RTK_ROOT}/components/rtk_proto/status.cpp
    ${RTK_ROOT}/components/rtk_proto/bytes.cpp
    ${RTK_ROOT}/components/rtk_proto/frame.cpp
    ${RTK_ROOT}/components/rtk_proto/alarm_codec.cpp
    ${RTK_ROOT}/components/rtk_core/alarm_source.cpp
    ${RTK_ROOT}/components/rtk_core/link_manager.cpp
    ${RTK_ROOT}/components/rtk_core/command_router.cpp
    ${RTK_ROOT}/components/rtk_core/services/device_service.cpp
    ${RTK_ROOT}/components/rtk_core/services/config_service.cpp
    ${RTK_ROOT}/components/rtk_core/services/alarm_service.cpp
    ${RTK_ROOT}/components/rtk_core/services/pair_service.cpp
    ${RTK_ROOT}/components/rtk_core/link_service.cpp)
target_include_directories(rtk_pure PUBLIC
    ${RTK_ROOT}/components/rtk_proto/include
    ${RTK_ROOT}/components/rtk_core/include)
# services/*.cpp 共用组件私有头 service_support.hpp。
target_include_directories(rtk_pure PRIVATE ${RTK_ROOT}/components/rtk_core/private_include)
target_link_libraries(rtk_pure PUBLIC rtk_proto_c)
target_compile_options(rtk_pure PRIVATE ${RTK_CXX_STRICT})

enable_testing()
foreach(name test_bytes test_frame test_alarm_codec test_link_manager test_link_service)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} rtk_pure unity)
    target_compile_options(${name} PRIVATE ${RTK_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# C 接口测试只链接 C 库，证明报警器固件（纯 C）可单独使用 rtk_proto 的 C 部分。
foreach(name test_proto_frame_c test_proto_alarm_c)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} rtk_proto_c unity)
    target_compile_options(${name} PRIVATE ${RTK_C_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
