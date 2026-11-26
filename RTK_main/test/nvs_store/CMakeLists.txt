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
    "${root}/main/modules/storage/LegacyMigration.cpp")
target_include_directories(legacy_eeprom_test PRIVATE .
    "${root}/main" "${root}/components/nvs_store/include" "${root}/components/nvs_store/boot_include"
    "${root}/components/app_fw/include" "${root}/components/settings/include"
    "${root}/tools/t13_host_check/shim/inc")
target_compile_options(legacy_eeprom_test PRIVATE -Wall -Wextra -Werror -Wshadow
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
foreach(scenario none bad_len bad_len_large unset golden v31 boundaries invalid control_text
                 preserve existing_type
                 write_failure readback_failure marker_failure scratch_failure freset)
    add_test(NAME legacy_eeprom_${scenario} COMMAND legacy_eeprom_test ${scenario}
        "${root}/test/fixtures/legacy_eeprom/v27_snpn.bin")
endforeach()
