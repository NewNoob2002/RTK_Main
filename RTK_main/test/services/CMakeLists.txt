# J2a：CommandRouter 与 4 个服务的 host 测试（ADR-0007 §8、任务卡 J2a）。
# 独立 CMake 工程；Unity 取自 ESP-IDF 自带副本（与 test/host 相同的做法与告警组）。
# 普通构建与 ASan/UBSan 构建都要通过：
#   cmake -S test/services -B builds/services && cmake --build builds/services
#   ctest --test-dir builds/services --output-on-failure
# 与目标一致的语言运行时约束：-fno-exceptions -fno-rtti（CPP-001/002）。
cmake_minimum_required(VERSION 3.22)
project(services_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(DEFINED ENV{IDF_PATH})
    set(IDF_DIR $ENV{IDF_PATH})
else()
    set(IDF_DIR $ENV{HOME}/esp/esp-idf)
endif()
set(UNITY_DIR ${IDF_DIR}/components/unity/unity/src)
if(NOT EXISTS ${UNITY_DIR}/unity.c)
    message(FATAL_ERROR "Unity not found at ${UNITY_DIR}")
endif()

set(PROJ_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LINK_CORE_DIR ${PROJ_ROOT}/components/link_core)
set(DEV_PROTO_DIR ${PROJ_ROOT}/components/device_proto)
# J2b：旧 CommandDispatcher 已退役；R7a 只在构建目录适配其命名空间/include，reference 原件不改。
set(REF_SOURCE ${PROJ_ROOT}/reference/cpp-v3-dispatcher)
set(REF_GOLDEN ${CMAKE_CURRENT_BINARY_DIR}/golden)
set(REF_LEGACY_PREFIX "rtk")
foreach(relative command_dispatcher.cpp ${REF_LEGACY_PREFIX}/core/command_dispatcher.hpp)
    set(source ${REF_SOURCE}/${relative})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
    file(READ ${source} content)
    string(REPLACE "${REF_LEGACY_PREFIX}::" "" content "${content}")
    string(REPLACE "${REF_LEGACY_PREFIX}/" "" content "${content}")
    string(REPLACE "namespace proto = proto;\n" "" content "${content}")
    # 仅适配基础错误码；alarm::Status 仍为协议状态结构，golden 逻辑不变。
    string(REPLACE "const Status " "const fw::Status " content "${content}")
    string(REPLACE "${REF_LEGACY_PREFIX}/" "" output "${relative}")
    get_filename_component(output_dir ${REF_GOLDEN}/${output} DIRECTORY)
    file(MAKE_DIRECTORY ${output_dir})
    file(WRITE ${REF_GOLDEN}/${output} "${content}")
endforeach()

# 纯 C++ 组件的严格告警组（与 test/host/CMakeLists.txt 一致）。
set(PROJ_CXX_STRICT
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wdouble-promotion -Wnull-dereference
    -Wimplicit-fallthrough -Wundef -Wswitch-default)

# C 源文件严格告警（IDF 风格 + BARR-C 安全核心中可由编译器检查的部分）。
set(PROJ_C_STRICT
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align
    -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes
    -Wfloat-equal -Werror=implicit-function-declaration)

add_library(unity STATIC ${UNITY_DIR}/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${UNITY_DIR})

add_library(dev_proto_c STATIC
    ${DEV_PROTO_DIR}/dev_proto_frame.c
    ${DEV_PROTO_DIR}/dev_proto_alarm.c)
target_include_directories(dev_proto_c PUBLIC ${DEV_PROTO_DIR}/include)
target_compile_options(dev_proto_c PRIVATE ${PROJ_C_STRICT})

# 被测代码：device_proto 的 C++ 外观 + link_core（旧 dispatcher 保留作为 golden 基准）。
add_library(link_svc STATIC
    ${PROJ_ROOT}/components/app_fw/src/status.cpp
    # link_core 的事件点经 fw::trace 上报（ADR-0009），测试要链上实现。
    ${PROJ_ROOT}/components/app_fw/src/trace.cpp
    ${DEV_PROTO_DIR}/bytes.cpp
    ${DEV_PROTO_DIR}/frame.cpp
    ${DEV_PROTO_DIR}/alarm_codec.cpp
    ${LINK_CORE_DIR}/alarm_source.cpp
    ${LINK_CORE_DIR}/link_manager.cpp
    ${LINK_CORE_DIR}/link_service.cpp
    ${REF_GOLDEN}/command_dispatcher.cpp
    ${LINK_CORE_DIR}/command_router.cpp
    ${LINK_CORE_DIR}/services/device_service.cpp
    ${LINK_CORE_DIR}/services/config_service.cpp
    ${LINK_CORE_DIR}/services/alarm_service.cpp
    ${LINK_CORE_DIR}/services/pair_service.cpp
    ${LINK_CORE_DIR}/services/gnss_service.cpp)
target_include_directories(link_svc PUBLIC
    ${PROJ_ROOT}/components/app_fw/include
    ${DEV_PROTO_DIR}/include
    ${LINK_CORE_DIR}/include)
target_include_directories(link_svc PRIVATE
    ${LINK_CORE_DIR}/private_include
    # 仅使用构建目录中适配后的 golden 头。
    ${REF_GOLDEN})
target_link_libraries(link_svc PUBLIC dev_proto_c)
target_compile_options(link_svc PRIVATE ${PROJ_CXX_STRICT})

enable_testing()
foreach(name test_command_router test_services_golden test_gnss_service test_link_trace test_alarm_fix_quality
             test_link_credit)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} link_svc unity)
    target_compile_options(${name} PRIVATE ${PROJ_CXX_STRICT})
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# golden 测试包含构建目录中的退役 dispatcher 头。
target_include_directories(test_services_golden PRIVATE ${REF_GOLDEN})
