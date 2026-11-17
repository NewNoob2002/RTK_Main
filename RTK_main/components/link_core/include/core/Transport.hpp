// 会话传输抽象：SPP 驱动实现它；将来 BLE/UART/ESP-NOW 通道可复用 LinkService。
// 运行期选择的边界，按 CPP-011 使用受保护非虚析构 + final 实现。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <cstdint>
#include <span>

#include "core/Types.hpp"
#include "fw/Status.hpp"

namespace core {

/** reply 字节预留凭证；纯逻辑值，不持有指针；仅链路任务使用、非 ISR。 */
struct ReplyCredit {
    uint32_t generation = 0;
    uint32_t serial = 0;
    uint16_t capacity = 0;
    uint8_t slot = 0;
    uint8_t channel = 0;
};

/** 逻辑会话的字节传输抽象；线程约束由实现方声明，调用方是链路任务。 */
class Transport {
public:
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    /** 发送；拥塞时最多等待 timeoutMs。线程安全：只允许链路任务调用。 */
    [[nodiscard]] virtual fw::Status write(SessionHandle handle, std::span<const uint8_t> data,
                                           uint32_t timeoutMs) noexcept = 0;

    /** 可选 reply credit；默认仅兼容旧同步 host Transport，生产装配必须验证为 true。
     * 以下方法只允许链路任务、非 ISR、非阻塞。reserve 成功后 commit 不重新争抢容量；
     * 满返回 Timeout/NoSpace/Busy，调用者保留完整请求并暂停该会话。CLOSE 使凭证失效。
     */
    [[nodiscard]] virtual bool supportsCredit() const noexcept
    {
        return false;
    }
    [[nodiscard]] virtual fw::Status reserveReply(SessionHandle, ReplyCredit &) noexcept
    {
        return fw::Status::kNotSupported;
    }
    [[nodiscard]] virtual fw::Status commitReply(const ReplyCredit &,
                                                 std::span<const uint8_t>) noexcept
    {
        return fw::Status::kNotSupported;
    }
    virtual void cancelReply(const ReplyCredit &) noexcept {}
    [[nodiscard]] virtual bool replyReady(SessionHandle) const noexcept
    {
        return false;
    }
    /** 最新40B推送邮箱，不覆盖 reply；默认保留旧同步 host 行为。 */
    [[nodiscard]] virtual fw::Status pushLatest(SessionHandle handle,
                                                std::span<const uint8_t> bytes) noexcept
    {
        constexpr uint32_t kLegacyTimeoutMs = 200;
        return write(handle, bytes, kLegacyTimeoutMs);
    }

    /** 请求断开；结果以关闭事件异步返回。线程安全：只允许链路任务调用。 */
    [[nodiscard]] virtual fw::Status disconnect(SessionHandle handle) noexcept = 0;

protected:
    Transport() = default;
    ~Transport() = default;
};

} // namespace core
