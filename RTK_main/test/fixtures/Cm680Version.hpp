// ADR-0012 §2.9 的 CM680 实板 VERSIONA 原始帧，包含串口行结束符。
// 只读测试数据，无线程约束。
#pragma once

#include <array>
#include <string_view>

namespace gnss_test {

inline constexpr std::array<std::string_view, 3U> kCm680VersionFrames{
    "#VERSIONA,99,GPS,FINE,0,0,0,0,0,4761795;\"CM680\",\"CM680_SDK.1.2.5a_GNSS.1.1.9\","
    "\"20381121\",\"10141001110\",\"000039AF76220240\",\"2024/12/20\"*489EA8BF\r\n",
    "#VERSIONA,99,GPS,FINE,0,0,0,0,0,4768416;\"CM680\",\"CM680_SDK.1.2.5a_GNSS.1.1.9\","
    "\"20381121\",\"10141001110\",\"000039AF76220240\",\"2024/12/20\"*656CB59D\r\n",
    "#VERSIONA,99,GPS,FINE,0,0,0,0,0,4775069;\"CM680\",\"CM680_SDK.1.2.5a_GNSS.1.1.9\","
    "\"20381121\",\"10141001110\",\"000039AF76220240\",\"2024/12/20\"*9D119AAF\r\n",
};

} // namespace gnss_test
