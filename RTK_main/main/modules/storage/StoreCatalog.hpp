// 持久化目录（ADR-0010 §2.10）：把各模块的 fw::StoreKey 表按“类别”登记成一张表，供启动加载与 `config` 只读查看。
// SN 与报警器白名单走冻结的 nvs_store C API（ns "rtk"），不在本目录里，由 config 命令单独展示。
// 纯常量数据，无状态；可在任何任务读取。
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "fw/StoreKey.hpp"

namespace app {

/**
 * 数值键的显示名钩子：key 为键名，value 为 NVS 读出的数值；有对应名字返回静态字符串，否则 nullptr
 * （控制台只打印数字）。fw::StoreKey 冻结，故挂在类别上。线程/ISR 安全：须为纯函数。
 */
using StoreValueName = const char *(*)(std::string_view key, uint32_t value) noexcept;

/** 一个持久化类别：名字（控制台用）、说明与该类别的键表；功能关闭时键表为空。 */
struct StoreGroup {
    std::string_view name;  // 例如 "gnss.config"
    std::string_view title; // 一行说明
    std::span<const fw::StoreKey> keys;
    StoreValueName valueName = nullptr; // 可空：数值键不需要显示名
};

/** 全部类别，按显示顺序排列。 */
[[nodiscard]] std::span<const StoreGroup> storeCatalog() noexcept;

} // namespace app
