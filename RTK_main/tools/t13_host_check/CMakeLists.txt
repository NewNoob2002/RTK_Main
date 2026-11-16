# T13 驱动级回归：用 host 替身（FreeRTOS + IDF）编译真实的 components/spp_server/spp_server_*.c。
# 不参与固件构建，也不进 test/host（test/host 属冻结输入），单独一个 CMake 工程：
#   cmake -S tools/t13_host_check -B builds/t13 && cmake --build builds/t13
#   ctest --test-dir builds/t13 --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(t13_host_check C)

# 为 .clangd 提供包含 FreeRTOS/IDF shim 的主机编译命令。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SPP_SERVER_INC "${PROJ_ROOT}/components/spp_server/include")
set(SPP_SERVER_BOOT_INC "${PROJ_ROOT}/components/spp_server/boot_include")
set(SPP_SERVER_PRIV_INC "${PROJ_ROOT}/components/spp_server/private_include")
# 替身头文件目录必须放在 IDF 之前；host 环境没有 IDF。
set(SHIM_INC "${CMAKE_CURRENT_SOURCE_DIR}/shim/inc")

find_package(Threads REQUIRED)

add_library(fake_rtos STATIC shim/fake_rtos.c)
target_include_directories(fake_rtos PUBLIC "${SHIM_INC}")
target_link_libraries(fake_rtos PUBLIC Threads::Threads)

add_library(fake_idf STATIC shim/fake_idf.c)
target_include_directories(fake_idf PUBLIC "${SHIM_INC}")
target_link_libraries(fake_idf PUBLIC fake_rtos)

# 被检查的真实源码：与固件同一份文件（ADR-0008 R5 拆成 5 个职责文件），按 AGENTS.md 对
# IDF 依赖组件的 C 告警集编译。
add_library(spp_server_driver STATIC
    "${PROJ_ROOT}/components/spp_server/spp_server_init.c"
    "${PROJ_ROOT}/components/spp_server/spp_server_session.c"
    "${PROJ_ROOT}/components/spp_server/spp_server_rx.c"
    "${PROJ_ROOT}/components/spp_server/spp_server_tx.c"
    "${PROJ_ROOT}/components/spp_server/spp_server_gap.c")
target_include_directories(spp_server_driver PUBLIC "${SHIM_INC}" "${SPP_SERVER_INC}" "${SPP_SERVER_BOOT_INC}")
target_include_directories(spp_server_driver PRIVATE "${SPP_SERVER_PRIV_INC}")
target_link_libraries(spp_server_driver PUBLIC fake_idf)
target_compile_options(spp_server_driver PRIVATE
    -fno-builtin-memcpy # 仅 host：K2 通过 --wrap=memcpy 给复制窗口加确定性门闩。
    -Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes)

add_executable(t13_host_check t13_host_check.c)
# fake_idf.h 在 shim/ 根（shim/inc 只放 IDF/FreeRTOS 头）。测试直接包含它，
# 因此单独给测试目标加该目录；不移动头文件。
target_include_directories(t13_host_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim")
target_link_libraries(t13_host_check PRIVATE spp_server_driver)

enable_testing()

# 每个场景独立进程：驱动只有 init、没有 deinit 公共接口，静态对象在进程内不复用。
set(T13_SCENARIOS
    boot_stop
    boot_stop_failure
    boot_stop_after_init_failure
    storage_binding
    rx_wake_level_and_stats
    reject_rearm_after_close
    lifecycle_burst_churn
    drop_counts_by_handle
    disconnect_async_and_reject
    disconnect_retry
    disconnect_reuse_across_slots
    reject_dedup_hole
    init_waits_stages
    init_sync_stage_order
    init_failure_rollback
    init_start_status_fail
    init_start_timeout
    open_no_slot_rejects_from_task
    slot_held_rejects_new_connect
    write_waits_completion
    write_chunks_large_payload
    write_timeout_budget
    write_status_propagation
    write_submit_error
    write_congestion
    write_close_midflight
    write_cancel_by_close
    write_cancel_during_submit
    stale_completion_after_reopen
    write_unmatched_no_resource
    write_stale_failure_ignored
    write_chunk_budget_shared
    unknown_handle_errors
    gap_replies_from_task)

foreach(scenario IN LISTS T13_SCENARIOS)
    add_test(NAME "t13_${scenario}" COMMAND t13_host_check "${scenario}")
    set_tests_properties("t13_${scenario}" PROPERTIES TIMEOUT 30)
endforeach()

add_executable(k2_tx_check k2_tx_check.c)
target_include_directories(k2_tx_check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${SPP_SERVER_PRIV_INC}")
target_link_libraries(k2_tx_check PRIVATE spp_server_driver)
target_link_options(k2_tx_check PRIVATE "-Wl,--wrap=memcpy")
target_compile_options(k2_tx_check PRIVATE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
foreach(scenario capacity stream_order early_close anonymous short submit_error failure copy_close copy_unbind)
    add_test(NAME "k2_tx_${scenario}" COMMAND k2_tx_check "${scenario}")
    set_tests_properties("k2_tx_${scenario}" PROPERTIES TIMEOUT 30)
endforeach()
