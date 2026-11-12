# 调试阶段“告警即错误”（AGENTS.md §3）：本工程组件与 main 调用 rtk_strict_warnings(<target>)。
# 原因：IDF v6.1 tools/cmake/build.cmake:184-192 给所有组件加了
#   -Wno-error=unused-function/unused-variable/unused-but-set-variable/deprecated-declarations/extra
#   以及 -Wno-unused-parameter、-Wno-sign-compare；
# 组件自己的 -Werror 不会覆盖针对单项的 -Wno-error=<x>，因此必须逐项写 -Werror=<x> 并重新打开两项告警。
# 开关：Kconfig CONFIG_RTK_STRICT_WARNINGS（main/Kconfig.projbuild，默认 y）；未定义（host 构建、test_apps）时视为开启。
include_guard(GLOBAL)

function(rtk_strict_warnings target)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    if(DEFINED CONFIG_RTK_STRICT_WARNINGS AND NOT CONFIG_RTK_STRICT_WARNINGS)
        return()
    endif()
    # 告警列表放在函数内：include_guard 下文件只展开一次，文件级 set() 只在首个组件的目录作用域可见。
    set(RTK_STRICT_WARNING_FLAGS
        -Werror
        -Werror=unused-function
        -Werror=unused-variable
        -Werror=unused-but-set-variable
        -Werror=deprecated-declarations
        -Werror=extra
        -Wunused-parameter
        -Wsign-compare)
    target_compile_options(${target} PRIVATE ${RTK_STRICT_WARNING_FLAGS})
endfunction()
