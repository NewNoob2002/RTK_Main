// 通用多协议字节流分流器（C++20，平台无关；冻结接口 ADR-0005）：一条字节流一个实例，按 Decoder 描述符找帧起点、
// 累积、失败回放重扫并同步分发回调；缓冲区由调用方提供（StaticDemux 编译期定长）。
// 线程安全：否（一个实例只由一个任务使用）；ISR 安全（无阻塞、无 OS 调用，回调在 feed/abort 内同步执行）；无堆/异常/RTTI/浮点。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "demux/Decoder.hpp"

namespace demux {

/** 一个实例最多挂载的解码器数（首字节表用 uint8_t 位图）。 */
inline constexpr std::size_t kMaxDecoders = 8;

/** onReject 的拒绝来源。 */
enum class RejectCause : uint8_t {
    kStep = 0, // step() 返回 kReject（同步/长度/校验/非法字符；细节由 Decoder::explain 给出）
    kTooLong,  // step() 要求的长度超过 maxFrame 或缓冲区容量（overflows 计数的那种）
    kContract, // step() 违反契约（need == 0、未知枚举值）
    kAbort,    // abort() 时丢弃的半帧
};

/**
 * @brief  分流结果的接收方，由调用方实现（建议 final）
 * @note   运行期多态边界：受保护非虚析构，不能经基类指针销毁（CPP-011，ADR-0005 §2 授权的第二个多态边界）。
 *         生命周期须覆盖所挂载的 Demux。三个回调都在 feed()/abort() 内同步执行，数据 span 只在回调返回前
 *         有效；回调中对同一实例调用 init/feed/abort 返回 Error::kReentry 且不改变状态。
 */
class Sink {
public:
    Sink(const Sink &) = delete;
    Sink &operator=(const Sink &) = delete;

    /**
     * @brief  回调一条完整帧
     * @param  decoder: 产出该帧的解码器描述符（地址可作协议身份）
     * @param  index: 该解码器在 init 时解码器表中的下标
     * @param  frame: 整条帧字节，只在回调返回前有效
     * @note   在 Demux::feed/abort 内同步调用；不得在回调中重入同一实例。
     */
    virtual void onFrame(const Decoder &decoder, uint8_t index,
                         std::span<const uint8_t> frame) noexcept = 0;

    /**
     * @brief  回调一段不属于任何已挂载协议的字节
     * @param  bytes: 未知字节，按流顺序；同一段连续未知字节可能分多次回调，只在回调返回前有效
     * @note   默认实现丢弃（统计照常计数）；重入约束同 onFrame。
     */
    virtual void onUnknown(std::span<const uint8_t> bytes) noexcept
    {
        static_cast<void>(bytes);
    }

    /**
     * @brief  回调“某个候选解码器放弃了当前起点”（ADR-0005 §5 的可选观察钩子，默认空操作）
     * @param  decoder: 放弃的候选解码器描述符
     * @param  index: 该解码器在 init 时解码器表中的下标
     * @param  prefix: 被拒绝的那段前缀（以起点为首字节），只在回调返回前有效
     * @param  cause: 拒绝来源（见 RejectCause）
     * @param  last: true 表示该起点已无其他候选，随后起点前移一字节重扫（resyncs+1）
     * @note   重入约束同 onFrame；实时路径的 Sink 应保持空操作或只记 trace() 事件。
     */
    virtual void onReject(const Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                          RejectCause cause, bool last) noexcept
    {
        static_cast<void>(decoder);
        static_cast<void>(index);
        static_cast<void>(prefix);
        static_cast<void>(cause);
        static_cast<void>(last);
    }

protected:
    constexpr Sink() = default;
    ~Sink() = default;
};

/** init/feed/abort 的返回码。 */
enum class Error : uint8_t {
    kOk = 0,
    kInvalidArg, // 解码器表为空/超过 kMaxDecoders/含空指针或无效描述符、缓冲区小于最大 maxFrame、尚未 init 成功
    kReentry, // 在本实例回调中调用了 init/feed/abort
};

/** 单个解码器的累计统计。 */
struct DecoderStats {
    uint32_t frames = 0;  // 成功帧数
    uint32_t rejects = 0; // 放弃该候选的次数（kStep/kTooLong/kContract/kAbort 都计入）
    uint32_t bytes = 0;   // 成功帧总字节数
};

/** 分流器整体统计（含未知字节与重扫次数）。 */
struct Stats {
    std::array<DecoderStats, kMaxDecoders> decoder{};
    uint32_t unknownBytes = 0; // 交给 onUnknown 的字节数
    uint32_t resyncs = 0;      // 所有候选都拒绝后起点前移重扫的次数
    uint32_t overflows = 0;    // 解码器要求的长度超过缓冲区容量而被强制拒绝的次数
    uint32_t aborts = 0;       // abort() 时存在半帧的次数
};

/**
 * @brief  分流引擎：找帧起点、累积字节、失败回放重扫、回调分发与统计
 * @note   constexpr 构造后处于“未初始化”状态（feed/abort 返回 kInvalidArg），可 constinit 定义为全局对象；
 *         独占缓冲区，禁止拷贝与移动。线程安全：否（同一实例只能由一个任务使用）。
 */
class Demux {
public:
    constexpr Demux() noexcept = default;
    Demux(const Demux &) = delete;
    Demux &operator=(const Demux &) = delete;
    Demux(Demux &&) = delete;
    Demux &operator=(Demux &&) = delete;
    ~Demux() = default;

