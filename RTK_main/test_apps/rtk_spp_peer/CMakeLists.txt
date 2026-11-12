# HIL SPP 对端工程（T14）：独立 ESP-IDF 工程，供主机侧 BlueZ RFCOMM 连接；不参与 RTK_main 固件构建。
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/stream_demux"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rtk_proto"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bt_reconnect")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rtk_spp_peer)
