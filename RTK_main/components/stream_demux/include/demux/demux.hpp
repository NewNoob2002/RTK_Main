// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN（待人工审查签字）
//
// 通用多协议字节流分流器（stream demultiplexer），C++20 子集，平台无关：
// - 只依赖 <array> <cstddef> <cstdint> <span>（实现可用 <cstring> <algorithm>）；无 OS/SDK、无堆、无异常/RTTI、无浮点。
// - 一个实例对应一条字节流（蓝牙会话、GNSS UART、TCP 连接……）；帧缓冲区由调用方提供，或用 StaticDemux 编译期自动定长。
// - 协议由 Decoder 描述符插入；引擎负责找帧起点、累积、失败回放重扫、回调分发与统计（语义见 ADR-0004 §3）。
// 线程安全：否（一个实例只能由一个任务使用）。ISR 安全：是（无阻塞、无 OS 调用），回调在 feed/abort 内同步执行。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/decoder.hpp"

namespace demux {

/** 一个实例最多挂载的解码器数（首字节表用 uint8_t 位图）。 */
inline constexpr std::size_t kMaxDecoders = 8;

/**
 * 分流结果的接收方。调用方实现它（建议 final），生命周期覆盖所挂载的 Demux。
 * 运行期多态边界：受保护非虚析构，不能经基类指针销毁（CPP-011，ADR-0005 §2 授权的第二个多态边界）。
 * 两个回调都在 feed()/abort() 内同步执行；数据 span 只在回调返回前有效。回调中对同一实例调用
 * init/feed/abort 返回 Error::kReentry 且不改变状态。
 */
class Sink {
public:
    Sink(const Sink &) = delete;
    Sink &operator=(const Sink &) = delete;

    /** 一条完整帧。index 为该解码器在 init 时解码器表中的下标。 */
    virtual void on_frame(const Decoder &decoder, uint8_t index, std::span<const uint8_t> frame) noexcept = 0;

    /**
     * 一段不属于任何已挂载协议的字节（按流顺序；同一段连续未知字节可能分多次回调）。
     * 默认实现丢弃（统计照常计数）。
     */
    virtual void on_unknown(std::span<const uint8_t> bytes) noexcept
    {
        static_cast<void>(bytes);
    }

protected:
    constexpr Sink() = default;
    ~Sink() = default;
};

enum class Error : uint8_t {
    kOk = 0,
    kInvalidArg, // 解码器表为空/超过 kMaxDecoders/含空指针或无效描述符、缓冲区小于最大 max_frame、尚未 init 成功
    kReentry,    // 在本实例回调中调用了 init/feed/abort
};

struct DecoderStats {
    uint32_t frames = 0;  // 成功帧数
    uint32_t rejects = 0; // 已认领后 kReject 的次数（含 step 契约违规）
    uint32_t bytes = 0;   // 成功帧总字节数
};

struct Stats {
    std::array<DecoderStats, kMaxDecoders> decoder{};
    uint32_t unknown_bytes = 0; // 交给 on_unknown 的字节数
    uint32_t resyncs = 0;       // 所有候选都拒绝后起点前移重扫的次数
    uint32_t overflows = 0;     // 解码器要求的长度超过缓冲区容量而被强制拒绝的次数
    uint32_t aborts = 0;        // abort() 时存在半帧的次数
};

/**
 * 分流引擎。constexpr 构造后处于“未初始化”状态（feed/abort 返回 kInvalidArg），可 constinit 定义为全局对象；
 * 不存在“未清零存储”问题（C 版 G1 阻塞项 1 由默认成员初值消除）。独占缓冲区，禁止拷贝与移动。
 */
class Demux {
public:
    constexpr Demux() noexcept = default;
    Demux(const Demux &) = delete;
    Demux &operator=(const Demux &) = delete;

    /**
     * 初始化（或重新初始化）：校验参数、建立 256 项首字节位图、清空状态与统计。
     * decoders 下标越小优先级越高（同首字节时先尝试）；表本身、描述符、buf、sink 必须在实例生命周期内有效。
     * 失败时实例回到未初始化状态。线程安全：否。
     */
    [[nodiscard]] Error init(std::span<const Decoder *const> decoders, std::span<uint8_t> buf, Sink &sink) noexcept;

    /**
     * 喂入任意长度字节（可为空）。本调用内同步完成所有能完成的帧回调与未知字节回调。
     * 分块无关（ADR-0004 §3 I1）：同一字节序列任意切分喂入，帧回调序列相同，未知字节拼接后相同。
     */
    [[nodiscard]] Error feed(std::span<const uint8_t> data) noexcept;

    /** 放弃当前半帧（流空闲超时、断链）：按 kReject 回放重扫，剩余字节作为未知字节输出，回到空闲。 */
    [[nodiscard]] Error abort() noexcept;

    /** 是否有半帧在累积（调用方据此决定是否启动空闲超时）。 */
    [[nodiscard]] bool busy() const noexcept
    {
        return len_ != 0;
    }

    [[nodiscard]] const Stats &stats() const noexcept
    {
        return stats_;
    }

    /** 清零统计，不影响解析状态。回调中调用也安全。 */
    void reset_stats() noexcept
    {
        stats_ = Stats{};
    }

private:
    static constexpr uint8_t kNoCandidate = 0xFF;

    std::array<const Decoder *, kMaxDecoders> decoders_{};
    uint8_t decoder_count_ = 0; // 0 = 未初始化
    uint8_t *buf_ = nullptr;
    std::size_t buf_size_ = 0;
    Sink *sink_ = nullptr;
    std::array<uint8_t, 256> first_mask_{}; // bit i = decoders_[i] 接受该首字节
    std::size_t len_ = 0;                   // buf_ 中已累积的字节数
    std::size_t need_ = 0;                  // 当前候选下次 step 前至少还要的字节数
    uint8_t active_ = kNoCandidate;         // 当前候选下标
    uint8_t tried_mask_ = 0;                // 当前起点已拒绝过的候选位图
    bool in_callback_ = false;              // 防重入
    Stats stats_{};

    // 实现细节全部放在 demux.cpp 中定义的 Engine（静态成员函数集合），以免冻结头随实现变化。
    friend struct Engine;
};

} // namespace demux
