// 固件版本（T10）。与 C 版 APP_VERSION_STR 一致。
#pragma once

#include <string_view>

namespace rtk::app {

inline constexpr std::string_view kAppVersion = "6.1.0-dev";

} // namespace rtk::app
