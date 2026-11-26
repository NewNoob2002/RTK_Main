# 在主机内存 NVS 上运行真实的 C 组件。
cmake_minimum_required(VERSION 3.16)
project(nvs_store_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(nvs_store_test TestNvsStore.c FakeNvs.c "${root}/components/nvs_store/NvsStore.c")
target_include_directories(nvs_store_test PRIVATE .
    "${root}/components/nvs_store/include" "${root}/components/app_fw/include"
    "${root}/tools/t13_host_check/shim/inc")
target_compile_options(nvs_store_test PRIVATE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
enable_testing()
foreach(scenario both missing_cfg missing_system different corrupt cfg_recovery system_recovery
                 system_recovery_new_version legacy_pn write_failure freset factory_erase whitelist)
    add_test(NAME nvs_store_${scenario} COMMAND nvs_store_test ${scenario})
endforeach()