    /**
     * @brief  初始化（或重新初始化）：校验参数、建立 256 项首字节位图、清空状态与统计
     * @param  decoders: 解码器表，下标越小优先级越高（同首字节时先尝试）
     * @param  buf: 累积与回放共用的缓冲区，至少容纳表内最大的 maxFrame
     * @param  sink: 分流结果接收方
     * @retval kOk 初始化完成，可以 feed
     * @retval kInvalidArg 表为空/超过 kMaxDecoders/含空指针或无效描述符，或缓冲区小于某个 maxFrame；
     *         失败时实例回到未初始化状态
     * @retval kReentry 在本实例回调中调用，状态不变
     * @note   表本身、描述符、buf、sink 必须在实例生命周期内有效；线程安全：否。
     */
    [[nodiscard]] Error init(std::span<const Decoder *const> decoders, std::span<uint8_t> buf,
                             Sink &sink) noexcept;

    /**
     * @brief  喂入任意长度字节（可为空），本调用内同步完成所有能完成的回调
     * @param  data: 新到的字节；只在调用期间读取
     * @retval kOk 全部字节已处理（含全部作为未知字节输出）
     * @retval kInvalidArg 尚未 init 成功
     * @retval kReentry 在本实例回调中调用，状态不变
     * @note   分块无关（ADR-0004 §3 I1）：同一字节序列任意切分喂入，帧回调序列相同，未知字节拼接后相同。
     */
    [[nodiscard]] Error feed(std::span<const uint8_t> data) noexcept;

    /**
     * @brief  放弃当前半帧（流空闲超时、断链）：按 kAbort 回放重扫，剩余字节作为未知字节输出，回到空闲
     * @retval kOk 已复位（本来空闲时也返回 kOk，只是不计数）
     * @retval kInvalidArg 尚未 init 成功
     * @retval kReentry 在本实例回调中调用，状态不变
     * @note   拒绝后仍按流顺序回放，丢弃的半帧计入 stats().aborts。
     */
    [[nodiscard]] Error abort() noexcept;

    /**
     * @brief  是否有半帧在累积
     * @retval true 有半帧（调用方据此决定是否启动空闲超时）；false 空闲
     */
    [[nodiscard]] bool busy() const noexcept
    {
        return len_ != 0;
    }

    /**
     * @brief  读取累计统计
     * @retval 统计表的只读引用；下一次 init() 前一直有效
     * @note   在回调里读取安全（不重入）。
     */
    [[nodiscard]] const Stats &stats() const noexcept
    {
        return stats_;
    }

    /**
     * @brief  清零统计
     * @note   不影响解析状态；在回调中调用也安全。
     */
    void resetStats() noexcept
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

    // 实现细节全部放在 Demux.cpp 中定义的 Engine（静态成员函数集合），以免冻结头随实现变化。
    friend struct Engine;
};

} // namespace demux
