// 平台无关的分流引擎；原地回放与输入累积共用调用方提供的缓冲区。
#include "demux/demux.hpp"

#include <algorithm>
#include <cstring>

namespace demux {

struct Engine {
    static bool config_valid(std::span<const Decoder *const> decoders, std::span<uint8_t> buf) noexcept
    {
        if (decoders.empty() || decoders.size() > kMaxDecoders) {
            return false;
        }
        constexpr std::size_t kMaxFrame = 65535;
        for (const Decoder *decoder : decoders) {
            if (decoder == nullptr || decoder->name == nullptr || decoder->first_byte == nullptr ||
                decoder->step == nullptr || decoder->max_frame == 0 || decoder->max_frame > kMaxFrame ||
                decoder->max_frame > buf.size()) {
                return false;
            }
        }
        return true;
    }

    static uint8_t decoder_bit(uint8_t index) noexcept
    {
        // index < kMaxDecoders == 8，单个位最多为 0x80，收窄无损。
        return static_cast<uint8_t>(1U << index);
    }

    static uint8_t next_decoder(const Demux &dmx, uint8_t first) noexcept
    {
        for (uint8_t index = 0; index < dmx.decoder_count_; ++index) {
            const uint8_t bit = decoder_bit(index);
            if ((dmx.first_mask_[first] & bit) != 0U && (dmx.tried_mask_ & bit) == 0U) {
                return index;
            }
        }
        return Demux::kNoCandidate;
    }

    static void idle(Demux &dmx) noexcept
    {
        dmx.active_ = Demux::kNoCandidate;
        dmx.tried_mask_ = 0;
        dmx.need_ = 0;
    }

    static void unknown(Demux &dmx, std::span<const uint8_t> bytes) noexcept
    {
        if (!bytes.empty()) {
            // 先取低 32 位，值不超过 UINT32_MAX；统计按 uint32_t 模 2^32 累加。
            dmx.stats_.unknown_bytes += static_cast<uint32_t>(bytes.size() & UINT32_MAX);
            dmx.in_callback_ = true;
            dmx.sink_->on_unknown(bytes);
            dmx.in_callback_ = false;
        }
    }

    static void frame(Demux &dmx, std::span<const uint8_t> bytes) noexcept
    {
        DecoderStats &stats = dmx.stats_.decoder[dmx.active_];
        ++stats.frames;
        // 前缀长度 <= 当前解码器 max_frame <= 65535，转换到 uint32_t 无损。
        stats.bytes += static_cast<uint32_t>(bytes.size());
        dmx.in_callback_ = true;
        dmx.sink_->on_frame(*dmx.decoders_[dmx.active_], dmx.active_, bytes);
        dmx.in_callback_ = false;
    }

    // 拒绝钩子：回调期间置 in_callback_，与 on_frame/on_unknown 的重入约束一致。
    static void reject(Demux &dmx, uint8_t index, std::span<const uint8_t> prefix, RejectCause cause,
                       bool last) noexcept
    {
        if (prefix.empty()) {
            return;
        }
        dmx.in_callback_ = true;
        dmx.sink_->on_reject(*dmx.decoders_[index], index, prefix, cause, last);
        dmx.in_callback_ = false;
    }

    static void consume(Demux &dmx, std::size_t count) noexcept
    {
        const std::span<const uint8_t> buffered{dmx.buf_, dmx.len_};
        dmx.len_ -= count;
        if (dmx.len_ != 0) {
            std::memmove(dmx.buf_, buffered.subspan(count).data(), dmx.len_);
        }
    }

    // start 前是待合并输出的未知字节；prefix 按 need 递增，避免跳过缓冲区内部的帧尾。
    // 返回时只保留一个尚待补全的候选；aborting 时半帧一律进入拒绝路径。
    static void replay(Demux &dmx, bool aborting) noexcept
    {
        std::size_t start = 0;
        std::size_t prefix = dmx.len_;
        bool reject_current = aborting && dmx.active_ != Demux::kNoCandidate;
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
                dmx.active_ = next_decoder(dmx, buffered[start]);
                prefix = 1;
            }

            const Decoder &decoder = *dmx.decoders_[dmx.active_];
            const bool forced = reject_current;
            std::size_t need = 0;
            const Step step = forced ? Step::kReject : decoder.step(buffered.subspan(start, prefix), need);
            reject_current = false;
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
                    if (need != 0 && need <= decoder.max_frame - prefix && !overflow) {
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
                    } else if ((need == 0) || (need > decoder.max_frame - prefix)) {
                        cause = RejectCause::kContract;
                    } else {
                        cause = RejectCause::kAbort;
                    }
                } else if (step != Step::kReject) {
                    cause = RejectCause::kContract;
                }
                ++dmx.stats_.decoder[rejected].rejects;
                dmx.tried_mask_ |= decoder_bit(rejected);
                dmx.active_ = next_decoder(dmx, buffered[start]);
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
};

// 线程安全：同一实例须串行访问；ISR 安全，first_byte 必须满足冻结的纯函数契约。
Error Demux::init(std::span<const Decoder *const> decoders, std::span<uint8_t> buf, Sink &sink) noexcept
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
    if (!Engine::config_valid(decoders, buf)) {
        return Error::kInvalidArg;
    }

    // 参数校验保证数量在 1..kMaxDecoders == 8，转换到 uint8_t 无损。
    decoder_count_ = static_cast<uint8_t>(decoders.size());
    std::copy(decoders.begin(), decoders.end(), decoders_.begin());
    buf_ = buf.data();
    buf_size_ = buf.size();
    sink_ = &sink;
    for (uint8_t index = 0; index < decoder_count_; ++index) {
        const uint8_t bit = Engine::decoder_bit(index);
        for (std::size_t byte = 0; byte < first_mask_.size(); ++byte) {
            // 循环范围为 0..255，转换到 uint8_t 无损。
            if (decoders_[index]->first_byte(static_cast<uint8_t>(byte))) {
                first_mask_[byte] |= bit;
            }
        }
    }
    return Error::kOk;
}

// 线程安全：同一实例须串行访问；ISR 安全取决于同步执行的用户回调；回调内禁止重入。
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
            std::memcpy(std::span{buf_, buf_size_}.subspan(len_).data(), data.subspan(offset).data(), count);
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

// 线程安全：同一实例须串行访问；ISR 安全取决于用户回调；拒绝半帧后仍按流顺序回放。
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
