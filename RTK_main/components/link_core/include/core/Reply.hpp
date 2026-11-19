// 协议应答缓冲、设备信息字段与路由处理结果分类；CommandRouter/services 共用，内容自 ADR-0002 迁出。
// 线程安全：纯数据类型，无共享状态，访问由持有方（链路任务）独占；不分配堆、不阻塞。
// 冻结接口（ADR-0007 §3.5）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/Types.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"

namespace core {

// 定宽、0 填充的字符串字段（不要求 0 结尾）。
template <size_t N> class FixedStr {
public:
    constexpr FixedStr() noexcept = default;

    /** @brief 以 text 填充本字段：拷贝前 N 字节，其余补 0。
     *  @param text 源字符串；超过容量时静默截断，不报错也不置错误标志。
     *  @note 纯值操作，不分配、不阻塞，任意上下文（含 ISR）可调用。 */
    constexpr void assign(std::string_view text) noexcept
    {
        for (size_t i = 0; i < N; ++i) {
            data_[i] = (i < text.size()) ? text[i] : '\0';
        }
    }
    /** @brief 取有效内容视图，遇到第一个 0 或字段边界即止。
     *  @return 指向内部缓冲的只读视图（不含结尾 0）；字段全 0 时为空视图。 */
    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        size_t len = 0;
        while ((len < N) && (data_[len] != '\0')) {
            ++len;
        }
        return {data_.data(), len};
    }
    /** @brief 字段容量。
     *  @return 恒为模板参数 N（字节数）。 */
    [[nodiscard]] static constexpr size_t capacity() noexcept
    {
        return N;
    }

private:
    std::array<char, N> data_{};
};

inline constexpr uint32_t kDefaultComBaud = 115200; // 0x0D 应答中的默认波特率（5.4 实际值 UNKNOWN）

/** 设备信息字段（SN、固件版本、设备名等）的唯一副本；由 DeviceService 持有，链路任务读写。
    电台/电池等上位机可选字段在设备侧为占位值。 */
struct DeviceInfo {
    FixedStr<8> product;
    FixedStr<16> sn;
    FixedStr<8> hwVer;
    FixedStr<16> fwVer;
    FixedStr<8> board;
    FixedStr<16> boardSn;
    FixedStr<16> boardFw;
    FixedStr<8> radioType;
    FixedStr<16> radioFw;
    bool onBattery = true;
    uint8_t batteryPct = proto::alarm::kValueUnknown;
    int8_t batteryTempC = 0;
    uint32_t comBaud = kDefaultComBaud;
    uint8_t fixQuality =
        proto::alarm::kValueUnknown; // GGA 质量（ADR-0010 §2.7），0x40 字节 4；0xFF 未知
    bool gnssRunning = false; // 0x02 字节 2：Controller kRunning 为 0x81，否则 0x01（G-C §7）
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
    kDeferred,  // 应答延后生成；由 LinkService 持有 credit 并轮询发送
};

struct Reply;

/** 延迟应答的轮询回调：就绪时填好 reply 并返回 true；仍在等待返回 false。 */
using DeferredPollFn = bool (*)(void *ctx, uint32_t nowMs, Reply &reply) noexcept;
/** 延迟应答的取消回调：会话断开或槽满时通知服务清理挂起态（可为空）。 */
using DeferredCancelFn = void (*)(void *ctx) noexcept;

/** 一次延迟应答的回调绑定；由 handler 返回 DispatchResult::kDeferred 时写入 Reply::deferred。 */
struct DeferredHook {
    void *ctx = nullptr;
    DeferredPollFn poll = nullptr;
    DeferredCancelFn cancel = nullptr;
};

/** 复用的应答缓冲与副作用结果（白名单变更、心跳、延迟应答）；链路任务独占，不跨任务共享。 */
struct Reply {
    std::array<uint8_t, proto::kMaxFrame> buf{};
    size_t len = 0;
    bool whitelistChanged = false;                    // app 层需持久化白名单
    std::optional<proto::alarm::Heartbeat> heartbeat; // 收到 0x41 心跳
    AppKind appKind = AppKind::kNav;                  // 当前请求帧对应的手机 App 类型
    DeferredHook deferred{};                          // DispatchResult::kDeferred 时有效

    /** @brief 复位应答缓冲与副作用标志，准备组下一帧。
     *  @note 只清长度与标志，不擦 buf 内容；不分配、不阻塞，仅链路任务调用。 */
    void clear() noexcept
    {
        len = 0;
        whitelistChanged = false;
        heartbeat.reset();
        appKind = AppKind::kNav;
        deferred = {};
    }
    /** @brief 取已组好的应答字节。
     *  @return 指向 buf 前 len 字节的只读视图；未组帧时为空视图。 */
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept
    {
        return {buf.data(), len};
    }
};

} // namespace core
