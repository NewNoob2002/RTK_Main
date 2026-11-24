# 组合根与组件注册（ADR-0002 §4）。C 驱动 + C++ 应用（ADR-0003）。
# esp_hw_support：esp_read_mac；esp_system：复位原因/堆/TWDT；console：UART0 REPL。
# esp_adc / esp_driver_gpio：板型识别与电源保持。esp_driver_uart：控制台与 GNSS。
# 模块源码按目录发现；IDF 的依赖扫描以脚本模式运行，不能使用 CONFIGURE_DEPENDS。
set(web_priv_requires esp_wifi esp_netif esp_event esp_http_server web_proto)
set(web_embed_files "")
set(web_ui_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
set(web_config_enabled ON)
if(DEFINED CONFIG_RTK_WIFI_CONFIG_ENABLE AND NOT CONFIG_RTK_WIFI_CONFIG_ENABLE)
    set(web_config_enabled OFF)
    set(web_priv_requires "")
endif()

if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "modules/*.cpp")
    # 模板仅作新增模块参考并由主机测试持续编译，不编入固件。
    list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/_template/")
    # ADR-0009 §2.6：追踪关闭时整个 debug 模块不编译（module_list.inc 与 app_main 同步按 FW_TRACE_ENABLED 排除）。
    if(DEFINED CONFIG_RTK_TRACE_ENABLE AND NOT CONFIG_RTK_TRACE_ENABLE)
        list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/debug/")
    endif()
    # ADR-0010 §2.7：GNSS 关闭时整个 gnss 模块不编译。
    if(DEFINED CONFIG_RTK_GNSS_ENABLE AND NOT CONFIG_RTK_GNSS_ENABLE)
        list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/gnss/")
    endif()
    # ADR-0017 §4：关闭 Wi-Fi 配置模式时不编译 web 模块、不嵌入页面、不链入 Wi-Fi（G3-idf-wifi-off）。
    if(NOT web_config_enabled)
        list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/web/")
    else()
        if(DEFINED CONFIG_RTK_OTA_ENABLE AND NOT CONFIG_RTK_OTA_ENABLE)
            list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/web/WebOta\\.cpp$")
        endif()
        # W7 F8：关闭强制门户时不编译 WebDns.cpp。
        if(DEFINED CONFIG_RTK_WIFI_CAPTIVE_PORTAL AND NOT CONFIG_RTK_WIFI_CAPTIVE_PORTAL)
            list(FILTER module_sources EXCLUDE REGEX "(^|/)modules/web/WebDns\\.cpp$")
        endif()
        if(NOT CMAKE_BUILD_EARLY_EXPANSION)
            set(web_ui_dir "${CMAKE_CURRENT_LIST_DIR}/web/ui")
            set(web_pack_script "${CMAKE_CURRENT_LIST_DIR}/../tools/web_mock/pack.py")
            add_custom_command(
                OUTPUT "${web_ui_gz}"
                COMMAND python3 "${web_pack_script}" --ui-dir "${web_ui_dir}" --out "${web_ui_gz}"
                DEPENDS "${web_pack_script}"
                        "${web_ui_dir}/index.html"
                        "${web_ui_dir}/app.css"
                        "${web_ui_dir}/app.js"
                        "${web_ui_dir}/logo.png"
                COMMENT "Packing and gzipping web UI (<= 24 KB)"
                VERBATIM)
            set(web_embed_files "${web_ui_gz}")
        endif()
    endif()
endif()
idf_component_register(
    SRCS "AppMain.cpp"
         "app/App.cpp"
         "app/BootBuffers.cpp"
         "app/BootConfig.cpp"
         "app/BootFlow.cpp"
         "app/BootSteps.cpp"
         "app/ConsoleSetup.cpp"
         "app/Executors.cpp"
         "app/SettingsBackend.cpp"
         "app/RadioMode.cpp"
         "board/BoardCurrent.cpp"
         "board/BoardProfile.cpp"
         "board/BoardStartup.cpp"
         "board/I2cBus.cpp"
         "board/Charger.cpp"
         "board/Sh1106.cpp"
         "board/BatteryAdc.cpp"
         "board/PowerLogic.cpp"
         "data/AppData.cpp"
         "data/GnssAppRing.cpp"
         "data/GnssOutput.cpp"
         "support/ConsoleSupport.cpp"
         "support/ResetReason.cpp"
         ${module_sources}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../components/link_core/private_include" "../components/gnss/private_include"
    PRIV_REQUIRES app_update esp_app_format bootloader_support espcoredump esp_adc esp_driver_i2c bt third_party ${web_priv_requires}
    REQUIRES app_fw settings device_proto link_core rtos spp_server nvs_store cli stream_demux gnss console esp_system esp_hw_support esp_driver_gpio esp_driver_uart freertos log
    EMBED_FILES ${web_embed_files})

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，本项目对其他依赖 IDF 的组件
# 同样不开启这三项；其余告警按 AGENTS.md 全开。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
