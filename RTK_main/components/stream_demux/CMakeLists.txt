# 通用多协议字节流分流器（ADR-0005）。C++20 子集，平台无关：同一份 CMakeLists 既是 ESP-IDF 组件，
# 也可被任意 CMake 工程 add_subdirectory() 复用（arm-none-eabi、host 测试等）。
# 每个解码器一个源文件，按开关选择编译；未用到的解码器即使编译了也会被 --gc-sections 去除。
set(_demux_decoders rtcm3 nmea ubx unicore_bin unicore_hash)

if(ESP_PLATFORM)
    set(_srcs "src/demux.cpp" "src/crc.cpp")
    foreach(_d IN LISTS _demux_decoders)
        string(TOUPPER "${_d}" _D)
        if(CONFIG_STREAM_DEMUX_${_D})
            list(APPEND _srcs "src/decoders/${_d}.cpp")
        endif()
    endforeach()
    idf_component_register(SRCS ${_srcs} INCLUDE_DIRS "include")
    set(_demux_target ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(stream_demux STATIC src/demux.cpp src/crc.cpp)
    foreach(_d IN LISTS _demux_decoders)
        string(TOUPPER "${_d}" _D)
        option(STREAM_DEMUX_${_D} "编译 ${_d} 解码器" ON)
        if(STREAM_DEMUX_${_D})
            target_sources(stream_demux PRIVATE src/decoders/${_d}.cpp)
        endif()
    endforeach()
    target_include_directories(stream_demux PUBLIC include)
    target_compile_features(stream_demux PUBLIC cxx_std_20)
    set(_demux_target stream_demux)
endif()

# 纯组件告警集（AGENTS.md §3）+ 可移植性相关项；无 IDF 头，因此可全开。
target_compile_options(${_demux_target} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef -Wcast-align -Wnull-dereference)
