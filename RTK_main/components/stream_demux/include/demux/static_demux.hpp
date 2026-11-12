// 编译期装配的分流器：解码器集合作为模板参数，缓冲区按最大 max_frame 自动定长，数量越界编译报错。
//   constinit demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder> s_bt_demux;
//   if (s_bt_demux.init(sink) != demux::Error::kOk) { ... }
// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "demux/demux.hpp"

namespace demux {

template <const Decoder &...Ds> class StaticDemux {
public:
    static constexpr std::size_t kCount = sizeof...(Ds);
    static_assert(kCount >= 1 && kCount <= kMaxDecoders, "StaticDemux: 解码器数量必须在 1..kMaxDecoders");
    static constexpr std::size_t kBufSize = std::max({Ds.max_frame...});
    static_assert(kBufSize >= 1 && kBufSize <= 65535U, "StaticDemux: max_frame 必须在 1..65535");

    constexpr StaticDemux() noexcept = default;
    StaticDemux(const StaticDemux &) = delete;
    StaticDemux &operator=(const StaticDemux &) = delete;

    /** 见 Demux::init。线程安全：否。 */
    [[nodiscard]] Error init(Sink &sink) noexcept
    {
        return engine_.init(kTable, buf_, sink);
    }

    [[nodiscard]] Error feed(std::span<const uint8_t> data) noexcept
    {
        return engine_.feed(data);
    }

    [[nodiscard]] Error abort() noexcept
    {
        return engine_.abort();
    }

    [[nodiscard]] bool busy() const noexcept
    {
        return engine_.busy();
    }

    [[nodiscard]] const Stats &stats() const noexcept
    {
        return engine_.stats();
    }

    void reset_stats() noexcept
    {
        engine_.reset_stats();
    }

    /** 解码器在表中的下标（与 Sink::on_frame 的 index 对应）。 */
    template <const Decoder &D> [[nodiscard]] static constexpr uint8_t index_of() noexcept
    {
        // 用模板实参等价性比较身份，而不是 `&D == &Ds`：GCC 在 -fsanitize=undefined 下不把不同对象的地址比较
        // 视为常量表达式（H3 报告 §5 复现），模板实参比较在任何模式下都是编译期常量。
        constexpr std::array<bool, kCount> kMatch{std::is_same_v<Tag<D>, Tag<Ds>>...};
        static_assert((std::is_same_v<Tag<D>, Tag<Ds>> || ...), "StaticDemux::index_of: 解码器不在本实例中");
        for (std::size_t i = 0; i < kCount; ++i) {
            if (kMatch[i]) {
                return static_cast<uint8_t>(i); // i < kCount ≤ 8
            }
        }
        return 0; // 不可达（上面的 static_assert 保证）
    }

private:
    template <const Decoder &> struct Tag {};

    static constexpr std::array<const Decoder *, kCount> kTable{&Ds...};

    std::array<uint8_t, kBufSize> buf_{};
    Demux engine_{};
};

} // namespace demux
