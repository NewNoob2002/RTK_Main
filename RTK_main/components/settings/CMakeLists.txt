# 统一设置服务与键注册表（ADR-0018 §2.3）：纯 C++20，无 IDF/FreeRTOS 依赖，支持主机测试。
set(_settings_srcs "src/Registry.cpp" "src/Service.cpp")

if(ESP_PLATFORM)
    idf_component_register(SRCS ${_settings_srcs} INCLUDE_DIRS "include" REQUIRES app_fw)
    set(_settings_target ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(settings STATIC ${_settings_srcs})
    target_include_directories(settings PUBLIC include)
    target_link_libraries(settings PUBLIC app_fw)
    target_compile_features(settings PUBLIC cxx_std_20)
    set(_settings_target settings)
endif()

# 纯组件告警集（AGENTS.md §3），与 web_proto/app_fw 相同。
target_compile_options(${_settings_target} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef -Wcast-align -Wnull-dereference)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${_settings_target})
