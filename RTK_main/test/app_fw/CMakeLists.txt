# app_fw 纯 C++20 组件 host 单元测试工程（ADR-0007，J4 交付）。
# 覆盖 Result、LockGuard、Slot、StoreKey、FanoutRing、trace/TraceRing、tlog/LogRing、Log（分级日志）；
# 另建 app_fw_off（FW_TRACE_ENABLED=0）验证追踪关闭时 tlog 编译路径；支持普通构建与 ASan/UBSan 构建。
cmake_minimum_required(VERSION 3.22)
project(app_fw_tests C CXX)

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
target_compile_options(app_fw PRIVATE -fno-exceptions -fno-rtti)

enable_testing()

find_package(Threads REQUIRED)

include(${PROJ_ROOT}/cmake/pascal_case.cmake)
foreach(name test_app_fw_basics test_fanout_ring test_fanout_typed test_trace test_tlog test_log)
    rtk_pascal_case(_f "${name}")
    add_executable(${name} ${_f}.cpp)
    target_link_libraries(${name} app_fw unity)
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
target_link_libraries(test_fanout_typed Threads::Threads)
set_tests_properties(test_fanout_typed PROPERTIES TIMEOUT 30)

# FW_TRACE_ENABLED=0 编译路径（ADR-0009 §2.6）：同一批源文件按“追踪关闭”再编一份库，
# 确认 tlog/tlog_hex 退化为空内联后 app_fw 仍能编译，且 tlog 不再写环。
add_library(app_fw_off STATIC
    ${PROJ_ROOT}/components/app_fw/src/Status.cpp
    ${PROJ_ROOT}/components/app_fw/src/Result.cpp
    ${PROJ_ROOT}/components/app_fw/src/FanoutRing.cpp
    ${PROJ_ROOT}/components/app_fw/src/FanoutRingRead.cpp
    ${PROJ_ROOT}/components/app_fw/src/Trace.cpp
    ${PROJ_ROOT}/components/app_fw/src/Tlog.cpp
    ${PROJ_ROOT}/components/app_fw/src/Log.cpp)
target_include_directories(app_fw_off PUBLIC ${PROJ_ROOT}/components/app_fw/include)
target_compile_features(app_fw_off PUBLIC cxx_std_20)
target_compile_definitions(app_fw_off PUBLIC FW_TRACE_ENABLED=0 FW_LOG_MAX_LEVEL=2)
target_compile_options(app_fw_off PRIVATE -fno-exceptions -fno-rtti ${PROJ_CXX_STRICT})

add_executable(test_tlog_off TestTlogOff.cpp)
target_link_libraries(test_tlog_off app_fw_off unity)
target_compile_options(test_tlog_off PRIVATE ${PROJ_CXX_STRICT})
add_test(NAME test_tlog_off COMMAND test_tlog_off)

add_executable(test_log_off TestLogOff.cpp)
target_link_libraries(test_log_off app_fw_off unity)
target_compile_options(test_log_off PRIVATE ${PROJ_CXX_STRICT})
add_test(NAME test_log_off COMMAND test_log_off)
