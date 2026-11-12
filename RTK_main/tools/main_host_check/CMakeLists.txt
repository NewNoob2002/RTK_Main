# Generated-by: Codex / GPT-6.1-sol; pi + codex/gpt-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
cmake_minimum_required(VERSION 3.22)
project(main_host_check C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(RTK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_library(main_rtos STATIC ../t13_host_check/shim/fake_rtos.c)
target_include_directories(main_rtos PUBLIC ../t13_host_check/shim/inc)
target_link_libraries(main_rtos PUBLIC Threads::Threads)
add_subdirectory(${RTK_ROOT}/components/stream_demux stream_demux_build)
add_library(main_pure STATIC
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_frame.c
    ${RTK_ROOT}/components/rtk_proto/rtk_proto_alarm.c
    ${RTK_ROOT}/components/rtk_proto/frame.cpp
    ${RTK_ROOT}/components/rtk_proto/demux_frame.cpp
    ${RTK_ROOT}/components/rtk_proto/alarm_codec.cpp
    ${RTK_ROOT}/components/rtk_proto/bytes.cpp
    ${RTK_ROOT}/components/rtk_proto/status.cpp
    ${RTK_ROOT}/components/rtk_core/alarm_source.cpp
    ${RTK_ROOT}/components/rtk_core/link_manager.cpp
    ${RTK_ROOT}/components/rtk_core/command_router.cpp
    ${RTK_ROOT}/components/rtk_core/services/device_service.cpp
    ${RTK_ROOT}/components/rtk_core/services/config_service.cpp
    ${RTK_ROOT}/components/rtk_core/services/alarm_service.cpp
    ${RTK_ROOT}/components/rtk_core/services/pair_service.cpp
    ${RTK_ROOT}/components/rtk_core/link_service.cpp)
target_include_directories(main_pure PUBLIC
    ${RTK_ROOT}/components/rtk_proto/include ${RTK_ROOT}/components/rtk_core/include)
target_include_directories(main_pure PRIVATE ${RTK_ROOT}/components/rtk_core/private_include)
target_link_libraries(main_pure PUBLIC stream_demux)
target_compile_options(main_pure PRIVATE -Wall -Wextra -Werror -Wshadow)
enable_testing()
# 同一真实组合根分别编译两种 Kconfig 分支，产品不增加测试开关或可替换调度框架。
foreach(policy concurrent preempt)
    add_executable(main_${policy} main_host_check.cpp ${RTK_ROOT}/main/modules/rtcm/rtcm_forwarder.cpp
        ${RTK_ROOT}/main/data/app_data.cpp ${RTK_ROOT}/main/modules/storage/storage_module.cpp
        ${RTK_ROOT}/main/modules/rtcm/rtcm_module.cpp ${RTK_ROOT}/main/modules/bt_link/async_tx.cpp
        ${RTK_ROOT}/components/rtk_bt/spp_transport.cpp)
    target_include_directories(main_${policy} BEFORE PRIVATE shim)
    target_include_directories(main_${policy} PRIVATE
        ${RTK_ROOT}/main ${RTK_ROOT}/components/rtk_bt/include
        ${RTK_ROOT}/components/rtk_console/include
        ${RTK_ROOT}/components/rtk_store/include ${RTK_ROOT}/components/rtk_os/include
        ${RTK_ROOT}/components/app_fw/include)
    target_compile_options(main_${policy} PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
    target_link_libraries(main_${policy} PRIVATE main_pure main_rtos)
    if(policy STREQUAL "preempt")
        target_compile_definitions(main_${policy} PRIVATE CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT=1)
    endif()
    foreach(scenario policy_failure async_tx save_recovery storage_backoff rx_fairness rx_fast_partial rx_tick_budget lifecycle_application
            rtcm_mix rtcm_roles rtcm_replay_budget rtcm_reset_lifecycle rtcm_byte_budget)
        add_test(NAME main_${policy}_${scenario} COMMAND main_${policy} ${scenario})
    endforeach()
endforeach()

add_executable(executor_check executor_check.cpp ${RTK_ROOT}/components/rtk_os/executor.cpp)
target_include_directories(executor_check BEFORE PRIVATE shim)
target_include_directories(executor_check PRIVATE ../t13_host_check/shim/inc
    ${RTK_ROOT}/components/rtk_os/include ${RTK_ROOT}/components/rtk_proto/include
    ${RTK_ROOT}/components/app_fw/include)
target_compile_options(executor_check PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
foreach(scenario lifecycle calls invalid)
    add_test(NAME executor_${scenario} COMMAND executor_check ${scenario})
endforeach()
