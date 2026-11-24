# 第三方开源库汇总组件（ADR-0020）：当前仅包含 WouoUI-PageVersion（MPL-2.0）。
# 仅编译 OL1 所需的图形与字体子集，不引入未使用的页面/弹窗状态机。
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "wououi/WouoUI_graph.c"
             "wououi/WouoUI_font.c"
        INCLUDE_DIRS "wououi"
    )
    # 第三方上游 C 代码保留原样以符合 MPL-2.0 最小改动原则，仅抑制 char 下标类无关告警以维持 0-warning 构建。
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-char-subscripts -Wno-implicit-fallthrough)
else()
    add_library(third_party_wououi STATIC
        "${CMAKE_CURRENT_LIST_DIR}/wououi/WouoUI_graph.c"
        "${CMAKE_CURRENT_LIST_DIR}/wououi/WouoUI_font.c"
    )
    target_include_directories(third_party_wououi PUBLIC "${CMAKE_CURRENT_LIST_DIR}/wououi")
    target_compile_options(third_party_wououi PRIVATE -Wno-char-subscripts -Wno-implicit-fallthrough)
endif()
