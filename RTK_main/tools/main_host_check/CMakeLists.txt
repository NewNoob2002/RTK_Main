# main_host_check 主机回归工程：用替身编译真实 main 模块与 link_core/rtos，不参与固件构建。
cmake_minimum_required(VERSION 3.22)
project(main_host_check C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_library(main_rtos STATIC ../t13_host_check/shim/FakeRtos.c)
# NvsShim.c: storage 模块的通用键表直接用 nvs.h（冻结的 NvsStore.h 无整数 API）。
# 单独成库：它是 C 代码，不能继承 C++ 专用告警（-fno-rtti/-Wnon-virtual-dtor）。
add_library(main_nvs_shim STATIC shim/NvsShim.c)
target_include_directories(main_nvs_shim PUBLIC shim)
target_include_directories(main_rtos PUBLIC ../t13_host_check/shim/inc)
target_link_libraries(main_rtos PUBLIC Threads::Threads)
add_subdirectory(${PROJ_ROOT}/components/stream_demux stream_demux_build)
add_subdirectory(${PROJ_ROOT}/components/app_fw app_fw_build)
add_subdirectory(${PROJ_ROOT}/components/gnss gnss_build)
add_library(main_pure STATIC
    ${PROJ_ROOT}/components/device_proto/DevProtoFrame.c
    ${PROJ_ROOT}/components/device_proto/DevProtoAlarm.c
    ${PROJ_ROOT}/components/device_proto/Frame.cpp
    ${PROJ_ROOT}/components/device_proto/DemuxFrame.cpp
    ${PROJ_ROOT}/components/device_proto/AlarmCodec.cpp
    ${PROJ_ROOT}/components/device_proto/Bytes.cpp
    ${PROJ_ROOT}/components/app_fw/src/Status.cpp
    ${PROJ_ROOT}/components/app_fw/src/Result.cpp
    ${PROJ_ROOT}/components/app_fw/src/FanoutRing.cpp
    ${PROJ_ROOT}/components/app_fw/src/FanoutRingRead.cpp
    ${PROJ_ROOT}/components/app_fw/src/Trace.cpp
    ${PROJ_ROOT}/components/app_fw/src/Tlog.cpp
    ${PROJ_ROOT}/components/app_fw/src/Log.cpp
    ${PROJ_ROOT}/components/cli/src/Out.cpp
    ${PROJ_ROOT}/components/link_core/AlarmSource.cpp
    ${PROJ_ROOT}/components/link_core/LinkManager.cpp
    ${PROJ_ROOT}/components/link_core/CommandRouter.cpp
    ${PROJ_ROOT}/components/link_core/services/DeviceService.cpp
    ${PROJ_ROOT}/components/link_core/services/ConfigService.cpp
    ${PROJ_ROOT}/components/link_core/services/AlarmService.cpp
    ${PROJ_ROOT}/components/link_core/services/PairService.cpp
    ${PROJ_ROOT}/components/link_core/services/GnssService.cpp
    ${PROJ_ROOT}/components/link_core/LinkService.cpp)
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
    add_executable(main_${policy} MainHostCheck.cpp ${PROJ_ROOT}/main/modules/rtcm/RtcmForwarder.cpp
        ${PROJ_ROOT}/main/data/AppData.cpp ${PROJ_ROOT}/main/modules/storage/StorageModule.cpp ${PROJ_ROOT}/main/modules/storage/StoreKeys.cpp ${PROJ_ROOT}/main/modules/storage/StoreCatalog.cpp
        ${PROJ_ROOT}/main/modules/storage/ConfigCmds.cpp ${PROJ_ROOT}/main/support/ConsoleSupport.cpp
        ${PROJ_ROOT}/components/cli/src/Reply.cpp
        ${PROJ_ROOT}/components/rtos/Executor.cpp
        ${PROJ_ROOT}/main/modules/rtcm/RtcmModule.cpp
        # storage 的通用键表引用 DebugModule 的绑定函数，因此链接真实的 debug 模块。
        ${PROJ_ROOT}/main/modules/debug/DebugModule.cpp ${PROJ_ROOT}/main/modules/debug/DebugModuleLog.cpp ${PROJ_ROOT}/main/modules/debug/TraceText.cpp ${PROJ_ROOT}/main/modules/debug/TraceTextValues.cpp
        ${PROJ_ROOT}/components/spp_server/SppTransport.cpp)
    target_include_directories(main_${policy} BEFORE PRIVATE shim)
    target_include_directories(main_${policy} PRIVATE
        ${PROJ_ROOT}/main ${PROJ_ROOT}/components/spp_server/include
        ${PROJ_ROOT}/components/cli/include
        ${PROJ_ROOT}/components/gnss/include
        ${PROJ_ROOT}/components/nvs_store/include ${PROJ_ROOT}/components/rtos/include
        ${PROJ_ROOT}/components/app_fw/include)
    target_compile_options(main_${policy} PRIVATE -fno-exceptions -fno-rtti
        -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
    # host 检查不跑 Kconfig：给出与 main/Kconfig.projbuild 默认值一致的文本日志环容量（ADR-0009 §2.11）。
    target_compile_definitions(main_${policy} PRIVATE CONFIG_RTK_LOG_RING_BYTES=4096
        CONFIG_RTK_CONSOLE_UART_NUM=0 CONFIG_RTK_CONSOLE_UART_BAUD=2000000)
    target_link_libraries(main_${policy} PRIVATE main_pure main_rtos main_nvs_shim)
    if(policy STREQUAL "preempt")
        target_compile_definitions(main_${policy} PRIVATE CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT=1)
    endif()
    foreach(scenario policy_failure debug_cat_names credit_window save_recovery storage_backoff rx_fairness rx_fast_partial rx_tick_budget rx_wake_level_recheck rx_512_partial_and_pause_isolation rtcm_pipe_direct_and_io_stats lifecycle_application
            rtcm_mix rtcm_roles rtcm_replay_budget rtcm_reset_lifecycle rtcm_byte_budget storage_startup)
        add_test(NAME main_${policy}_${scenario} COMMAND main_${policy} ${scenario})
    endforeach()
endforeach()

# Executor.cpp 通过 fw::trace 报告轮次超时/投递失败、经 FW_LOG* 报启动错误，因此要链 Trace.cpp、Tlog.cpp（文本环）与 Log.cpp。
add_executable(executor_check ExecutorCheck.cpp ${PROJ_ROOT}/components/rtos/Executor.cpp
    ${PROJ_ROOT}/components/app_fw/src/Trace.cpp ${PROJ_ROOT}/components/app_fw/src/Tlog.cpp
    ${PROJ_ROOT}/components/app_fw/src/Log.cpp)
target_include_directories(executor_check BEFORE PRIVATE shim)
target_include_directories(executor_check PRIVATE ../t13_host_check/shim/inc
    ${PROJ_ROOT}/components/rtos/include ${PROJ_ROOT}/components/device_proto/include
    ${PROJ_ROOT}/components/app_fw/include)
target_compile_options(executor_check PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
foreach(scenario lifecycle calls invalid)
    add_test(NAME executor_${scenario} COMMAND executor_check ${scenario})
endforeach()

add_executable(template_check TemplateCheck.cpp
    ${PROJ_ROOT}/main/modules/_template/TemplateModule.cpp
    ${PROJ_ROOT}/main/modules/_template/TemplateCmds.cpp
    ${PROJ_ROOT}/main/data/AppData.cpp
    ${PROJ_ROOT}/main/support/ConsoleSupport.cpp
    ${PROJ_ROOT}/components/cli/src/Parse.cpp
    # debug 命令组用 cli::fail/dispatch（ADR-0009 §2.5）。
    ${PROJ_ROOT}/components/cli/src/Reply.cpp
    ${PROJ_ROOT}/components/rtos/Executor.cpp
    ${PROJ_ROOT}/main/modules/bt_link/BtLinkModule.cpp ${PROJ_ROOT}/main/modules/bt_link/BtLinkSessions.cpp ${PROJ_ROOT}/main/modules/bt_link/BtLinkGnss.cpp
    ${PROJ_ROOT}/main/modules/rtcm/RtcmModule.cpp
    ${PROJ_ROOT}/main/modules/rtcm/RtcmForwarder.cpp
    ${PROJ_ROOT}/main/modules/storage/StorageModule.cpp ${PROJ_ROOT}/main/modules/storage/StoreKeys.cpp ${PROJ_ROOT}/main/modules/storage/StoreCatalog.cpp
    ${PROJ_ROOT}/main/modules/debug/DebugModule.cpp ${PROJ_ROOT}/main/modules/debug/DebugModuleLog.cpp ${PROJ_ROOT}/main/modules/debug/TraceText.cpp ${PROJ_ROOT}/main/modules/debug/TraceTextValues.cpp
    ${PROJ_ROOT}/main/modules/debug/DebugCmds.cpp
    ${PROJ_ROOT}/components/spp_server/SppTransport.cpp)
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

add_executable(gnss_output_check GnssOutputCheck.cpp
    GnssResetCheck.cpp
    ${PROJ_ROOT}/main/data/GnssOutput.cpp
    ${PROJ_ROOT}/main/modules/gnss/GnssModule.cpp
    ${PROJ_ROOT}/main/modules/gnss/GnssModuleCmds.cpp
    ${PROJ_ROOT}/main/modules/gnss/GnssModuleFrames.cpp
    ${PROJ_ROOT}/main/modules/gnss/GnssModuleMaintenance.cpp
    ${PROJ_ROOT}/main/modules/gnss/GnssModuleProbe.cpp
    ${PROJ_ROOT}/main/modules/gnss/GnssModuleStore.cpp
    ${PROJ_ROOT}/main/modules/storage/StoreKeys.cpp)
target_include_directories(gnss_output_check BEFORE PRIVATE shim)
target_include_directories(gnss_output_check PRIVATE ../t13_host_check/shim/inc
    ${PROJ_ROOT}/main ${PROJ_ROOT}/components/rtos/include
    ${PROJ_ROOT}/components/spp_server/include
    ${PROJ_ROOT}/components/cli/include
    ${PROJ_ROOT}/components/bt_reconnect/include
    ${PROJ_ROOT}/components/nvs_store/include
    ${PROJ_ROOT}/components/app_fw/include
    ${PROJ_ROOT}/components/gnss/include
    ${PROJ_ROOT}/components/stream_demux/include
    ${PROJ_ROOT}/components/link_core/include)
target_compile_options(gnss_output_check PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
target_compile_definitions(gnss_output_check PRIVATE CONFIG_RTK_GNSS_ENABLE=1 CONFIG_RTK_LOG_RING_BYTES=4096
    CONFIG_RTK_GNSS_UART_NUM=2 CONFIG_RTK_GNSS_RX_GPIO=16 CONFIG_RTK_GNSS_TX_GPIO=17
    CONFIG_RTK_GNSS_POWER_GPIO=-1 CONFIG_RTK_GNSS_PORT=2 CONFIG_RTK_GNSS_DEFAULT_BAUD=115200
    CONFIG_RTK_GNSS_RATE_HZ=1)
target_link_libraries(gnss_output_check PRIVATE main_pure main_nvs_shim main_rtos gnss)
add_test(NAME gnss_output COMMAND gnss_output_check)

# 编译真实组合根的内存计划；函数分节仅裁去 host 无法运行的硬件入口，不替换待测绑定/回滚逻辑。
foreach(features on gnss_off trace_off all_off)
    set(boot_target boot_alloc_check_${features})
    add_executable(${boot_target} BootAllocCheck.cpp
        ${PROJ_ROOT}/main/app/App.cpp
        ${PROJ_ROOT}/main/app/BootBuffers.cpp
        ${PROJ_ROOT}/main/app/BootSteps.cpp
        ${PROJ_ROOT}/main/app/ConsoleSetup.cpp
        ${PROJ_ROOT}/components/rtos/BootAlloc.cpp
        ${PROJ_ROOT}/main/data/AppData.cpp
        ${PROJ_ROOT}/main/data/GnssOutput.cpp
        ${PROJ_ROOT}/main/modules/debug/DebugModule.cpp ${PROJ_ROOT}/main/modules/debug/DebugModuleLog.cpp
        ${PROJ_ROOT}/main/modules/debug/TraceText.cpp ${PROJ_ROOT}/main/modules/debug/TraceTextValues.cpp
        ${PROJ_ROOT}/components/spp_server/SppServerInit.c
        ${PROJ_ROOT}/components/spp_server/SppServerSession.c
        ${PROJ_ROOT}/components/cli/src/Reply.cpp)
    target_include_directories(${boot_target} BEFORE PRIVATE shim)
    target_include_directories(${boot_target} PRIVATE ../t13_host_check/shim/inc
        ${PROJ_ROOT}/main ${PROJ_ROOT}/components/spp_server/include
        ${PROJ_ROOT}/components/spp_server/boot_include ${PROJ_ROOT}/components/spp_server/private_include
        ${PROJ_ROOT}/components/cli/include
        ${PROJ_ROOT}/components/nvs_store/include ${PROJ_ROOT}/components/rtos/include
        ${PROJ_ROOT}/components/app_fw/include ${PROJ_ROOT}/components/gnss/include
        ${PROJ_ROOT}/components/bt_reconnect/include)
    target_compile_options(${boot_target} PRIVATE -ffunction-sections -fdata-sections
        -Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-Wnon-virtual-dtor;-Woverloaded-virtual>)
    target_link_options(${boot_target} PRIVATE -Wl,--gc-sections -Wl,--wrap=xTaskCreateStaticPinnedToCore)
    # 与产品 Kconfig 默认值一致；公共 C 头在未跑 Kconfig 时的兼容回退为 2 KiB，不能冒充产品的 4 KiB RX。
    target_compile_definitions(${boot_target} PRIVATE CONFIG_RTK_LOG_RING_BYTES=4096 CONFIG_RTK_BT_SPP_RX_BUF_SIZE=4096)
    if(features STREQUAL "on" OR features STREQUAL "trace_off")
        target_compile_definitions(${boot_target} PRIVATE CONFIG_RTK_GNSS_ENABLE=1)
    endif()
    if(features STREQUAL "trace_off" OR features STREQUAL "all_off")
        target_compile_definitions(${boot_target} PRIVATE FW_TRACE_ENABLED=0)
    endif()
    target_link_libraries(${boot_target} PRIVATE main_pure main_rtos main_nvs_shim)
    add_test(NAME boot_alloc_${features} COMMAND ${boot_target})
endforeach()

# 启动步骤执行器（R8a）：纯 C++ 表驱动逻辑，用注入的计时/堆/降级钩子验证四种结果分类。
add_executable(boot_steps_check BootStepsCheck.cpp ${PROJ_ROOT}/main/app/BootSteps.cpp)
target_include_directories(boot_steps_check PRIVATE ${PROJ_ROOT}/main)
target_compile_options(boot_steps_check PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wshadow
    -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
add_test(NAME boot_steps COMMAND boot_steps_check)
