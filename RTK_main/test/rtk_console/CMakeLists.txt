# rtk_console 参数解析辅助 host 单元测试工程（ADR-0007，J3 交付）。
# 支持普通构建与 ASan/UBSan 构建。
cmake_minimum_required(VERSION 3.22)
project(rtk_console_tests C CXX)

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

set(RTK_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_library(rtk_console_parse STATIC ${RTK_ROOT}/components/rtk_console/src/parse.cpp)
target_include_directories(rtk_console_parse BEFORE PUBLIC shim)
target_include_directories(rtk_console_parse PUBLIC ${RTK_ROOT}/components/rtk_console/include)
target_compile_features(rtk_console_parse PUBLIC cxx_std_20)
target_compile_options(rtk_console_parse PRIVATE ${RTK_CXX_STRICT})

enable_testing()

add_executable(test_parse test_parse.cpp)
target_link_libraries(test_parse rtk_console_parse unity)
target_compile_options(test_parse PRIVATE ${RTK_CXX_STRICT})
add_test(NAME test_parse COMMAND test_parse)
