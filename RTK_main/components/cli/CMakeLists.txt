# 控制台组件（ADR-0007 §3.3）：esp_console REPL + 命令表登记 + 参数解析辅助。
# 只依赖 IDF（console、esp_system、串口驱动），不依赖任何自有业务组件（含 app_fw）。
# src/reply.cpp 与 src/out.cpp 只用 C/C++ 标准库（无 IDF 头），host 测试直接编译。
idf_component_register(
    SRCS "src/console.cpp" "src/parse.cpp" "src/reply.cpp" "src/out.cpp"
    INCLUDE_DIRS "include"
    REQUIRES console esp_system esp_driver_uart
    PRIV_REQUIRES log freertos pthread)

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，按 AGENTS.md §3 不开启这三项。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)

# 调试阶段告警即错误（cmake/strict_warnings.cmake，开关 CONFIG_RTK_STRICT_WARNINGS）
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/strict_warnings.cmake)
rtk_strict_warnings(${COMPONENT_LIB})
