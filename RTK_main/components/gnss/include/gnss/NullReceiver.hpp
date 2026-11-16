// “无接收机”驱动（ADR-0012 §2.3/§2.2）：探测全表无应答时控制器持有它而不是 nullptr，main 无需判空。
// 所有能力查询返回 0/false，所有命令生成返回失败；不产生字节、不识别帧。
// 线程安全：是（无可变状态）。ISR 安全：是。
// 冻结接口（ADR-0012 §3）。实现方不得修改；变更须先修订 ADR-0012。
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
    [[nodiscard]] bool build_probe(CmdBuf &) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool match_probe(const demux::Decoder &, std::span<const uint8_t>,
                                   ProbeResult &) const noexcept override
    {
        return false;
    }
    [[nodiscard]] bool rover_step(size_t, Model, const RoverConfig &, ConfigStep &) const noexcept override
    {
        return false;
    }
    [[nodiscard]] Ack classify_reply(const demux::Decoder &, std::span<const uint8_t>,
                                     std::span<const uint8_t>) const noexcept override
    {
        return Ack::kNone;
    }
    [[nodiscard]] uint32_t output_mask(Model) const noexcept override
    {
        return 0U;
    }
    [[nodiscard]] fw::Status build_output(OutputMsg, uint32_t, Model, const OutputPlan &,
                                          CmdBuf &) const noexcept override
    {
        return fw::Status::kNotSupported;
    }
    [[nodiscard]] OutputMsg classify_frame(const demux::Decoder &, std::span<const uint8_t>) const noexcept override
    {
        return OutputMsg::kNone;
    }
    size_t frame_tag(const demux::Decoder &, std::span<const uint8_t>, std::span<char>) const noexcept override
    {
        return 0U;
    }
    [[nodiscard]] bool build_readback(CmdBuf &) const noexcept override
    {
        return false;
    }
    size_t match_readback(const demux::Decoder &, std::span<const uint8_t>, std::span<char>) const noexcept override
    {
        return 0U;
    }
};

namespace detail {
inline constexpr NullReceiver kNullReceiver{};
} // namespace detail

/** 全局“无接收机”实例。线程/ISR 安全：是（只读）。 */
inline const Receiver &null_receiver() noexcept
{
    return detail::kNullReceiver;
}

} // namespace gnss
