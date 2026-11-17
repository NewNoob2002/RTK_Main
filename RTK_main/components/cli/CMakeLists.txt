# 控制台组件（ADR-0007 §3.3）：esp_console REPL + 命令表登记 + 参数解析辅助。
# 依赖 IDF（console、esp_system、串口驱动）与框架基座 app_fw（fw/Log.hpp，AGENTS.md §3），不依赖其他自有组件。
# src/Reply.cpp 与 src/Out.cpp 只用 C/C++ 标准库（无 IDF 头），host 测试直接编译。
idf_component_register(
    SRCS "src/Console.cpp" "src/Parse.cpp" "src/Reply.cpp" "src/Out.cpp"
    INCLUDE_DIRS "include"
    REQUIRES console esp_system esp_driver_uart app_fw
    PRIV_REQUIRES log freertos pthread)

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，按 AGENTS.md §3 不开启这三项。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})

# 未知命令输出 ERR：包装 esp_console_run（实现与原因见 src/Console.cpp 末尾 __wrap_esp_console_run）。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_console_run")
