# 网页配置 WebSocket 协议解析/输出/键表（ADR-0017 §2.5，docs/web/ws-protocol.md）。
# 纯 C++20、无 IDF/FreeRTOS 头、不分配堆：同一份 CMakeLists 既是 ESP-IDF 组件，
# 也可被 host 测试 add_subdirectory() 复用（同 stream_demux/app_fw 模式）。
set(_web_proto_srcs "src/Error.cpp" "src/Request.cpp" "src/Writer.cpp")

if(ESP_PLATFORM)
    idf_component_register(SRCS ${_web_proto_srcs} INCLUDE_DIRS "include" REQUIRES app_fw)
    set(_web_proto_target ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(web_proto STATIC ${_web_proto_srcs})
    target_include_directories(web_proto PUBLIC include)
    target_link_libraries(web_proto PUBLIC app_fw)
    target_compile_features(web_proto PUBLIC cxx_std_20)
    set(_web_proto_target web_proto)
endif()

# 纯组件告警集（AGENTS.md §3），与 stream_demux/app_fw 相同。
target_compile_options(${_web_proto_target} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef -Wcast-align -Wnull-dereference)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${_web_proto_target})
