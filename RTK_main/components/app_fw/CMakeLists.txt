# 应用框架（ADR-0007）：模块契约、Slot、LockOps、StoreKey、FanoutRing。纯 C++20，平台无关，
# 同一份 CMakeLists 既是 ESP-IDF 组件，也可被 host 测试 add_subdirectory() 复用（同 stream_demux 模式）。
# J4 在 _app_fw_srcs 中追加 src/fanout_ring.cpp；J7-b 追加 src/trace.cpp。
set(_app_fw_srcs "src/status.cpp" "src/result.cpp" "src/fanout_ring.cpp" "src/trace.cpp")

if(ESP_PLATFORM)
    idf_component_register(SRCS ${_app_fw_srcs} INCLUDE_DIRS "include")
    set(_app_fw_target ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(app_fw STATIC ${_app_fw_srcs})
    target_include_directories(app_fw PUBLIC include)
    target_compile_features(app_fw PUBLIC cxx_std_20)
    set(_app_fw_target app_fw)
endif()

# 纯组件告警集（AGENTS.md §3）
target_compile_options(${_app_fw_target} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef -Wcast-align -Wnull-dereference)
