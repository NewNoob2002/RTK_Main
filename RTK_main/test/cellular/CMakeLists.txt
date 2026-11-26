# 4G 蜂窝链路纯逻辑 host 测试工程（ADR-0021，N1 交付）：AT 引擎分帧/超时 + 链路 FSM 全路径。
cmake_minimum_required(VERSION 3.22)
project(cellular_tests C CXX)

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

add_subdirectory(${PROJ_ROOT}/components/cellular cellular_build)
target_compile_options(cellular PRIVATE -fno-exceptions -fno-rtti)

enable_testing()

include(${PROJ_ROOT}/cmake/pascal_case.cmake)
foreach(name test_at_engine test_link_fsm)
    rtk_pascal_case(_f "${name}")
    add_executable(${name} ${_f}.cpp)
    target_link_libraries(${name} cellular unity)
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
