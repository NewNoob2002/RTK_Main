// 冻结接口（ADR-0002）。实现方不得修改。
// NVS 持久化：设备 SN、报警器白名单（其余配置预留）。命名空间 "rtk"；键 "sn"（字符串）、
// "alarm_wl"（blob，n×6 字节）。每次操作 open→操作→close（RAII 句柄），写后 commit。
// 线程安全：否（只由启动流程与链路任务调用，二者不并发）。
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "rtk/core/command_dispatcher.hpp"
#include "rtk/core/types.hpp"
#include "rtk/status.hpp"

namespace rtk::store {

inline constexpr size_t kSnMax = 16;

class SettingsStore {
public:
    constexpr SettingsStore() noexcept = default;

    /**
     * nvs_flash_init；遇到 NO_FREE_PAGES / NEW_VERSION_FOUND 时只擦除 nvs 分区后重试一次（IDF 标准做法），
     * 并以 WARNING 日志记录。
     */
    [[nodiscard]] Status init() noexcept;

    /** 不存在 → kNotFound；长度为 0 或 > kSnMax → kCorrupt。 */
    [[nodiscard]] Status load_sn(core::FixedStr<kSnMax> &sn) const noexcept;
    /** 长度 0 或 > kSnMax → kInvalidArg。 */
    [[nodiscard]] Status save_sn(std::string_view sn) const noexcept;

    /**
     * 读白名单到 out，count 返回条数（≤ out.size()）。不存在：count=0 返回 kOk。
     * blob 长度不是 6 的倍数或超过 kMaxAlarms×6：count=0 返回 kCorrupt。
     */
    [[nodiscard]] Status load_alarms(std::span<core::BtAddr> out, size_t &count) const noexcept;
    /** 覆盖写；空 span 表示删除键。超过 kMaxAlarms → kInvalidArg。 */
    [[nodiscard]] Status save_alarms(std::span<const core::BtAddr> alarms) const noexcept;

private:
    bool initialized_ = false;
};

} // namespace rtk::store
