# 源文件名规范（docs/CODING-STYLE.md §3.1）：C/C++ 文件用 PascalCase，而 Kconfig 开关、测试目标名仍为 snake_case。
# rtk_pascal_case(<out> <snake>)：把 snake_case 名（如 unicore_hash、test_link_service）转换为 PascalCase 文件主名。
include_guard(GLOBAL)

function(rtk_pascal_case out_var snake)
    string(REPLACE "_" ";" _parts "${snake}")
    set(_result "")
    foreach(_p IN LISTS _parts)
        string(SUBSTRING "${_p}" 0 1 _head)
        string(SUBSTRING "${_p}" 1 -1 _tail)
        string(TOUPPER "${_head}" _head)
        string(APPEND _result "${_head}${_tail}")
    endforeach()
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()
