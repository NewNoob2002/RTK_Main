# C 驱动 + C++ 适配器（ADR-0003）。经典蓝牙只在 SOC_BT_CLASSIC_SUPPORTED 目标编译。
# 注意：IDF 的组件依赖收集阶段（component_get_requirements.cmake）不加载 sdkconfig，
# CONFIG_SOC_BT_CLASSIC_SUPPORTED 尚未定义；若此时走“不支持”分支，bt 会从 REQUIRES 中丢失，
# 导致真实构建时找不到 esp_bt.h。因此在“未定义”时按支持处理（只收集依赖与源文件），
# 真实构建阶段再按 sdkconfig 决定是否编译。target_compile_options 不能在早期展开阶段调用。
if(DEFINED CONFIG_SOC_BT_CLASSIC_SUPPORTED AND NOT CONFIG_SOC_BT_CLASSIC_SUPPORTED)
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES link_core rtos app_fw bt)
    message(WARNING "rtk_bt: CONFIG_SOC_BT_CLASSIC_SUPPORTED=0，SPP 驱动与适配器未编译")
else()
    idf_component_register(
        SRCS "rtk_bt_spp_init.c" "rtk_bt_spp_session.c" "rtk_bt_spp_rx.c" "rtk_bt_spp_tx.c" "rtk_bt_spp_gap.c"
             "spp_transport.cpp"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "private_include"
        REQUIRES link_core rtos app_fw bt)

    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        target_compile_options(${COMPONENT_LIB} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wimplicit-fallthrough -Wstrict-prototypes>
            $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
                -Wimplicit-fallthrough>)
    endif()
endif()
