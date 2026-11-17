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

    /** @brief 是否支持 reply credit 契约（reserve/commit/cancel）。
     *  @return true 表示 credit 可用；默认实现返回 false，仅兼容旧同步 host Transport，
     *          生产装配必须验证为 true。
     *  @note 只允许链路任务调用，非 ISR、非阻塞。 */
    [[nodiscard]] virtual bool supportsCredit() const noexcept
    {
        return false;
    }
    /** @brief 为一条应答预留发送容量，成功后 CLOSE 前凭证有效。
     *  @return kOk 预留成功，凭证由出参写入；kTimeout/kNoSpace/kBusy 容量暂不可用，调用方应保留
     *          完整请求并暂停该会话；kNotSupported 该 Transport 不支持 credit。
     *  @note 失败时出参内容未定义，调用方不得使用；只允许链路任务调用，非 ISR、非阻塞。 */
    [[nodiscard]] virtual fw::Status reserveReply(SessionHandle, ReplyCredit &) noexcept
    {
        return fw::Status::kNotSupported;
    }
    /** @brief 用预留凭证提交完整应答帧，不再重新争抢容量。
     *  @return kOk 已提交；kNotSupported 不支持 credit。
     *  @note 第二个参数为待发送整帧字节，长度不得超过凭证容量；
     *        只允许链路任务调用，非 ISR、非阻塞。 */
    [[nodiscard]] virtual fw::Status commitReply(const ReplyCredit &,
                                                 std::span<const uint8_t>) noexcept
    {
        return fw::Status::kNotSupported;
    }
    /** @brief 放弃未提交的预留，释放容量。
     *  @note 对无效或已失效的凭证幂等；只允许链路任务调用，非 ISR、非阻塞。 */
    virtual void cancelReply(const ReplyCredit &) noexcept {}
    /** @brief 查询会话当前能否立即发出应答，用于组帧前预判拥塞。
     *  @return true 表示 write/commit 预期不会因容量失败；默认实现恒返回 false。
     *  @note 只允许链路任务调用，非 ISR、非阻塞。 */
    [[nodiscard]] virtual bool replyReady(SessionHandle) const noexcept
    {
        return false;
    }
    /** @brief 立即发送最新 40B 推送帧，不排队、不占用 reply 缓冲。
     *  @param handle 目标会话。
     *  @param bytes 推送帧字节；默认实现按 200ms 超时直接 write，拥塞行为与 write 相同。
     *  @return kOk 已发送；其余为 write 的失败码。 */
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
