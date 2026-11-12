# 通用 SPP 客户端重连策略状态机（ADR-0007 §5）。纯 C++20，平台无关，
# 同一份 CMakeLists 既是 ESP-IDF 组件，也可被 host 测试 add_subdirectory() 复用（同 app_fw / stream_demux 模式）。
set(_bt_reconnect_srcs "src/reconnect.cpp")

if(ESP_PLATFORM)
    idf_component_register(SRCS ${_bt_reconnect_srcs} INCLUDE_DIRS "include")
    set(_bt_reconnect_target ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(bt_reconnect STATIC ${_bt_reconnect_srcs})
    target_include_directories(bt_reconnect PUBLIC include)
    target_compile_features(bt_reconnect PUBLIC cxx_std_20)
    set(_bt_reconnect_target bt_reconnect)
endif()

# 纯组件告警集（AGENTS.md §3）
target_compile_options(${_bt_reconnect_target} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef -Wcast-align -Wnull-dereference)
