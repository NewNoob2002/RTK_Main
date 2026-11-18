// 编译期装配的分流器：解码器集合作模板参数，缓冲区按最大 maxFrame 定长，数量/长度越界即编译报错；用法
//   constinit demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder> s_bt_demux; s_bt_demux.init(sink);
// 约束同 Demux：一个实例只归一个任务使用（线程安全：否），不阻塞、无堆，回调在 feed/abort 内同步执行，ISR 可调用但须独占。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "demux/Demux.hpp"

namespace demux {

/** @brief 按模板参数装配的 Demux 包装：省去手工传表与缓冲区，越界错误在编译期暴露。 */
template <const Decoder &...Ds> class StaticDemux {
public:
    static constexpr std::size_t kCount = sizeof...(Ds);
    static_assert(kCount >= 1 && kCount <= kMaxDecoders,
                  "StaticDemux: 解码器数量必须在 1..kMaxDecoders");
    static constexpr std::size_t kBufSize = std::max({Ds.maxFrame...});
    static constexpr std::size_t kBufSizeMax = 65535U; // 单缓冲区上限（uint16 长度字段）
    static_assert(kBufSize >= 1 && kBufSize <= kBufSizeMax,
                  "StaticDemux: maxFrame 必须在 1..65535");

    constexpr StaticDemux() noexcept = default;
    ~StaticDemux() = default;
    StaticDemux(const StaticDemux &) = delete;
    StaticDemux &operator=(const StaticDemux &) = delete;

    /**
     * @brief  初始化引擎：建立首字节位图并清空状态与统计
     * @param  sink: 分流结果接收方，生命周期须覆盖本对象
     * @retval kOk 初始化完成，可以 feed
     * @retval kInvalidArg 参数无效导致引擎拒绝（正常情况下模板装配已在编译期排除）
     * @retval kReentry 在本实例回调中调用，状态不变
     * @note   契约同 Demux::init；线程安全：否。
     */
    [[nodiscard]] Error init(Sink &sink) noexcept
    {
        return engine_.init(kTable, buf_, sink);
    }

    /**
     * @brief  喂入任意长度字节，本调用内同步完成能完成的回调
     * @param  data: 新到的字节；只在调用期间读取
     * @retval kOk 全部字节已处理；kInvalidArg 未 init 成功；kReentry 在回调中调用
     * @note   分块无关（ADR-0004 §3 I1）；契约同 Demux::feed。
     */
    [[nodiscard]] Error feed(std::span<const uint8_t> data) noexcept
    {
        return engine_.feed(data);
    }

    /**
     * @brief  放弃当前半帧（空闲超时、断链），剩余字节作为未知字节输出后回到空闲
     * @retval kOk 已复位（本来空闲时也返回 kOk）；kInvalidArg 未 init 成功；kReentry 在回调中调用
     * @note   契约同 Demux::abort。
     */
    [[nodiscard]] Error abort() noexcept
    {
        return engine_.abort();
    }

    /**
     * @brief  是否有半帧在累积
     * @retval true 有半帧（调用方据此启动空闲超时）；false 空闲
     */
    [[nodiscard]] bool busy() const noexcept
    {
        return engine_.busy();
    }

    /**
     * @brief  读取累计统计
     * @retval 统计表的只读引用；下一次 init() 前一直有效
     */
    [[nodiscard]] const Stats &stats() const noexcept
    {
        return engine_.stats();
    }

    /** @brief 清零统计（不影响解析状态）。 */
    void resetStats() noexcept
    {
        engine_.resetStats();
    }

    /**
     * @brief  取解码器在表中的下标（与 Sink::onFrame 的 index 对应）
     * @tparam D: 本实例的解码器之一；不在表中时 static_assert 编译失败
     * @retval 0..kCount-1 的编译期常量
     */
    template <const Decoder &D> [[nodiscard]] static constexpr uint8_t indexOf() noexcept
    {
        // 用模板实参等价性比较身份，而不是 `&D == &Ds`：GCC 在 -fsanitize=undefined 下不把不同对象的地址比较
        // 视为常量表达式，模板实参比较在任何模式下都是编译期常量。
        constexpr std::array<bool, kCount> kMatch{std::is_same_v<Tag<D>, Tag<Ds>>...};
        static_assert((std::is_same_v<Tag<D>, Tag<Ds>> || ...),
                      "StaticDemux::indexOf: 解码器不在本实例中");
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
