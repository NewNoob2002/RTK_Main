# main_host_check 主机回归工程：用替身编译真实 main 模块与 link_core/rtos，不参与固件构建。
cmake_minimum_required(VERSION 3.22)
project(main_host_check C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_library(main_rtos STATIC ../t13_host_check/shim/fake_rtos.c)
# nvs_shim.c: storage 模块的通用键表直接用 nvs.h（冻结的 nvs_store.h 无整数 API）。
# 单独成库：它是 C 代码，不能继承 C++ 专用告警（-fno-rtti/-Wnon-virtual-dtor）。
add_library(main_nvs_shim STATIC shim/nvs_shim.c)
target_include_directories(main_nvs_shim PUBLIC shim)
target_include_directories(main_rtos PUBLIC ../t13_host_check/shim/inc)
target_link_libraries(main_rtos PUBLIC Threads::Threads)
add_subdirectory(${PROJ_ROOT}/components/stream_demux stream_demux_build)
add_library(main_pure STATIC
    ${PROJ_ROOT}/components/device_proto/dev_proto_frame.c
    ${PROJ_ROOT}/components/device_proto/dev_proto_alarm.c
    ${PROJ_ROOT}/components/device_proto/frame.cpp
    ${PROJ_ROOT}/components/device_proto/demux_frame.cpp
    ${PROJ_ROOT}/components/device_proto/alarm_codec.cpp
    ${PROJ_ROOT}/components/device_proto/bytes.cpp
    ${PROJ_ROOT}/components/app_fw/src/status.cpp
    ${PROJ_ROOT}/components/app_fw/src/result.cpp
    ${PROJ_ROOT}/components/app_fw/src/trace.cpp
    ${PROJ_ROOT}/components/app_fw/src/tlog.cpp
    ${PROJ_ROOT}/components/cli/src/out.cpp
    ${PROJ_ROOT}/components/link_core/alarm_source.cpp
    ${PROJ_ROOT}/components/link_core/link_manager.cpp
    ${PROJ_ROOT}/components/link_core/command_router.cpp
    ${PROJ_ROOT}/components/link_core/services/device_service.cpp
    ${PROJ_ROOT}/components/link_core/services/config_service.cpp
    ${PROJ_ROOT}/components/link_core/services/alarm_service.cpp
    ${PROJ_ROOT}/components/link_core/services/pair_service.cpp
    ${PROJ_ROOT}/components/link_core/link_service.cpp)
target_include_directories(main_pure PUBLIC
    ${PROJ_ROOT}/components/app_fw/include
    ${PROJ_ROOT}/components/device_proto/include ${PROJ_ROOT}/components/link_core/include)
target_include_directories(main_pure PRIVATE ${PROJ_ROOT}/components/link_core/private_include
    ${PROJ_ROOT}/components/cli/include)
target_link_libraries(main_pure PUBLIC stream_demux)
target_compile_options(main_pure PRIVATE -Wall -Wextra -Werror -Wshadow)
enable_testing()
# 同一真实组合根分别编译两种 Kconfig 分支，产品不增加测试开关或可替换调度框架。
foreach(policy concurrent preempt)
    add_executable(main_${policy} main_host_check.cpp ${PROJ_ROOT}/main/modules/rtcm/rtcm_forwarder.cpp
        ${PROJ_ROOT}/main/data/app_data.cpp ${PROJ_ROOT}/main/modules/storage/storage_module.cpp ${PROJ_ROOT}/main/modules/storage/store_keys.cpp ${PROJ_ROOT}/main/modules/storage/store_catalog.cpp
        ${PROJ_ROOT}/main/modules/rtcm/rtcm_module.cpp
        # storage 的通用键表引用 DebugModule 的绑定函数，因此链接真实的 debug 模块。
        ${PROJ_ROOT}/main/modules/debug/debug_module.cpp ${PROJ_ROOT}/main/modules/debug/trace_text.cpp
        ${PROJ_ROOT}/components/spp_server/spp_transport.cpp)
    target_include_directories(main_${policy} BEFORE PRIVATE shim)
    target_include_directories(main_${policy} PRIVATE
        ${PROJ_ROOT}/main ${PROJ_ROOT}/components/spp_server/include
        ${PROJ_ROOT}/components/cli/include
        ${PROJ_ROOT}/components/nvs_store/include ${PROJ_ROOT}/components/rtos/include
        ${PROJ_ROOT}/components/app_fw/include)
    target_compile_options(main_${policy} PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
    # host 检查不跑 Kconfig：给出与 main/Kconfig.projbuild 默认值一致的文本日志环容量（ADR-0009 §2.11）。
    target_compile_definitions(main_${policy} PRIVATE CONFIG_RTK_LOG_RING_BYTES=4096)
    target_link_libraries(main_${policy} PRIVATE main_pure main_rtos main_nvs_shim)
    if(policy STREQUAL "preempt")
        target_compile_definitions(main_${policy} PRIVATE CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT=1)
    endif()
    foreach(scenario policy_failure debug_cat_names credit_window save_recovery storage_backoff rx_fairness rx_fast_partial rx_tick_budget lifecycle_application
            rtcm_mix rtcm_roles rtcm_replay_budget rtcm_reset_lifecycle rtcm_byte_budget)
        add_test(NAME main_${policy}_${scenario} COMMAND main_${policy} ${scenario})
    endforeach()
endforeach()

# executor.cpp 通过 fw::trace 报告轮次超时/投递失败，因此要链 trace.cpp。
add_executable(executor_check executor_check.cpp ${PROJ_ROOT}/components/rtos/executor.cpp
    ${PROJ_ROOT}/components/app_fw/src/trace.cpp)
target_include_directories(executor_check BEFORE PRIVATE shim)
target_include_directories(executor_check PRIVATE ../t13_host_check/shim/inc
    ${PROJ_ROOT}/components/rtos/include ${PROJ_ROOT}/components/device_proto/include
    ${PROJ_ROOT}/components/app_fw/include)
target_compile_options(executor_check PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
foreach(scenario lifecycle calls invalid)
    add_test(NAME executor_${scenario} COMMAND executor_check ${scenario})
endforeach()

add_executable(template_check template_check.cpp
    ${PROJ_ROOT}/main/modules/_template/template_module.cpp
    ${PROJ_ROOT}/main/modules/_template/template_cmds.cpp
    ${PROJ_ROOT}/main/data/app_data.cpp
    ${PROJ_ROOT}/main/support/console_support.cpp
    ${PROJ_ROOT}/components/cli/src/parse.cpp
    # debug 命令组用 cli::fail/dispatch（ADR-0009 §2.5）。
    ${PROJ_ROOT}/components/cli/src/reply.cpp
    ${PROJ_ROOT}/components/rtos/executor.cpp
    ${PROJ_ROOT}/main/modules/bt_link/bt_link_module.cpp
    ${PROJ_ROOT}/main/modules/rtcm/rtcm_module.cpp
    ${PROJ_ROOT}/main/modules/rtcm/rtcm_forwarder.cpp
    ${PROJ_ROOT}/main/modules/storage/storage_module.cpp ${PROJ_ROOT}/main/modules/storage/store_keys.cpp ${PROJ_ROOT}/main/modules/storage/store_catalog.cpp
    ${PROJ_ROOT}/main/modules/debug/debug_module.cpp ${PROJ_ROOT}/main/modules/debug/trace_text.cpp
    ${PROJ_ROOT}/main/modules/debug/debug_cmds.cpp
    ${PROJ_ROOT}/components/spp_server/spp_transport.cpp)
target_include_directories(template_check BEFORE PRIVATE shim)
target_include_directories(template_check PRIVATE ../t13_host_check/shim/inc
    ${PROJ_ROOT}/main ${PROJ_ROOT}/components/spp_server/include
    ${PROJ_ROOT}/components/cli/include
    ${PROJ_ROOT}/components/nvs_store/include ${PROJ_ROOT}/components/rtos/include
    ${PROJ_ROOT}/components/app_fw/include)
target_compile_options(template_check PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
target_compile_definitions(template_check PRIVATE CONFIG_RTK_LOG_RING_BYTES=4096)
target_link_libraries(template_check PRIVATE main_pure main_nvs_shim)
add_test(NAME template_module COMMAND template_check template_module)
