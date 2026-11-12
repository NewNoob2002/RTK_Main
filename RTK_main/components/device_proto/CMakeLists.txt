# 纯组件（ADR-0003）：协议核心用 C（C11 子集），C++ 外观委托给它。
# 不依赖任何 IDF/FreeRTOS 头文件，必须能被 test/host/CMakeLists.txt 用 host gcc/g++ 编译。
# 告警组按 AGENTS.md §3，用生成器表达式区分 C/C++。
idf_component_register(
    SRCS "rtk_proto_frame.c" "rtk_proto_alarm.c"
         "bytes.cpp" "frame.cpp" "alarm_codec.cpp"
         "demux_frame.cpp"
    INCLUDE_DIRS "include"
    # ADR-0005：只服务于 demux_frame.cpp（产品帧的 stream_demux 解码器）；
    # 上面的 C 核心与其它 C++ 外观不引用 stream_demux，也不包含它的头。
    REQUIRES stream_demux app_fw)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
        -Wimplicit-fallthrough -Wswitch-default -Wundef -Wstrict-prototypes -Wmissing-prototypes>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough
        -Wswitch-default -Wundef>)
