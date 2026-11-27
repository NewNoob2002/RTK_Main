# 4G 蜂窝链路纯逻辑组件（ADR-0021）：AT 引擎 + 链路 FSM，无 IDF 依赖，主机可编译、可单测。
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/AtEngine.cpp" "src/AtParse.cpp" "src/LinkFsm.cpp"
        INCLUDE_DIRS "include"
    )
    set(_t ${COMPONENT_LIB})
else()
    cmake_minimum_required(VERSION 3.16)
    project(cellular LANGUAGES CXX)
    add_library(cellular STATIC src/AtEngine.cpp src/AtParse.cpp src/LinkFsm.cpp)
    target_include_directories(cellular PUBLIC include)
    target_compile_features(cellular PUBLIC cxx_std_20)
    set(_t cellular)
endif()

target_compile_options(${_t} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default
    -Wundef -Wcast-align -Wnull-dereference)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${_t})
