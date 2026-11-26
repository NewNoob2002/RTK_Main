# 在主机内存 NVS 上运行真实的 C 组件。
cmake_minimum_required(VERSION 3.16)
project(nvs_store_test C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(nvs_store_test TestNvsStore.c FakeNvs.c "${root}/components/nvs_store/NvsStore.c")
target_include_directories(nvs_store_test PRIVATE .
    "${root}/components/nvs_store/include" "${root}/components/nvs_store/boot_include" "${root}/components/app_fw/include"
    "${root}/tools/t13_host_check/shim/inc")
target_compile_options(nvs_store_test PRIVATE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
enable_testing()
foreach(scenario both missing_cfg missing_system different corrupt cfg_recovery system_recovery
                 system_recovery_new_version legacy_pn write_failure freset factory_erase whitelist)
    add_test(NAME nvs_store_${scenario} COMMAND nvs_store_test ${scenario})
endforeach()

add_executable(legacy_eeprom_test TestLegacyMigration.cpp FakeNvs.c
    "${root}/components/nvs_store/NvsStore.c"
    "${root}/components/settings/src/Registry.cpp"
    "${root}/components/device_proto/Bytes.cpp"
    "${root}/main/data/GnssOutput.cpp"
    "${root}/main/modules/storage/LegacyEeprom.cpp"
    "${root}/main/modules/storage/LegacyMigration.cpp"
    "${root}/main/modules/storage/StoreKeys.cpp")
target_include_directories(legacy_eeprom_test PRIVATE .
    "${root}/main" "${root}/components/nvs_store/include" "${root}/components/nvs_store/boot_include"
    "${root}/components/app_fw/include" "${root}/components/device_proto/include"
    "${root}/components/settings/include" "${root}/components/rtos/include"
    "${root}/components/link_core/include" "${root}/components/link_core/private_include"
    "${root}/components/spp_server/include" "${root}/components/stream_demux/include"
    "${root}/components/gnss/include" "${root}/components/cli/include"
    "${root}/tools/main_host_check/shim" "${root}/tools/t13_host_check/shim/inc")
target_compile_options(legacy_eeprom_test PRIVATE -Wall -Wextra -Werror -Wshadow
    -ffunction-sections -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
# 仅保留迁移使用的 StoreKeys 和 GnssOutput 路径。
target_link_options(legacy_eeprom_test PRIVATE -Wl,--gc-sections)
foreach(scenario none bad_len bad_len_large unset golden fields boundaries preserve
                 write_failure readback_failure marker_failure scratch_failure freset)
    add_test(NAME legacy_eeprom_${scenario} COMMAND legacy_eeprom_test ${scenario}
        "${root}/test/fixtures/legacy_eeprom/v27_snpn.bin")
endforeach()
