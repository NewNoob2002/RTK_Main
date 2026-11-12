# bt_reconnect 纯 C++20 组件 host 单元测试工程（ADR-0007 §5，J4 交付）。
# 支持普通构建与 ASan/UBSan 构建。
cmake_minimum_required(VERSION 3.22)
project(bt_reconnect_tests C CXX)

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

set(PROJ_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_subdirectory(${PROJ_ROOT}/components/bt_reconnect bt_reconnect_build)
target_compile_options(bt_reconnect PRIVATE -fno-exceptions -fno-rtti)

enable_testing()

add_executable(test_bt_reconnect test_reconnect.cpp)
target_link_libraries(test_bt_reconnect bt_reconnect unity)
target_compile_options(test_bt_reconnect PRIVATE ${PROJ_CXX_STRICT})
add_test(NAME test_bt_reconnect COMMAND test_bt_reconnect)
