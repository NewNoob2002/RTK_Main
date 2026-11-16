# 应用框架（ADR-0007）：模块契约、Slot、LockOps、StoreKey、FanoutRing。纯 C++20，平台无关，
# 同一份 CMakeLists 既是 ESP-IDF 组件，也可被 host 测试 add_subdirectory() 复用（同 stream_demux 模式）。
# J4 在 _app_fw_srcs 中追加 src/FanoutRing.cpp；J7-b 追加 src/Trace.cpp；T4 追加 src/Tlog.cpp。
set(_app_fw_srcs "src/Status.cpp" "src/Result.cpp" "src/FanoutRing.cpp" "src/Trace.cpp" "src/Tlog.cpp")

if(ESP_PLATFORM)
    idf_component_register(SRCS ${_app_fw_srcs} INCLUDE_DIRS "include")
    set(_app_fw_target ${COMPONENT_LIB})
    # ADR-0009 §2.6：追踪总开关（Kconfig 在 main/Kconfig.projbuild）。PUBLIC 传给所有 REQUIRES app_fw 的组件，
    # 使 fw/Trace.hpp 中 trace()/trace_on() 在关闭时变为空内联。未定义（早期展开阶段）按开启处理。
    if(DEFINED CONFIG_RTK_TRACE_ENABLE AND NOT CONFIG_RTK_TRACE_ENABLE)
        target_compile_definitions(${COMPONENT_LIB} PUBLIC FW_TRACE_ENABLED=0)
    endif()
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

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${_app_fw_target})
