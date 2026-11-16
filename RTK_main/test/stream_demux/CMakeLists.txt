# 通用多协议字节流分流器 C++20 黑盒测试工程（ADR-0005，H3 交付）。
# 独立 host CMake 工程，不修改冻结的 test/host/ 与 components/stream_demux/CMakeLists.txt。
cmake_minimum_required(VERSION 3.22)
project(stream_demux_tests C CXX)

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

# 纯 C++ 组件的严格告警组（参照 test/host/CMakeLists.txt），对被测产品帧适配与测试同样生效。
set(PROJ_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

# C 源文件严格告警（DevProtoFrame.c）。
set(PROJ_C_STRICT
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align
    -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes
    -Wfloat-equal -Werror=implicit-function-declaration)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

# 独立复用组件 stream_demux（在独立 CMake 下自动定义 STREAM_DEMUX_<协议> 开关）。
add_subdirectory(${PROJ_ROOT}/components/stream_demux stream_demux_build)
# 与目标固件一致的运行时约束（CPP-001/002）：与测试侧保持同为 -fno-exceptions -fno-rtti，
# 避免被测 Sink 基类与测试子类 RTTI 开关不一致导致 UBSan vptr 误报。
target_compile_options(stream_demux PRIVATE -fno-exceptions -fno-rtti)

# 产品帧解码器直接编译 components/device_proto/DemuxFrame.cpp 及其依赖的 device_proto 源文件。
add_library(dev_proto_c_demux STATIC
    ${PROJ_ROOT}/components/device_proto/DevProtoFrame.c)
target_include_directories(dev_proto_c_demux PUBLIC ${PROJ_ROOT}/components/device_proto/include)
target_compile_options(dev_proto_c_demux PRIVATE ${PROJ_C_STRICT})

add_library(dev_proto_demux STATIC
    ${PROJ_ROOT}/components/app_fw/src/Status.cpp
    ${PROJ_ROOT}/components/device_proto/Bytes.cpp
    ${PROJ_ROOT}/components/device_proto/Frame.cpp
    ${PROJ_ROOT}/components/device_proto/DemuxFrame.cpp)
target_include_directories(dev_proto_demux PUBLIC
    ${PROJ_ROOT}/components/app_fw/include ${PROJ_ROOT}/components/device_proto/include)
target_link_libraries(dev_proto_demux PUBLIC dev_proto_c_demux stream_demux)
target_compile_options(dev_proto_demux PRIVATE ${PROJ_CXX_STRICT})

# 将 CMake 开关 STREAM_DEMUX_<协议> 显式转换为 0/1 编译定义，供测试按开关裁剪且不触发 -Wundef。
set(_demux_test_defs)
foreach(_d IN ITEMS rtcm3 nmea ubx unicore_bin unicore_hash)
    string(TOUPPER "${_d}" _D)
    if(STREAM_DEMUX_${_D})
        list(APPEND _demux_test_defs "STREAM_DEMUX_${_D}=1")
    else()
        list(APPEND _demux_test_defs "STREAM_DEMUX_${_D}=0")
    endif()
endforeach()

add_library(demux_gen STATIC DemuxGen.cpp)
target_include_directories(demux_gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(demux_gen PUBLIC dev_proto_demux)
target_compile_options(demux_gen PRIVATE ${PROJ_CXX_STRICT})

enable_testing()

include(${PROJ_ROOT}/cmake/pascal_case.cmake)
foreach(name test_demux_gen test_demux_engine test_demux_decoders test_demux_stream)
    rtk_pascal_case(_f "${name}")
    add_executable(${name} ${_f}.cpp)
    target_link_libraries(${name} stream_demux dev_proto_demux demux_gen unity)
    target_compile_definitions(${name} PRIVATE ${_demux_test_defs})
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
