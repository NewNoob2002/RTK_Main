// 线程安全：纯数据类型，无共享状态。
// 冻结接口（ADR-0007 §3.5；内容自 ADR-0002 的 command_dispatcher.hpp 原样迁出，供 CommandRouter/services 与旧
// dispatcher 共用）。 实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "rtk/proto/alarm_codec.hpp"
#include "rtk/proto/frame.hpp"

namespace rtk::core {

// 定宽、0 填充的字符串字段（不要求 0 结尾）。
template <size_t N> class FixedStr {
public:
    constexpr FixedStr() noexcept = default;

    /** 拷贝 min(text.size(), N) 字节，其余补 0。 */
    constexpr void assign(std::string_view text) noexcept
    {
        for (size_t i = 0; i < N; ++i) {
            data_[i] = (i < text.size()) ? text[i] : '\0';
        }
    }
    /** 到第一个 0 或 N 为止的视图。 */
    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        size_t len = 0;
        while ((len < N) && (data_[len] != '\0')) {
            ++len;
        }
        return {data_.data(), len};
    }
    [[nodiscard]] static constexpr size_t capacity() noexcept
    {
        return N;
    }

private:
    std::array<char, N> data_{};
};

inline constexpr uint32_t kDefaultComBaud = 115200; // 0x0D 应答中的默认波特率（5.4 实际值 UNKNOWN）

// 设备信息（app 层填写；GNSS/电台/电池字段本阶段为预留占位）。
/** 设备信息字段（SN、固件版本、设备名等）的唯一副本；由 DeviceService 持有，链路任务读写。 */
struct DeviceInfo {
    FixedStr<8> product;
    FixedStr<16> sn;
    FixedStr<8> hw_ver;
    FixedStr<16> fw_ver;
    FixedStr<8> board;
    FixedStr<16> board_sn;
    FixedStr<16> board_fw;
    FixedStr<8> radio_type;
    FixedStr<16> radio_fw;
    bool on_battery = true;
    uint8_t battery_pct = proto::alarm::kValueUnknown;
    int8_t battery_temp_c = 0;
    uint32_t com_baud = kDefaultComBaud;
};

inline constexpr size_t kWorkModeLen = 48;
inline constexpr size_t kWorkModeSetLen = 44;
inline constexpr size_t kRadioLen = 28;
inline constexpr size_t kRadioSetMinLen = 15;
inline constexpr size_t kRadioModeOffset = 17;
inline constexpr uint8_t kRadioModeDefault = 0x05;
inline constexpr size_t kLogLen = 50;
inline constexpr size_t kDevInfoLen = 136;
inline constexpr size_t kDevStateLen = 43;
inline constexpr size_t kBatteryLen = 16;
inline constexpr size_t kComLen = 32;
inline constexpr size_t kSatTrackLen = 12;

/** 路由处理结果的分类；只用于决定是否回包以及是否上报错误。 */
enum class DispatchResult : uint8_t {
    kReply = 0, // reply 中有应答帧，发回同一会话
    kNoReply,   // 已处理或按协议不回复
    kDenied,    // 该角色无权执行此命令，不回复
};

/** 复用的应答缓冲与副作用结果（白名单变更、心跳）；链路任务独占，不跨任务共享。 */
struct Reply {
    std::array<uint8_t, proto::kMaxFrame> buf{};
    size_t len = 0;
    bool whitelist_changed = false;                   // app 层需持久化白名单
    std::optional<proto::alarm::Heartbeat> heartbeat; // 收到 0x41 心跳

    void clear() noexcept
    {
        len = 0;
        whitelist_changed = false;
        heartbeat.reset();
    }
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept
    {
        return {buf.data(), len};
    }
};

} // namespace rtk::core
