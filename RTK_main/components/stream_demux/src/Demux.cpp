// 平台无关的分流引擎；原地回放与输入累积共用调用方提供的缓冲区。
// feed/abort 由持有该实例的唯一任务调用，回调在调用内同步执行；不阻塞、无堆，ISR 安全。
#include "demux/Demux.hpp"

#include <algorithm>
#include <cstring>

namespace demux {

/**
 * @brief  Demux 的实现细节：静态成员函数集合，直接读写 Demux 的私有状态
 * @note   放在 .cpp 中是为了让冻结头 demux/Demux.hpp 不随实现变化；本结构没有实例。
 */
struct Engine {
    /**
     * @brief  校验 init 参数：表非空且不超 kMaxDecoders，每个描述符字段有效且 maxFrame 不超过缓冲区
     * @param  decoders: 解码器表（含 nullptr 即判无效）
     * @param  buf: 待用的缓冲区
     * @retval true 参数可用；false 不合规（调用方据此返回 kInvalidArg）
     * @note   纯函数，不修改任何状态；不阻塞。
     */
    static bool configValid(std::span<const Decoder *const> decoders,
                            std::span<uint8_t> buf) noexcept
    {
        if (decoders.empty() || decoders.size() > kMaxDecoders) {
            return false;
        }
        constexpr std::size_t kMaxFrame = 65535;
        for (const Decoder *decoder : decoders) {
            if (decoder == nullptr || decoder->name == nullptr || decoder->firstByte == nullptr ||
                decoder->step == nullptr || decoder->maxFrame == 0 ||
                decoder->maxFrame > kMaxFrame || decoder->maxFrame > buf.size()) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief  把解码器下标转成首字节表与已试集合里的位掩码
     * @param  index: 解码器下标（< kMaxDecoders）
     * @retval 1 << index 的 8 位形式
     */
    static uint8_t decoderBit(uint8_t index) noexcept
    {
        // index < kMaxDecoders == 8，单个位最多为 0x80，收窄无损。
        return static_cast<uint8_t>(1U << index);
    }

    /**
     * @brief  找该首字节下第一个尚未试过的候选解码器
     * @param  dmx: 引擎状态，只读 first_mask_ 与 tried_mask_
     * @param  first: 起点字节，用作 first_mask_ 的下标
     * @retval 候选下标；无候选时返回 Demux::kNoCandidate
     * @note   下标升序即 init 时的优先级；只读，不改变状态。
     */
    static uint8_t nextDecoder(const Demux &dmx, uint8_t first) noexcept
    {
        for (uint8_t index = 0; index < dmx.decoder_count_; ++index) {
            const uint8_t bit = decoderBit(index);
            if ((dmx.first_mask_[first] & bit) != 0U && (dmx.tried_mask_ & bit) == 0U) {
                return index;
            }
        }
        return Demux::kNoCandidate;
    }

    /**
     * @brief  回到空闲：清掉当前候选、已试集合与待补字节数
     * @param  dmx: 目标实例
     * @note   不动 len_ 与统计；调用方负责缓冲区内容。
     */
    static void idle(Demux &dmx) noexcept
    {
        dmx.active_ = Demux::kNoCandidate;
        dmx.tried_mask_ = 0;
        dmx.need_ = 0;
    }

    /**
     * @brief  统计并回调一段未知字节
     * @param  dmx: 目标实例
     * @param  bytes: 未知字节；为空时既不计数也不回调
     * @note   onUnknown 期间 in_callback_ 为真，回调中重入 init/feed/abort 得 kReentry。
     */
    static void unknown(Demux &dmx, std::span<const uint8_t> bytes) noexcept
    {
        if (!bytes.empty()) {
            // 先取低 32 位，值不超过 UINT32_MAX；统计按 uint32_t 模 2^32 累加。
            dmx.stats_.unknownBytes += static_cast<uint32_t>(bytes.size() & UINT32_MAX);
            dmx.in_callback_ = true;
            dmx.sink_->onUnknown(bytes);
            dmx.in_callback_ = false;
        }
    }

    /**
     * @brief  统计并回调一条已判定的完整帧（调用前须已确定 dmx.active_）
     * @param  dmx: 目标实例
     * @param  bytes: 整条帧字节，调用方保证非空
     * @note   onFrame 期间 in_callback_ 为真；回调返回后调用方才移动缓冲区内容，
     *         因此回调里看到的未知字节与帧的顺序稳定。
     */
    static void frame(Demux &dmx, std::span<const uint8_t> bytes) noexcept
    {
        DecoderStats &stats = dmx.stats_.decoder[dmx.active_];
        ++stats.frames;
        // 前缀长度 <= 当前解码器 maxFrame <= 65535，转换到 uint32_t 无损。
        stats.bytes += static_cast<uint32_t>(bytes.size());
        dmx.in_callback_ = true;
        dmx.sink_->onFrame(*dmx.decoders_[dmx.active_], dmx.active_, bytes);
        dmx.in_callback_ = false;
    }

    /**
     * @brief  回调 onReject（可选观察钩子）
     * @param  dmx: 目标实例
     * @param  index: 被拒绝的候选下标
     * @param  prefix: 被拒绝的前缀；为空时不回调（正常路径至少 1 字节）
     * @param  cause: 拒绝来源
     * @param  last: true 表示该起点已无候选，调用方随后前移一字节重扫
     * @note   onReject 期间 in_callback_ 为真，与 onFrame/onUnknown 的重入约束一致。
     */
    static void reject(Demux &dmx, uint8_t index, std::span<const uint8_t> prefix,
                       RejectCause cause, bool last) noexcept
    {
        if (prefix.empty()) {
            return;
        }
        dmx.in_callback_ = true;
        dmx.sink_->onReject(*dmx.decoders_[index], index, prefix, cause, last);
        dmx.in_callback_ = false;
    }

    /**
     * @brief  从缓冲区头部丢弃 count 字节，并把剩余内容前移到开头
     * @param  dmx: 目标实例
     * @param  count: 已输出的字节数，须 ≤ dmx.len_；不修改 active_/need_
     * @note   剩余为 0 时不搬动缓冲区。
     */
    static void consume(Demux &dmx, std::size_t count) noexcept
    {
        const std::span<const uint8_t> buffered{dmx.buf_, dmx.len_};
        dmx.len_ -= count;
        if (dmx.len_ != 0) {
            std::memmove(dmx.buf_, buffered.subspan(count).data(), dmx.len_);
        }
    }

    // NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
    /**
     * @brief  把已缓冲字节就地回放给解码器，直到只剩一个待补全的候选（或全部输出完毕）
     * @param  dmx: 目标实例
     * @param  aborting: 为真时半帧一律进入拒绝路径（abort() 用）
     * @note   start 之前是待合并输出的未知字节；prefix 按 need 递增，避免跳过缓冲区内部的帧尾。
     *         返回时只保留一个尚待补全的候选（或回到空闲）。回调顺序固定：先 onUnknown 再 onFrame；
     *         拒绝时先更新候选集合与统计，再 onReject。
     * @note   不拆（ADR-0013 §2.4）：就地回放状态机，start/prefix/rejectCurrent 与候选、need、stats
     *         交织，拆出需 4 个以上出入参且回调顺序敏感。
     */
    static void replay(Demux &dmx, bool aborting) noexcept
    {
        std::size_t start = 0;
        std::size_t prefix = dmx.len_;
        bool rejectCurrent = aborting && dmx.active_ != Demux::kNoCandidate;
        while (start < dmx.len_) {
            const std::span<const uint8_t> buffered{dmx.buf_, dmx.len_};
            if (dmx.active_ == Demux::kNoCandidate) {
                while (start < dmx.len_ && dmx.first_mask_[buffered[start]] == 0U) {
                    ++start;
                }
                if (start == dmx.len_) {
                    break;
                }
                dmx.tried_mask_ = 0;
                dmx.active_ = nextDecoder(dmx, buffered[start]);
                prefix = 1;
            }

            const Decoder &decoder = *dmx.decoders_[dmx.active_];
            const bool forced = rejectCurrent;
            std::size_t need = 0;
            const Step step =
                forced ? Step::kReject : decoder.step(buffered.subspan(start, prefix), need);
            rejectCurrent = false;
            if (step == Step::kFrame) {
                // 两次回调结束后才移动 buf，保证未知字节与帧的内容及输出顺序稳定。
                unknown(dmx, buffered.first(start));
                frame(dmx, buffered.subspan(start, prefix));
                consume(dmx, start + prefix);
                idle(dmx);
                start = 0;
            } else {
                bool overflow = false;
                if (step == Step::kNeedMore) {
                    // prefix 已受此前 need 校验约束；用减法避免恶意 need == SIZE_MAX 时加法回绕。
                    overflow = need > dmx.buf_size_ - prefix;
                    if (overflow) {
                        ++dmx.stats_.overflows;
                    }
                    if (need != 0 && need <= decoder.maxFrame - prefix && !overflow) {
                        const std::size_t remaining = dmx.len_ - start - prefix;
                        if (need <= remaining) {
                            prefix += need;
                            continue;
                        }
                        if (!aborting) {
                            dmx.need_ = need - remaining;
                            unknown(dmx, buffered.first(start));
                            consume(dmx, start);
                            return;
                        }
                    }
                }

                // 未知 step 枚举值及 NEED_MORE 契约违规也按拒绝处理，候选集合严格缩小。
                const uint8_t rejected = dmx.active_;
                RejectCause cause = RejectCause::kStep;
                if (forced) {
                    cause = RejectCause::kAbort;
                } else if (step == Step::kNeedMore) {
                    // 三种来源：超出缓冲区容量（overflows 计数）、契约违规、abort() 时数据不足。
                    if (overflow) {
                        cause = RejectCause::kTooLong;
                    } else if ((need == 0) || (need > decoder.maxFrame - prefix)) {
                        cause = RejectCause::kContract;
                    } else {
                        cause = RejectCause::kAbort;
                    }
                } else if (step != Step::kReject) {
                    cause = RejectCause::kContract;
                }
                ++dmx.stats_.decoder[rejected].rejects;
                dmx.tried_mask_ |= decoderBit(rejected);
                dmx.active_ = nextDecoder(dmx, buffered[start]);
                const bool last = dmx.active_ == Demux::kNoCandidate;
                reject(dmx, rejected, buffered.subspan(start, prefix), cause, last);
                if (last) {
                    idle(dmx);
                    ++dmx.stats_.resyncs;
                    ++start;
                } else {
                    prefix = 1;
                }
            }
        }
        unknown(dmx, {dmx.buf_, dmx.len_});
        dmx.len_ = 0;
        idle(dmx);
    }
    // NOLINTEND(readability-function-size, readability-function-cognitive-complexity)
};

/**
 * @brief  初始化引擎：校验参数、建立首字节位图、清空状态与统计（完整契约见 Demux.hpp）
 * @param  decoders: 解码器表，元素不得为空指针，长度 1..kMaxDecoders
 * @param  buf: 工作缓冲区，容量须 ≥ 表中最大的 maxFrame
 * @param  sink: 分流结果接收方，生命周期须覆盖本对象；校验失败时不被采用
 * @retval kOk 初始化完成，可以 feed
 * @retval kInvalidArg 参数不合规，状态已复位为未初始化
 * @retval kReentry 在本实例回调中调用，状态不变
 * @note   线程安全：否；通常只在启动期调用，建位图的代价为 O(解码器数 × 256)。
 */
Error Demux::init(std::span<const Decoder *const> decoders, std::span<uint8_t> buf,
                  Sink &sink) noexcept
{
    if (in_callback_) {
        return Error::kReentry;
    }
    decoder_count_ = 0;
    decoders_.fill(nullptr);
    buf_ = nullptr;
    buf_size_ = 0;
    sink_ = nullptr;
    first_mask_.fill(0);
    len_ = 0;
    Engine::idle(*this);
    stats_ = Stats{};
    if (!Engine::configValid(decoders, buf)) {
        return Error::kInvalidArg;
    }

    // 参数校验保证数量在 1..kMaxDecoders == 8，转换到 uint8_t 无损。
    decoder_count_ = static_cast<uint8_t>(decoders.size());
    std::copy(decoders.begin(), decoders.end(), decoders_.begin());
    buf_ = buf.data();
    buf_size_ = buf.size();
    sink_ = &sink;
    for (uint8_t index = 0; index < decoder_count_; ++index) {
        const uint8_t bit = Engine::decoderBit(index);
        for (std::size_t byte = 0; byte < first_mask_.size(); ++byte) {
            // 循环范围为 0..255，转换到 uint8_t 无损。
            if (decoders_[index]->firstByte(static_cast<uint8_t>(byte))) {
                first_mask_[byte] |= bit;
            }
        }
    }
    return Error::kOk;
}

Error Demux::feed(std::span<const uint8_t> data) noexcept
{
    if (in_callback_) {
        return Error::kReentry;
    }
    if (decoder_count_ == 0) {
        return Error::kInvalidArg;
    }

    std::size_t offset = 0;
    while (offset < data.size()) {
        if (active_ == kNoCandidate) {
            const std::size_t begin = offset;
            while (offset < data.size() && first_mask_[data[offset]] == 0U) {
                ++offset;
            }
            Engine::unknown(*this, data.subspan(begin, offset - begin));
            if (offset == data.size()) {
                break;
            }
            std::span{buf_, buf_size_}[0] = data[offset];
            len_ = 1;
            ++offset;
            Engine::replay(*this, false);
        } else {
            const std::size_t count = std::min(data.size() - offset, need_);
            std::memcpy(std::span{buf_, buf_size_}.subspan(len_).data(),
                        data.subspan(offset).data(), count);
            len_ += count;
            need_ -= count;
            offset += count;
            if (need_ == 0) {
                Engine::replay(*this, false);
            }
        }
    }
    return Error::kOk;
}

Error Demux::abort() noexcept
{
    if (in_callback_) {
        return Error::kReentry;
    }
    if (decoder_count_ == 0) {
        return Error::kInvalidArg;
    }
    if (len_ != 0) {
        ++stats_.aborts;
        // 保留当前起点已拒绝的集合，直接拒绝待补全候选，再尝试其余候选与后续起点。
        Engine::replay(*this, true);
    }
    return Error::kOk;
}

} // namespace demux
