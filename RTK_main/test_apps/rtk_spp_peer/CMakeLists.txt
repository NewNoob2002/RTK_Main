# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/stream_demux"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/rtk_proto"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bt_reconnect")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rtk_spp_peer)
