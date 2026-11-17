// “无接收机”驱动（ADR-0012 §2.3/§2.2）：探测全表无应答时控制器持有它而不是 nullptr，main 无需判空；能力查询返回 0/false。
// 线程安全：是（无可变状态）。ISR 安全：是。
// 堆与阻塞：不使用堆、不阻塞；不产生字节、不识别帧，所有命令生成返回失败。冻结接口（ADR-0012 §3）实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gnss/Receiver.hpp"

namespace gnss {

class NullReceiver final : public Receiver {
public:
    constexpr NullReceiver() noexcept = default;

    [[nodiscard]] Family family() const noexcept override
    {
        return Family::kUnknown;
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "null";
    }
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override
    {
        return {};
    }
    [[nodiscard]] uint32_t caps(Model) const noexcept override
    {
        return 0U;
    }
    [[nodiscard]] bool buildProbe(CmdBuf &) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool matchProbe(const demux::Decoder &, std::span<const uint8_t>,
                                  ProbeResult &) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool roverStep(size_t, Model, const RoverConfig &,
                                 ConfigStep &) const noexcept override
    {
        return false;
    }
    [[nodiscard]] Ack classifyReply(const demux::Decoder &, std::span<const uint8_t>,
                                    std::span<const uint8_t>) const noexcept override
    {
        return Ack::kNone;
    }
    [[nodiscard]] uint32_t outputMask(Model) const noexcept override
    {
        return 0U;
    }
    [[nodiscard]] fw::Status buildOutput(OutputMsg, uint32_t, Model, const OutputPlan &,
                                         CmdBuf &) const noexcept override
    {
        return fw::Status::kNotSupported;
    }
    [[nodiscard]] OutputMsg classifyFrame(const demux::Decoder &,
                                          std::span<const uint8_t>) const noexcept override
    {
        return OutputMsg::kNone;
    }
    size_t frameTag(const demux::Decoder &, std::span<const uint8_t>,
                    std::span<char>) const noexcept override
    {
        return 0U;
    }
    [[nodiscard]] bool buildReadback(CmdBuf &) const noexcept override
    {
        return false;
    }
    size_t matchReadback(const demux::Decoder &, std::span<const uint8_t>,
                         std::span<char>) const noexcept override
    {
        return 0U;
    }
};

namespace detail {
inline constexpr NullReceiver kNullReceiver{};
} // namespace detail

/**
 * @brief  全局“无接收机”实例
 * @retval 只读的 NullReceiver 引用（静态存储，生命周期同进程），永不返回 nullptr
 * @note   线程/ISR 安全：是（只读）
 */
inline const Receiver &nullReceiver() noexcept
{
    return detail::kNullReceiver;
}

} // namespace gnss
