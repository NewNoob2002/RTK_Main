cmake_minimum_required(VERSION 3.16)
project(ota_host_tests LANGUAGES CXX)

add_subdirectory(../../components/app_fw app_fw_build)
add_subdirectory(../../components/web_proto web_proto_build)

enable_testing()
add_executable(ota_test TestOta.cpp)
target_include_directories(ota_test PRIVATE ../../main)
target_link_libraries(ota_test PRIVATE web_proto)
target_compile_features(ota_test PRIVATE cxx_std_20)
target_compile_options(ota_test PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion
    -Wsign-conversion -Wswitch-default -fno-exceptions -fno-rtti)
add_test(NAME ota COMMAND ota_test)
