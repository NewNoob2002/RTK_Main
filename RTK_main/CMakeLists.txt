cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 固件版本单一来源（ADR-0019 §2.6）：同一字符串写入镜像描述符（PROJECT_VER，OTA 比较与显示用）
# 和全局 C 宏 PROJECT_VERSION。CMake 变量不叫 PROJECT_VERSION：project() 会清空它，
# 且 IDF 自带的 unity/lwip 等子工程读取该变量。
set(RTK_FW_VERSION "V3.1.0")
set(PROJECT_VER "${RTK_FW_VERSION}")
idf_build_set_property(COMPILE_DEFINITIONS "PROJECT_VERSION=\"${RTK_FW_VERSION}\"" APPEND)
project(rtk_main)
