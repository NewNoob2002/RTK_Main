# GNSS 纯逻辑组件（ADR-0012 §2.2）：NMEA 解析、驱动接口（Receiver 基类）、探测/配置状态机。
# C++20、无 IDF 依赖，同一份 CMakeLists 既是 ESP-IDF 组件，也可被 host 测试 add_subdirectory()。
# 驱动按开关选编：IDF 下 CONFIG_RTK_GNSS_DRV_<D>，host 下 option RTK_GNSS_DRV_<D>；
# 启用的驱动以同名宏（=1）传给 src/Registry.cpp 生成 enabledReceivers()。
set(_gnss_drivers UNICORE UBLOX COMNAV)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/pascal_case.cmake)
set(_gnss_srcs src/NmeaParser.cpp src/NmeaParserNumeric.cpp src/NmeaParserPosition.cpp
               src/NmeaParserSatellites.cpp src/Controller.cpp src/ControllerProbe.cpp src/ControllerConfigure.cpp src/ControllerMaintenance.cpp
               src/Registry.cpp src/Receiver.cpp src/PppSequence.cpp src/GgaQualityGate.cpp
               src/UbloxExt.cpp)

if(ESP_PLATFORM)
    set(_srcs ${_gnss_srcs})
    set(_defs)
    foreach(_d IN LISTS _gnss_drivers)
        if(CONFIG_RTK_GNSS_DRV_${_d})
            string(TOLOWER "${_d}" _l)
            rtk_pascal_case(_f "${_l}")
            list(APPEND _srcs "src/drivers/${_f}Receiver.cpp")
            if(_d STREQUAL "UNICORE")
                # Unicore 驱动按职责分三文件：型号/能力、应答解码、命令脚本（ADR-0013 §2.4）。
                list(APPEND _srcs src/drivers/UnicoreResp.cpp src/drivers/UnicoreScript.cpp)
            elseif(_d STREQUAL "UBLOX")
                # u-blox 驱动分两文件：UBX 帧层/配置编码与驱动行为。
                list(APPEND _srcs src/drivers/UbloxUbx.cpp)
            endif()
            list(APPEND _defs "RTK_GNSS_DRV_${_d}=1")
        endif()
    endforeach()
    idf_component_register(SRCS ${_srcs} INCLUDE_DIRS "include" REQUIRES stream_demux app_fw)
    set(_gnss_target ${COMPONENT_LIB})
    target_compile_definitions(${_gnss_target} PRIVATE ${_defs})
else()
    cmake_minimum_required(VERSION 3.16)
    add_library(gnss STATIC ${_gnss_srcs})
    foreach(_d IN LISTS _gnss_drivers)
        if(_d STREQUAL "COMNAV")
            set(_default OFF)
        else()
            set(_default ON)
        endif()
        option(RTK_GNSS_DRV_${_d} "编译 ${_d} 驱动" ${_default})
        if(RTK_GNSS_DRV_${_d})
            string(TOLOWER "${_d}" _l)
            rtk_pascal_case(_f "${_l}")
            target_sources(gnss PRIVATE src/drivers/${_f}Receiver.cpp)
            if(_d STREQUAL "UNICORE")
                target_sources(gnss PRIVATE src/drivers/UnicoreResp.cpp src/drivers/UnicoreScript.cpp)
            elseif(_d STREQUAL "UBLOX")
                target_sources(gnss PRIVATE src/drivers/UbloxUbx.cpp)
            endif()
            target_compile_definitions(gnss PRIVATE RTK_GNSS_DRV_${_d}=1)
        endif()
    endforeach()
    target_include_directories(gnss PUBLIC include)
    target_link_libraries(gnss PUBLIC stream_demux app_fw)
    target_compile_features(gnss PUBLIC cxx_std_20)
    set(_gnss_target gnss)
endif()

target_include_directories(${_gnss_target} PRIVATE private_include)

# 纯组件告警集（AGENTS.md §3），与 stream_demux 相同。
target_compile_options(${_gnss_target} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
    -Woverloaded-virtual -Wimplicit-fallthrough -Wswitch-default -Wundef -Wcast-align -Wnull-dereference)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${_gnss_target})
