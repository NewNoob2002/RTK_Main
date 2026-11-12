# 控制台组件（ADR-0007 §3.3）：esp_console REPL + 命令表登记 + 参数解析辅助。
# 只依赖 IDF（console、esp_system、串口驱动），不依赖任何 rtk_* 或 app_fw。
idf_component_register(
    SRCS "src/console.cpp" "src/parse.cpp"
    INCLUDE_DIRS "include"
    REQUIRES console esp_system esp_driver_uart
    PRIV_REQUIRES log)

# IDF 宏会触发 -Wold-style-cast/-Wconversion/-Wundef，按 AGENTS.md §3 不开启这三项。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wimplicit-fallthrough)
