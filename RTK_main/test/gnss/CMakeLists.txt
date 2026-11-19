# GNSS 纯逻辑组件 host 测试工程（ADR-0010/ADR-0012）。
# 独立 CMake 工程，不修改冻结的 test/host/ 与 components/gnss/CMakeLists.txt。
# 驱动开关与组件同名（RTK_GNSS_DRV_<D>），并在测试侧转成 0/1 宏，便于按开关裁剪用例。
cmake_minimum_required(VERSION 3.22)
project(gnss_tests C CXX)

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

option(RTK_GNSS_DRV_UNICORE "编译 Unicore 驱动" ON)
option(RTK_GNSS_DRV_UBLOX "编译 u-blox 驱动" ON)
option(RTK_GNSS_DRV_COMNAV "编译 ComNav 驱动" OFF)
option(SANITIZE "启用 ASan/UBSan" OFF)

set(PROJ_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_subdirectory(${PROJ_ROOT}/components/stream_demux stream_demux_build)
add_subdirectory(${PROJ_ROOT}/components/app_fw app_fw_build)
add_subdirectory(${PROJ_ROOT}/components/gnss gnss_build)
target_compile_options(gnss PRIVATE -fno-exceptions -fno-rtti)

# 把 CMake 开关显式转成 0/1 宏，测试与组件看到同一份编译配置（-Wundef 下安全）。
set(_gnss_test_defs)
foreach(_d IN ITEMS UNICORE UBLOX COMNAV)
    if(RTK_GNSS_DRV_${_d})
        list(APPEND _gnss_test_defs "RTK_GNSS_DRV_${_d}=1")
    else()
        list(APPEND _gnss_test_defs "RTK_GNSS_DRV_${_d}=0")
    endif()
endforeach()

enable_testing()
# 驱动用例随开关裁剪：关闭的驱动不编进 gnss，测试也不能引用其 x_receiver() 符号。
set(_gnss_tests test_gnss_nmea test_gnss_controller test_gnss_registry test_gnss_null_receiver
    test_gnss_classify)
if(RTK_GNSS_DRV_UNICORE)
    list(APPEND _gnss_tests test_gnss_unicore_receiver)
    # $CONFIG 读回与控制器读回窗口（ADR-0012 §2.7）；u-blox 用例内部按开关裁剪。
    list(APPEND _gnss_tests test_gnss_readback)
endif()
if(RTK_GNSS_DRV_UBLOX)
    list(APPEND _gnss_tests test_gnss_ublox_receiver)
endif()
if(RTK_GNSS_DRV_COMNAV)
    list(APPEND _gnss_tests test_gnss_comnav_receiver)
endif()
include(${PROJ_ROOT}/cmake/pascal_case.cmake)
# 黄金向量生成器（ADR-0012 §2.4）：在基线 4cf1320 的 ADR-0010 Dialect 接口上运行一次，导出
# golden/Golden<Family>.inc。ADR-0012 删除 Dialect 后本目标不再构建，保留产物为唯一事实来源。
if(EXISTS ${PROJ_ROOT}/components/gnss/include/gnss/Dialect.hpp)
    add_executable(golden_gen golden/GoldenGen.cpp)
    target_link_libraries(golden_gen gnss)
    target_include_directories(golden_gen PRIVATE golden)
    target_compile_definitions(golden_gen PRIVATE ${_gnss_test_defs})
    target_compile_options(golden_gen PRIVATE ${PROJ_CXX_STRICT})
endif()

foreach(name IN LISTS _gnss_tests)
    rtk_pascal_case(_f "${name}")
    add_executable(${name} ${_f}.cpp)
    target_link_libraries(${name} gnss unity)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/golden)
    target_compile_definitions(${name} PRIVATE ${_gnss_test_defs})
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

if(RTK_GNSS_DRV_UNICORE)
    target_include_directories(test_gnss_unicore_receiver PRIVATE ${PROJ_ROOT}/components/gnss/private_include)
endif()
