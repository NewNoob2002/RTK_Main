# cli 参数解析与回执辅助 host 单元测试工程（ADR-0007 J3 / ADR-0009 T1a）。
# 支持普通构建与 ASan/UBSan 构建。
cmake_minimum_required(VERSION 3.22)
project(cli_tests C CXX)

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

add_library(cli_parse STATIC ${PROJ_ROOT}/components/cli/src/parse.cpp)
target_include_directories(cli_parse BEFORE PUBLIC shim)
target_include_directories(cli_parse PUBLIC ${PROJ_ROOT}/components/cli/include)
target_compile_features(cli_parse PUBLIC cxx_std_20)
target_compile_options(cli_parse PRIVATE ${PROJ_CXX_STRICT})

# reply.cpp 只用 C 标准库；它引用的 reply_state.hpp 与自身同目录，因此只需 cli 公共头与 esp_err shim。
add_library(cli_reply STATIC ${PROJ_ROOT}/components/cli/src/reply.cpp)
target_include_directories(cli_reply BEFORE PUBLIC shim)
target_include_directories(cli_reply PUBLIC ${PROJ_ROOT}/components/cli/include)
target_include_directories(cli_reply PRIVATE ${PROJ_ROOT}/components/cli/src)
target_compile_features(cli_reply PUBLIC cxx_std_20)
target_compile_options(cli_reply PRIVATE ${PROJ_CXX_STRICT})

enable_testing()

add_executable(test_parse test_parse.cpp)
target_link_libraries(test_parse cli_parse unity)
target_compile_options(test_parse PRIVATE ${PROJ_CXX_STRICT})
add_test(NAME test_parse COMMAND test_parse)

add_executable(test_reply test_reply.cpp)
target_link_libraries(test_reply cli_reply unity)
target_include_directories(test_reply PRIVATE ${PROJ_ROOT}/components/cli/src)
target_compile_options(test_reply PRIVATE ${PROJ_CXX_STRICT})
add_test(NAME test_reply COMMAND test_reply)
