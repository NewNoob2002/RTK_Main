// GNSS 驱动登记表（ADR-0012 §2.3）：新增驱动只需在这里加一行 + 一个驱动源文件。
// 顺序即探测优先级：实测过的 Unicore 在前，实验性的 ComNav 最后。
// 用法（Registry.cpp）：包含前先定义 GNSS_ENABLE_<suffix>（0/1）与 GNSS_DRIVER(suffix, family, type, accessor)
// 宏，然后 `#include "gnss/DriverList.inc"`；行内自带的 #if 守卫负责裁剪未启用驱动。
// 本文件不是 #pragma once 头：它只提供 GNSS_DRIVER 的展开列表。
#if GNSS_ENABLE_UNICORE
GNSS_DRIVER(UNICORE, kUnicore, UnicoreReceiver, unicore_receiver)
#endif
#if GNSS_ENABLE_UBLOX
GNSS_DRIVER(UBLOX, kUblox, UbloxReceiver, ublox_receiver)
#endif
#if GNSS_ENABLE_COMNAV
GNSS_DRIVER(COMNAV, kComnav, ComnavReceiver, comnav_receiver)
#endif
