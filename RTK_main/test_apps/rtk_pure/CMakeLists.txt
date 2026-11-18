# 引入纯组件 device_proto、link_core 及其依赖 app_fw、stream_demux；IDF 基础组件与 Unity 由依赖闭包提供。
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS ../../components/device_proto ../../components/link_core
    ../../components/app_fw ../../components/stream_demux)
set(COMPONENTS main)
# 两个目标各用独立配置，避免 set-target 覆盖另一架构的配置。
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rtk_pure)
