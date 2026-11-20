# web_proto 纯 C++20 组件 host 单元测试工程（ADR-0017 §2.5）。
# 覆盖请求解析、JSON 输出器与错误码映射；支持普通构建与 ASan/UBSan 构建。
cmake_minimum_required(VERSION 3.22)
project(web_proto_tests C CXX)

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

add_subdirectory(${PROJ_ROOT}/components/app_fw app_fw_build)
add_subdirectory(${PROJ_ROOT}/components/web_proto web_proto_build)
target_compile_options(web_proto PRIVATE -fno-exceptions -fno-rtti)

enable_testing()

foreach(name test_web_proto_request test_web_proto_writer test_web_proto_error)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} web_proto unity)
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
