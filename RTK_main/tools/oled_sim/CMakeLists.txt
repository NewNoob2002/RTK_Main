cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(oled_sim LANGUAGES C CXX)
    enable_testing()
endif()
get_filename_component(RTK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

foreach(component stream_demux app_fw gnss)
    if(NOT TARGET ${component})
        add_subdirectory("${RTK_ROOT}/components/${component}" "${component}")
    endif()
endforeach()
if(NOT TARGET third_party_wououi)
    add_subdirectory("${RTK_ROOT}/components/third_party" third_party)
endif()

add_executable(oled_sim OledSim.cpp
    "${RTK_ROOT}/main/board/PowerLogic.cpp"
    "${RTK_ROOT}/main/modules/oled/OledAnim.cpp"
    "${RTK_ROOT}/main/modules/oled/OledUi.cpp"
    "${RTK_ROOT}/main/modules/oled/OledRender.cpp")
target_compile_features(oled_sim PRIVATE cxx_std_20)
target_compile_options(oled_sim PRIVATE -Wall -Wextra -Werror -Wshadow)
target_include_directories(oled_sim PRIVATE
    "${RTK_ROOT}/main" "${RTK_ROOT}/components/link_core/include"
    "${RTK_ROOT}/components/cellular/include")
target_link_libraries(oled_sim PRIVATE PkgConfig::SDL2 third_party_wououi gnss)

add_test(NAME oled_sim_headless COMMAND oled_sim --headless
    --script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tour.txt"
    --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME oled_sim_comm COMMAND oled_sim --headless
    --script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/comm.txt"
    --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME oled_sim_standby COMMAND oled_sim --headless --boot standby
    --script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/standby.txt"
    --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")

add_test(NAME oled_sim_ol8 COMMAND oled_sim --headless
    --script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ol8.txt"
    --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")

add_test(NAME oled_sim_alarm COMMAND oled_sim --headless
    --script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/alarm.txt"
    --golden "${CMAKE_CURRENT_SOURCE_DIR}/golden")
