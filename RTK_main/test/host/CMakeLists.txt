# Host 单元测试（冻结输入，实现方不得修改）。
# Unity 取自 ESP-IDF 自带副本（esp-idf profile：IDF Unity；纯逻辑 host 测试）。
# ADR-0003：device_proto 的协议核心是 C（C11），C++ 外观与 link_core 是 C++20；两种语言的测试都在这里。
# 与目标一致的语言运行时约束：-fno-exceptions -fno-rtti（CPP-001/002）。
cmake_minimum_required(VERSION 3.22)
project(host_tests C CXX)

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

set(PROJ_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# 纯 C++ 组件的严格告警组（assets/cpp/compile_flags.md），对被测代码与测试同样生效。
set(PROJ_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

# C 源文件严格告警（IDF 风格 + BARR-C 安全核心中可由编译器检查的部分）。
set(PROJ_C_STRICT
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align
    -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes
    -Wfloat-equal -Werror=implicit-function-declaration)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_library(dev_proto_c STATIC
    ${PROJ_ROOT}/components/device_proto/dev_proto_frame.c
    ${PROJ_ROOT}/components/device_proto/dev_proto_alarm.c)
target_include_directories(dev_proto_c PUBLIC ${PROJ_ROOT}/components/device_proto/include)
target_compile_options(dev_proto_c PRIVATE ${PROJ_C_STRICT})

add_library(host_pure STATIC
    ${PROJ_ROOT}/components/app_fw/src/status.cpp
    ${PROJ_ROOT}/components/app_fw/src/trace.cpp
    ${PROJ_ROOT}/components/device_proto/bytes.cpp
    ${PROJ_ROOT}/components/device_proto/frame.cpp
    ${PROJ_ROOT}/components/device_proto/alarm_codec.cpp
    ${PROJ_ROOT}/components/link_core/alarm_source.cpp
    ${PROJ_ROOT}/components/link_core/link_manager.cpp
    ${PROJ_ROOT}/components/link_core/command_router.cpp
    ${PROJ_ROOT}/components/link_core/services/device_service.cpp
    ${PROJ_ROOT}/components/link_core/services/config_service.cpp
    ${PROJ_ROOT}/components/link_core/services/alarm_service.cpp
    ${PROJ_ROOT}/components/link_core/services/pair_service.cpp
    ${PROJ_ROOT}/components/link_core/link_service.cpp)
target_include_directories(host_pure PUBLIC
    ${PROJ_ROOT}/components/app_fw/include
    ${PROJ_ROOT}/components/device_proto/include
    ${PROJ_ROOT}/components/link_core/include)
target_link_libraries(host_pure PUBLIC dev_proto_c)
target_compile_options(host_pure PRIVATE ${PROJ_CXX_STRICT})

enable_testing()
foreach(name test_bytes test_frame test_alarm_codec test_link_manager test_link_service)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} host_pure unity)
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# C 接口测试只链接 C 库，证明报警器固件（纯 C）可单独使用 device_proto 的 C 部分。
foreach(name test_proto_frame_c test_proto_alarm_c)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} dev_proto_c unity)
    target_compile_options(${name} PRIVATE ${PROJ_C_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
