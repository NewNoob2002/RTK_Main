# H2 自测工程（不交付、不参与构建）。只编译 H2 交付的文件 + 它们依赖的 rtk_proto 源文件，
# 不依赖 H1 的 src/demux.cpp，因此可以与 H1/H3 并行运行。
cmake_minimum_required(VERSION 3.16)
project(h2_selftest C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RTK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(DEMUX_INC ${RTK_ROOT}/components/stream_demux/include)
set(PROTO_INC ${RTK_ROOT}/components/rtk_proto/include)

add_executable(h2_check
    h2_check.cpp
    ${RTK_ROOT}/components/stream_demux/src/crc.cpp
    ${RTK_ROOT}/components/stream_demux/src/decoders/rtcm3.cpp
    ${RTK_ROOT}/components/stream_demux/src/decoders/nmea.cpp
    ${RTK_ROOT}/components/stream_demux/src/decoders/ubx.cpp
    ${RTK_ROOT}/components/stream_demux/src/decoders/unicore_bin.cpp
    ${RTK_ROOT}/components/stream_demux/src/decoders/unicore_hash.cpp
    ${RTK_ROOT}/components/rtk_proto/demux_frame.cpp
    ${RTK_ROOT}/components/rtk_proto/frame.cpp
    ${RTK_ROOT}/components/rtk_proto/status.cpp
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_frame.c)

target_include_directories(h2_check PRIVATE ${DEMUX_INC} ${PROTO_INC})
target_compile_options(h2_check PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion
        -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
        -Wnull-dereference -Wimplicit-fallthrough -Wundef -Wswitch-default>
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
        -Wcast-align -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes
        -Wmissing-prototypes>)
