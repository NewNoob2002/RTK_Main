// 通用字节流分流器引擎黑盒单元测试（ADR-0005 / ADR-0004 §3，状态机、StaticDemux、constinit 与边界）。

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "demux/Demux.hpp"
#include "demux/StaticDemux.hpp"
#include "unity.h"

namespace {

constexpr std::size_t kTestBufCap = 128U;
constexpr std::size_t kTestCanaryLen = 16U;
constexpr uint8_t kTestCanaryByte = 0xA5U;
constexpr std::size_t kMaxEvents = 64U;
constexpr std::size_t kMaxStreamCap = 512U;

enum class EventKind : uint8_t {
    kUnknown = 1,
    kFrame = 2,
    kReject = 3,
};

struct RecordedEvent {
    EventKind kind = EventKind::kUnknown;
    uint8_t decoder_index = 0U;
    const demux::Decoder *decoder = nullptr;
    std::size_t len = 0U;
    std::array<uint8_t, kTestBufCap> data{};
    demux::RejectCause cause = demux::RejectCause::kStep;
    bool last = false;
};

/** explain 回调 spy：记录调用次数与最后一次拿到的前缀。 */
uint32_t s_explain_calls = 0U;
std::size_t s_explain_prefix_len = 0U;

std::size_t spy_explain(std::span<const uint8_t> prefix, std::span<char> out) noexcept
{
    s_explain_calls++;
    s_explain_prefix_len = prefix.size();
    static constexpr std::string_view kText{"spy says no"};
    if (out.size() > kText.size()) {
        std::copy(kText.begin(), kText.end(), out.begin());
        out[kText.size()] = '\0';
        return kText.size();
    }
    return 0U;
}

// --------------------------------------------------------------------------
// 自定义测试解码器（纯函数，无状态）
// --------------------------------------------------------------------------

// 1) 固定 4 字节帧：A1 <d1> <d2> <chk>，其中 chk == (A1 ^ d1 ^ d2)
bool dec_fixed4_first(uint8_t byte) noexcept
{
    return byte == 0xA1U;
}

demux::Step dec_fixed4_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 4U) {
        need = 4U - frame.size();
        return demux::Step::kNeedMore;
    }
    const auto expected =
        static_cast<uint8_t>(static_cast<unsigned int>(frame[0]) ^ static_cast<unsigned int>(frame[1]) ^
                             static_cast<unsigned int>(frame[2])); // 3 个 uint8_t 异或结果在 0..255
    return (expected == frame[3]) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecFixed4{"fixed4", 4U, &dec_fixed4_first, &dec_fixed4_step};

// 2) 批量 spy 解码器：A2 <total_len> <payload...> <0xEE>；统计 step 调用次数
uint32_t s_bulk_step_calls = 0U;

bool dec_bulk_first(uint8_t byte) noexcept
{
    return byte == 0xA2U;
}

demux::Step dec_bulk_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    s_bulk_step_calls++;
    if (frame.size() < 2U) {
        need = 2U - frame.size();
        return demux::Step::kNeedMore;
    }
    const std::size_t total = frame[1];
    if ((total < 3U) || (total > 64U)) {
        return demux::Step::kReject;
    }
    if (frame.size() < total) {
        need = total - frame.size();
        return demux::Step::kNeedMore;
    }
    return (frame[total - 1U] == 0xEEU) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecBulk{"bulk64", 64U, &dec_bulk_first, &dec_bulk_step};

// 3) 同首字节冲突组（均以 0xAA 开头）：
//    - p0: 6 字节，要求 frame[5] == 0x55（前 5 字节不拒，凑满 6 字节后才可能拒，用于触发长前缀回放）
//    - p1: 4 字节，要求 frame[1] == 0x22 && frame[3] == 0x44
//    - p2: 3 字节，要求 frame[1] == 0x33 && frame[2] == 0x66
bool dec_aa_first(uint8_t byte) noexcept
{
    return byte == 0xAAU;
}

demux::Step dec_aa_p0_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 6U) {
        need = 6U - frame.size();
        return demux::Step::kNeedMore;
    }
    return (frame[5] == 0x55U) ? demux::Step::kFrame : demux::Step::kReject;
}

demux::Step dec_aa_p1_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if ((frame.size() >= 2U) && (frame[1] != 0x22U)) {
        return demux::Step::kReject;
    }
    if (frame.size() < 4U) {
        need = 4U - frame.size();
        return demux::Step::kNeedMore;
    }
    return (frame[3] == 0x44U) ? demux::Step::kFrame : demux::Step::kReject;
}

demux::Step dec_aa_p2_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if ((frame.size() >= 2U) && (frame[1] != 0x33U)) {
        return demux::Step::kReject;
    }
    if (frame.size() < 3U) {
        need = 3U - frame.size();
        return demux::Step::kNeedMore;
    }
    return (frame[2] == 0x66U) ? demux::Step::kFrame : demux::Step::kReject;
}

// 立即拒绝：用于验证同一首字节多候选时的 last 标志（前一候选 false、末位候选 true）。
demux::Step dec_aa_reject_now_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = 0U;
    return demux::Step::kReject;
}

constexpr demux::Decoder kDecAaNo0{"aa_no0", 4U, &dec_aa_first, &dec_aa_reject_now_step};
constexpr demux::Decoder kDecAaNo1{"aa_no1", 4U, &dec_aa_first, &dec_aa_reject_now_step};
constexpr demux::Decoder kDecAaP0{"aa_p0", 6U, &dec_aa_first, &dec_aa_p0_step};
constexpr demux::Decoder kDecAaP1{"aa_p1", 4U, &dec_aa_first, &dec_aa_p1_step};
constexpr demux::Decoder kDecAaP2{"aa_p2", 3U, &dec_aa_first, &dec_aa_p2_step};

// 4) 契约违规与溢出解码器（均以 0xBA 开头）
bool dec_ba_first(uint8_t byte) noexcept
{
    return byte == 0xBAU;
}

demux::Step dec_need_zero_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = 0U; // 违规：kNeedMore 时 need == 0
    return demux::Step::kNeedMore;
}

demux::Step dec_exceed_max_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    need = 17U - frame.size(); // max_frame == 16，17 超出 max_frame
    return demux::Step::kNeedMore;
}

demux::Step dec_overflow_buf_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = 20U; // 当 buf_size == 16 时，1 + 20 > 16
    return demux::Step::kNeedMore;
}

demux::Step dec_overflow_sizemax_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = std::numeric_limits<std::size_t>::max(); // 测试无符号加法回绕溢出
    return demux::Step::kNeedMore;
}

demux::Step dec_ba_valid2_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 2U) {
        need = 1U;
        return demux::Step::kNeedMore;
    }
    return (frame[1] == 0x77U) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecNeedZero{"bad_need0", 16U, &dec_ba_first, &dec_need_zero_step};
constexpr demux::Decoder kDecExceedMax{"bad_max", 16U, &dec_ba_first, &dec_exceed_max_step};
constexpr demux::Decoder kDecOverflowBuf{"bad_ovf", 16U, &dec_ba_first, &dec_overflow_buf_step};
constexpr demux::Decoder kDecOverflowSizeMax{"bad_smax", 16U, &dec_ba_first, &dec_overflow_sizemax_step};
constexpr demux::Decoder kDecBaValid2{"ba_ok2", 2U, &dec_ba_first, &dec_ba_valid2_step};

// 5) 8 个解码器满配测试（首字节 0xC0..0xC7，每帧 2 字节：0xCx <~0xCx>）
template <uint8_t Tag> bool dec_slot_first(uint8_t byte) noexcept
{
    return byte == Tag;
}

demux::Step dec_slot_step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 2U) {
        need = 1U;
        return demux::Step::kNeedMore;
    }
    // 0xFFU 与 uint8_t 异或结果为 0..255
    const auto expected = static_cast<uint8_t>(frame[0] ^ 0xFFU);
    return (frame[1] == expected) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecSlot0{"slot0", 2U, &dec_slot_first<0xC0U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot1{"slot1", 2U, &dec_slot_first<0xC1U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot2{"slot2", 2U, &dec_slot_first<0xC2U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot3{"slot3", 2U, &dec_slot_first<0xC3U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot4{"slot4", 2U, &dec_slot_first<0xC4U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot5{"slot5", 2U, &dec_slot_first<0xC5U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot6{"slot6", 2U, &dec_slot_first<0xC6U>, &dec_slot_step};
constexpr demux::Decoder kDecSlot7{"slot7", 2U, &dec_slot_first<0xC7U>, &dec_slot_step};

// --------------------------------------------------------------------------
// Sink 实现与全局测试对象
// --------------------------------------------------------------------------

class TestSink final : public demux::Sink {
public:
    void reset(demux::Demux *dmx) noexcept
    {
        dmx_ = dmx;
        event_count_ = 0U;
        unknown_len_ = 0U;
        test_reentry_in_frame_ = false;
        test_reentry_in_unknown_ = false;
        reset_stats_in_frame_ = false;
        reentry_feed_err_ = demux::Error::kOk;
        reentry_abort_err_ = demux::Error::kOk;
        reentry_init_err_ = demux::Error::kOk;
        reentry_decs_ = {};
        reentry_buf_ = {};
        reject_count_ = 0U;
    }

    void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> frame) noexcept override
    {
        static constexpr std::array<uint8_t, 1> kExtra{0x5AU};
        if (test_reentry_in_frame_ && (dmx_ != nullptr)) {
            reentry_feed_err_ = dmx_->feed(kExtra);
            reentry_abort_err_ = dmx_->abort();
            reentry_init_err_ = dmx_->init(reentry_decs_, reentry_buf_, *this);
        }
        if (reset_stats_in_frame_ && (dmx_ != nullptr)) {
            dmx_->reset_stats();
        }
        if (event_count_ < kMaxEvents) {
            RecordedEvent &ev = events_[event_count_++];
            ev.kind = EventKind::kFrame;
            ev.decoder_index = index;
            ev.decoder = &decoder;
            ev.len = frame.size();
            if (frame.size() <= ev.data.size()) {
                std::copy(frame.begin(), frame.end(), ev.data.begin());
            }
        }
    }

    void on_reject(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                   demux::RejectCause cause, bool last) noexcept override
    {
        if (reject_count_ < rejects_.size()) {
            RecordedEvent &ev = rejects_[reject_count_++];
            ev.kind = EventKind::kReject;
            ev.decoder_index = index;
            ev.decoder = &decoder;
            ev.len = prefix.size();
            ev.cause = cause;
            ev.last = last;
            if (prefix.size() <= ev.data.size()) {
                std::copy(prefix.begin(), prefix.end(), ev.data.begin());
            }
        }
    }

    void on_unknown(std::span<const uint8_t> bytes) noexcept override
    {
        static constexpr std::array<uint8_t, 1> kExtra{0x5AU};
        if (test_reentry_in_unknown_ && (dmx_ != nullptr)) {
            reentry_feed_err_ = dmx_->feed(kExtra);
            reentry_abort_err_ = dmx_->abort();
            reentry_init_err_ = dmx_->init(reentry_decs_, reentry_buf_, *this);
        }
        if (event_count_ < kMaxEvents) {
            RecordedEvent &ev = events_[event_count_++];
            ev.kind = EventKind::kUnknown;
            ev.decoder_index = 0U;
            ev.decoder = nullptr;
            ev.len = bytes.size();
            if (bytes.size() <= ev.data.size()) {
                std::copy(bytes.begin(), bytes.end(), ev.data.begin());
            }
        }
        if ((unknown_len_ + bytes.size()) <= unknown_stream_.size()) {
            std::copy(bytes.begin(), bytes.end(), unknown_stream_.begin() + static_cast<std::ptrdiff_t>(unknown_len_));
        }
        unknown_len_ += bytes.size();
    }

    demux::Demux *dmx_ = nullptr;
    std::array<RecordedEvent, kMaxEvents> events_{};
    std::size_t event_count_ = 0U;
    std::array<uint8_t, kMaxStreamCap> unknown_stream_{};
    std::size_t unknown_len_ = 0U;
    bool test_reentry_in_frame_ = false;
    bool test_reentry_in_unknown_ = false;
    bool reset_stats_in_frame_ = false;
    /** 拒绝事件单独存放，避免改变 events_ 的既有下标记号（其它用例按序号断言）。 */
    std::array<RecordedEvent, kMaxEvents> rejects_{};
    uint32_t reject_count_ = 0U;
    demux::Error reentry_feed_err_ = demux::Error::kOk;
    demux::Error reentry_abort_err_ = demux::Error::kOk;
    demux::Error reentry_init_err_ = demux::Error::kOk;
    std::span<const demux::Decoder *const> reentry_decs_{};
    std::span<uint8_t> reentry_buf_{};
};

// 仅实现 on_frame、继承默认 Sink::on_unknown 的极简 Sink
class MinimalSink final : public demux::Sink {
public:
    void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> frame) noexcept override
    {
        last_decoder_ = &decoder;
        last_index_ = index;
        last_len_ = frame.size();
        frame_count_++;
    }

    const demux::Decoder *last_decoder_ = nullptr;
    uint8_t last_index_ = 0U;
    std::size_t last_len_ = 0U;
    uint32_t frame_count_ = 0U;
};

// constinit 全局实例验证（CPP-020 / ADR-0005 §2）
constinit demux::Demux s_global_demux;
constinit demux::StaticDemux<kDecFixed4, kDecBulk> s_global_static_demux;

TestSink s_sink;
std::array<uint8_t, kTestBufCap + kTestCanaryLen> s_guarded_buf{};
demux::Demux s_dmx;

std::span<uint8_t> guarded_slice(std::size_t size) noexcept
{
    return std::span<uint8_t>(s_guarded_buf).subspan(0, size);
}

void reset_test_env() noexcept
{
    s_sink.reset(&s_dmx);
    s_guarded_buf.fill(kTestCanaryByte);
    s_bulk_step_calls = 0U;
}

} // namespace

extern "C" void setUp(void)
{
    reset_test_env();
}

extern "C" void tearDown(void) {}

// --------------------------------------------------------------------------
// 测试用例
// --------------------------------------------------------------------------

static void test_uninitialized_and_constinit_globals()
{
    static constexpr std::array<uint8_t, 4> kFrame{0xA1U, 0x10U, 0x20U, 0x91U};
    demux::Demux local_dmx;
    demux::StaticDemux<kDecFixed4> local_static;

    // 1) 未 init 状态下 feed（含空 span）与 abort 均返回 kInvalidArg
    TEST_ASSERT_FALSE(local_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, local_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, local_dmx.feed({}));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, local_dmx.abort());

    TEST_ASSERT_FALSE(local_static.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, local_static.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, local_static.feed({}));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, local_static.abort());

    // 2) constinit 全局实例初始处于未初始化状态，随后可正常 init 并解析
    TEST_ASSERT_FALSE(s_global_demux.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_demux.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_demux.abort());

    TEST_ASSERT_FALSE(s_global_static_demux.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_static_demux.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_static_demux.abort());

    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    s_sink.reset(&s_global_demux);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_demux.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_demux.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_global_demux.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events_[0].decoder);

    s_sink.reset(nullptr);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_static_demux.init(s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_static_demux.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_global_static_demux.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT8((demux::StaticDemux<kDecFixed4, kDecBulk>::index_of<kDecFixed4>()),
                            s_sink.events_[0].decoder_index);
}

static void test_init_validation_and_poison_on_fail()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecsOk{&kDecFixed4};
    static constexpr std::array<const demux::Decoder *, 2> kDecsNullItem{&kDecFixed4, nullptr};
    static constexpr std::array<const demux::Decoder *, 1> kDecsOnlyNull{nullptr};
    static constexpr std::array<const demux::Decoder *, 2> kDecsTwo{&kDecFixed4, &kDecBulk};
    static constexpr std::array<const demux::Decoder *, demux::kMaxDecoders + 1U> kDecsNine{
        &kDecSlot0, &kDecSlot1, &kDecSlot2, &kDecSlot3, &kDecSlot4, &kDecSlot5, &kDecSlot6, &kDecSlot7, &kDecFixed4,
    };
    static constexpr std::array<uint8_t, 4> kFrame{0xA1U, 0x10U, 0x20U, 0x91U};
    static std::array<uint8_t, 65536> s_huge_buf{};

    // 空表、超长表、含空指针
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init({}, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsNine, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsNullItem, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsOnlyNull, guarded_slice(kTestBufCap), s_sink));

    // 缓冲区为空或小于 max_frame（含后项 max_frame 更大）
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsOk, {}, s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsOk, guarded_slice(kDecFixed4.max_frame - 1U), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsTwo, guarded_slice(kDecBulk.max_frame - 1U), s_sink));

    // 解码器字段非法
    demux::Decoder bad_dec = kDecFixed4;
    const std::array<const demux::Decoder *, 1> bad_list{&bad_dec};

    bad_dec = kDecFixed4;
    bad_dec.name = nullptr;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(bad_list, guarded_slice(kTestBufCap), s_sink));

    bad_dec = kDecFixed4;
    bad_dec.max_frame = 0U;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(bad_list, guarded_slice(kTestBufCap), s_sink));

    bad_dec = kDecFixed4;
    bad_dec.max_frame = 65536U;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(bad_list, s_huge_buf, s_sink));

    bad_dec = kDecFixed4;
    bad_dec.first_byte = nullptr;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(bad_list, guarded_slice(kTestBufCap), s_sink));

    bad_dec = kDecFixed4;
    bad_dec.step = nullptr;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(bad_list, guarded_slice(kTestBufCap), s_sink));

    // 先成功 init，再用非法配置 re-init 失败后，实例必须回到未初始化状态（毒化）
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecsOk, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);

    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init({}, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.abort());

    // 重新有效 init 后恢复可用且统计已清空
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecsOk, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
}

static void test_feed_empty_span_and_zero_len()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 1> kDummy{0x00U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed({}));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kDummy.data(), 0U)));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(0U, s_sink.event_count_);
}

static void test_idle_unknown_and_default_on_unknown()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 8> kStream{0x11U, 0x22U, 0xA1U, 0x01U, 0x02U, 0xA2U, 0x33U, 0x44U};
    static constexpr std::array<uint8_t, 4> kUnk{0x11U, 0x22U, 0x33U, 0x44U};
    static constexpr std::array<uint8_t, 2> kHalf{0xA1U, 0x99U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kStream));
    TEST_ASSERT_FALSE(s_dmx.busy());

    // 流顺序：未知字节 {0x11,0x22} 在帧前，{0x33,0x44} 在帧后
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.event_count_);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[0].kind);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events_[1].kind);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events_[1].len);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events_[1].decoder);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[2].kind);
    TEST_ASSERT_EQUAL_UINT(kUnk.size(), s_sink.unknown_len_);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kUnk.data(), s_sink.unknown_stream_.data(), kUnk.size());

    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(4U, s_dmx.stats().decoder[0].bytes);
    TEST_ASSERT_EQUAL_UINT32(4U, s_dmx.stats().unknown_bytes);

    // 验证 Sink::on_unknown 默认实现（丢弃且统计照常计数）
    MinimalSink min_sink;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), min_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kStream));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kHalf));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_EQUAL_UINT32(1U, min_sink.frame_count_);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, min_sink.last_decoder_);
    TEST_ASSERT_EQUAL_UINT(4U, min_sink.last_len_);
    TEST_ASSERT_EQUAL_UINT32(6U, s_dmx.stats().unknown_bytes);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().aborts);
}

static void test_bulk_accumulation_and_split_chunks()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecBulk};
    std::array<uint8_t, 40> frame{};

    frame[0] = 0xA2U;
    frame[1] = 40U;
    for (std::size_t i = 2U; i < 39U; ++i) {
        // i < 39，在 uint8_t 范围内
        frame[i] = static_cast<uint8_t>(i);
    }
    frame[39] = 0xEEU;

    // 整块喂入：step 只应在 len=1, len=2, len=40 被调用 3 次
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(frame));
    TEST_ASSERT_EQUAL_UINT32(3U, s_bulk_step_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_PTR(&kDecBulk, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT(frame.size(), s_sink.events_[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame.data(), s_sink.events_[0].data.data(), frame.size());

    // 分块喂入（10 + 10 + 20），step 依然只在凑满 need 时触发（共 3 次）
    reset_test_env();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    const std::span<const uint8_t> fspan(frame);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(fspan.subspan(0, 10)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(0U, s_sink.event_count_);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(fspan.subspan(10, 10)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(fspan.subspan(20, 20)));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(3U, s_bulk_step_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count_);
}

static void test_same_first_byte_priority_and_midbuffer_replay()
{
    static constexpr std::array<const demux::Decoder *, 4> kDecs{&kDecAaP0, &kDecAaP1, &kDecAaP2, &kDecFixed4};
    static constexpr std::array<uint8_t, 6> kMatchP0{0xAAU, 0x22U, 0x00U, 0x44U, 0x00U, 0x55U};
    static constexpr std::array<uint8_t, 6> kMatchP1Unk{0xAAU, 0x22U, 0x99U, 0x44U, 0x77U, 0x88U};
    static constexpr std::array<uint8_t, 7> kMatchP2ThenFixed4{0xAAU, 0x33U, 0x66U, 0xA1U, 0x05U, 0x06U, 0xA2U};
    static constexpr std::array<uint8_t, 9> kMatchP2ThenP1{
        0xAAU, 0x33U, 0x66U, 0xAAU, 0x22U, 0x99U, 0x44U, 0x77U, 0x88U,
    };

    // 1) 命中 p0（优先级最高）
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP0));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT8(0U, s_sink.rejects_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP0, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[1].rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);

    // 2) p0 收满 6 字节拒 -> 回放 p1 在 p=4 成帧 -> 剩余 {0x77, 0x88} 未知
    reset_test_env();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP1Unk));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events_[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP1, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events_[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kMatchP1Unk.data(), s_sink.events_[0].data.data(), 4U);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[1].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknown_len_);
    TEST_ASSERT_EQUAL_HEX8(0x77U, s_sink.unknown_stream_[0]);
    TEST_ASSERT_EQUAL_HEX8(0x88U, s_sink.unknown_stream_[1]);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[1].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);

    // 3a) p0 拒 -> p1 拒 -> p2 在 p=3 成帧 -> buf[3..6) 与第 7 字节拼成 fixed4 帧
    reset_test_env();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP2ThenFixed4));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP2, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.events_[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kMatchP2ThenFixed4.data(), s_sink.events_[0].data.data(), 3U);
    TEST_ASSERT_EQUAL_UINT8(3U, s_sink.events_[1].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events_[1].decoder);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events_[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kMatchP2ThenFixed4[3], s_sink.events_[1].data.data(), 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknown_bytes);

    // 3b) p2 在 p=3 成帧 -> 后续 AA 冲突回放 p1 成帧并输出尾部未知字节
    reset_test_env();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP2ThenP1));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP2, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events_[1].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP1, s_sink.events_[1].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kMatchP2ThenP1[3], s_sink.events_[1].data.data(), 4U);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[2].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknown_len_);
}

static void test_all_reject_resync_recovers_nested_frame()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 6> kStream{0xA1U, 0x99U, 0xA1U, 0x05U, 0x06U, 0xA2U};
    static constexpr std::array<uint8_t, 2> kExpUnk{0xA1U, 0x99U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kStream));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[0].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknown_len_);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpUnk.data(), s_sink.unknown_stream_.data(), kExpUnk.size());
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events_[1].kind);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events_[1].decoder);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events_[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kStream[2], s_sink.events_[1].data.data(), 4U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknown_bytes);
}

static void test_reject_callback_causes_and_last_flag()
{
    // 1) 解码器 kReject → cause kStep，单解码器下 last == true，前缀为该起点全部字节
    {
        static constexpr std::array<uint8_t, 4> kBadChecksum{0xA1U, 0x01U, 0x02U, 0x00U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
        reset_test_env();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kBadChecksum));
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.reject_count_);
        const RecordedEvent &ev = s_sink.rejects_[0];
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EventKind::kReject), static_cast<uint8_t>(ev.kind));
        TEST_ASSERT_EQUAL_UINT8(0U, ev.decoder_index);
        TEST_ASSERT_EQUAL_PTR(&kDecFixed4, ev.decoder);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kStep), static_cast<uint8_t>(ev.cause));
        TEST_ASSERT_TRUE(ev.last);
        TEST_ASSERT_EQUAL_UINT32(4U, ev.len);
        TEST_ASSERT_EQUAL_HEX8(0xA1U, ev.data[0]);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    }

    // 2) 需求超过缓冲 → cause kTooLong
    {
        static constexpr std::array<uint8_t, 2> kTrigger{0xBAU, 0x01U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecOverflowBuf};
        reset_test_env();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(16U), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.reject_count_);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kTooLong),
                                static_cast<uint8_t>(s_sink.rejects_[0].cause));
        TEST_ASSERT_TRUE(s_sink.rejects_[0].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
    }

    // 3) need == 0 → cause kContract
    {
        static constexpr std::array<uint8_t, 2> kTrigger{0xBAU, 0x01U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecNeedZero};
        reset_test_env();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kContract),
                                static_cast<uint8_t>(s_sink.rejects_[0].cause));
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().overflows);
    }

    // 4) abort() 丢弃半帧 → cause kAbort，前缀长度等于已收字节数，last == true
    {
        static constexpr std::array<uint8_t, 2> kHalf{0xA1U, 0x07U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
        reset_test_env();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kHalf));
        TEST_ASSERT_EQUAL_UINT32(0U, s_sink.reject_count_);
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.reject_count_);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kAbort),
                                static_cast<uint8_t>(s_sink.rejects_[0].cause));
        TEST_ASSERT_EQUAL_UINT32(2U, s_sink.rejects_[0].len);
        TEST_ASSERT_TRUE(s_sink.rejects_[0].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().aborts);
    }

    // 5) 多解码器：同一首字节两个候选都立即拒绝 → 首个 last == false，末个 last == true
    {
        static constexpr std::array<uint8_t, 1> kTrigger{0xAAU};
        static constexpr std::array<const demux::Decoder *, 2> kDecs{&kDecAaNo0, &kDecAaNo1};
        reset_test_env();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(2U, s_sink.reject_count_);
        TEST_ASSERT_EQUAL_UINT8(0U, s_sink.rejects_[0].decoder_index);
        TEST_ASSERT_FALSE(s_sink.rejects_[0].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejects_[0].len);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kStep),
                                static_cast<uint8_t>(s_sink.rejects_[0].cause));
        TEST_ASSERT_EQUAL_UINT8(1U, s_sink.rejects_[1].decoder_index);
        TEST_ASSERT_TRUE(s_sink.rejects_[1].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejects_[1].len);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    }

    // 6) explain spy：通过 kDecoder 的 explain 指针可以直接取到前缀与原因文本
    {
        static constexpr std::array<uint8_t, 2> kPrefix{0xA1U, 0x02U};
        std::array<char, 32> text{};
        s_explain_calls = 0U;
        const std::size_t len = spy_explain(kPrefix, text);
        TEST_ASSERT_EQUAL_UINT32(1U, s_explain_calls);
        TEST_ASSERT_EQUAL_UINT32(2U, s_explain_prefix_len);
        TEST_ASSERT_EQUAL_UINT32(11U, len);
        TEST_ASSERT_EQUAL_STRING("spy says no", text.data());

        // 缓冲不足时返回 0，调用方按原因名兜底。
        std::array<char, 4> tiny{};
        TEST_ASSERT_EQUAL_UINT32(0U, spy_explain(kPrefix, tiny));
    }
}

static void test_contract_violations_and_overflow()
{
    static constexpr std::array<uint8_t, 2> kTrigger{0xBAU, 0x01U};

    // 1) need == 0 契约违规
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecNeedZero};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknown_bytes);
    }

    // 2) len + need > max_frame 契约违规（但 <= buf_size）
    reset_test_env();
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecExceedMax};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknown_bytes);
    }

    // 3) len + need > buf_size 触发 overflow（buf_size == 16，哨兵不得被踩）
    reset_test_env();
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecOverflowBuf};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(16U), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknown_bytes);
        for (std::size_t i = 16U; i < s_guarded_buf.size(); ++i) {
            TEST_ASSERT_EQUAL_HEX8(kTestCanaryByte, s_guarded_buf[i]);
        }
    }

    // 4) need == SIZE_MAX 防无符号回绕溢出
    reset_test_env();
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecOverflowSizeMax};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(16U), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknown_bytes);
    }

    // 5) 契约违规视为 kReject，同首字节次优先级解码器仍可接管成帧
    reset_test_env();
    {
        static constexpr std::array<const demux::Decoder *, 2> kDecs{&kDecNeedZero, &kDecBaValid2};
        static constexpr std::array<uint8_t, 2> kOk{0xBAU, 0x77U};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kOk));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[1].frames);
        TEST_ASSERT_EQUAL_PTR(&kDecBaValid2, s_sink.events_[0].decoder);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknown_bytes);
    }
}

static void test_reentry_from_callbacks()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 4> kFrame{0xA1U, 0x05U, 0x0AU, 0xAEU};
    static constexpr std::array<uint8_t, 8> kTwoFrames{
        0xA1U, 0x05U, 0x0AU, 0xAEU, 0xA1U, 0x01U, 0x02U, 0xA2U,
    };
    static constexpr std::array<uint8_t, 1> kUnk{0x55U};

    // on_frame 内重入
    s_sink.reentry_decs_ = kDecs;
    s_sink.reentry_buf_ = guarded_slice(kTestBufCap);
    s_sink.test_reentry_in_frame_ = true;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTwoFrames));
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_feed_err_);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_abort_err_);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_init_err_);
    // 重入 init 失败不破坏实例，第 2 帧照常完成
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().decoder[0].frames);

    // on_unknown 内重入
    s_sink.test_reentry_in_frame_ = false;
    s_sink.test_reentry_in_unknown_ = true;
    s_sink.reentry_feed_err_ = demux::Error::kOk;
    s_sink.reentry_abort_err_ = demux::Error::kOk;
    s_sink.reentry_init_err_ = demux::Error::kOk;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kUnk));
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_feed_err_);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_abort_err_);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_init_err_);

    // abort 的 on_unknown 回调内重入
    s_sink.reentry_feed_err_ = demux::Error::kOk;
    s_sink.reentry_abort_err_ = demux::Error::kOk;
    s_sink.reentry_init_err_ = demux::Error::kOk;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kFrame).subspan(0, 2U)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_feed_err_);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_abort_err_);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_init_err_);

    // abort 的 on_frame 回放回调内重入
    {
        static constexpr std::array<const demux::Decoder *, 2> kDecsConflict{&kDecAaP0, &kDecAaP1};
        static constexpr std::array<uint8_t, 4> kHalfP1{0xAAU, 0x22U, 0x99U, 0x44U};

        s_sink.test_reentry_in_unknown_ = false;
        s_sink.test_reentry_in_frame_ = true;
        s_sink.reentry_decs_ = kDecsConflict;
        s_sink.reentry_feed_err_ = demux::Error::kOk;
        s_sink.reentry_abort_err_ = demux::Error::kOk;
        s_sink.reentry_init_err_ = demux::Error::kOk;
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecsConflict, guarded_slice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kHalfP1));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
        TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_feed_err_);
        TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_abort_err_);
        TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentry_init_err_);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[1].frames);
    }

    // 回调中调用 reset_stats() 安全且不影响后续解析
    reset_test_env();
    s_sink.reset_stats_in_frame_ = true;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTwoFrames));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
}

static void test_abort_busy_and_recovery()
{
    static constexpr std::array<const demux::Decoder *, 3> kDecs{&kDecAaP0, &kDecAaP1, &kDecFixed4};
    static constexpr std::array<uint8_t, 5> kNestedInHalf{0xAAU, 0xA1U, 0x01U, 0x02U, 0xA2U};
    static constexpr std::array<uint8_t, 5> kReplayInAbort{0xAAU, 0x22U, 0x99U, 0x44U, 0x77U};
    static constexpr std::array<uint8_t, 2> kPartial{0xA1U, 0x07U};

    // 空闲 abort 不计数
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().aborts);

    // 纯半帧 abort
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kPartial));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().aborts);
    TEST_ASSERT_EQUAL_UINT(kPartial.size(), s_sink.unknown_len_);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kPartial.data(), s_sink.unknown_stream_.data(), kPartial.size());

    // 半帧内嵌套另一解码器完整帧
    reset_test_env();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kNestedInHalf));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[0].kind);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.unknown_len_);
    TEST_ASSERT_EQUAL_HEX8(0xAAU, s_sink.unknown_stream_[0]);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events_[1].kind);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events_[1].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events_[1].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kNestedInHalf[1], s_sink.events_[1].data.data(), 4U);

    // 同首字节候选在 abort 时回放成帧
    reset_test_env();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kReplayInAbort));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events_[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP1, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kReplayInAbort.data(), s_sink.events_[0].data.data(), 4U);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events_[1].kind);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.unknown_len_);
    TEST_ASSERT_EQUAL_HEX8(0x77U, s_sink.unknown_stream_[0]);
}

static void test_reset_stats_midframe_and_eight_decoders()
{
    static constexpr std::array<const demux::Decoder *, demux::kMaxDecoders> kDecs{
        &kDecSlot0, &kDecSlot1, &kDecSlot2, &kDecSlot3, &kDecSlot4, &kDecSlot5, &kDecSlot6, &kDecSlot7,
    };
    static constexpr std::array<uint8_t, 2> kF0{0xC0U, 0x3FU};
    static constexpr std::array<uint8_t, 2> kF7{0xC7U, 0x38U};
    static constexpr std::array<uint8_t, 2> kBad{0xC3U, 0x00U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kF0));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kBad));
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[3].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknown_bytes);

    // 喂入 slot7 第 1 字节，半帧期间 reset_stats 必须保持解析状态
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kF7).subspan(0, 1U)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    s_dmx.reset_stats();
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[3].rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknown_bytes);

    // 喂入第 2 字节完成 slot7 帧
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kF7).subspan(1U, 1U)));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[7].frames);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().decoder[7].bytes);
    TEST_ASSERT_EQUAL_UINT8(7U, s_sink.events_[s_sink.event_count_ - 1U].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecSlot7, s_sink.events_[s_sink.event_count_ - 1U].decoder);
}

static void test_two_instances_independent()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 4> kFa{0xA1U, 0x01U, 0x02U, 0xA2U};
    static constexpr std::array<uint8_t, 4> kFb{0xA1U, 0x10U, 0x20U, 0x91U};
    demux::Demux dmx_b;
    std::array<uint8_t, kTestBufCap> buf_b{};
    TestSink sink_b;

    sink_b.reset(&dmx_b);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guarded_slice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, dmx_b.init(kDecs, buf_b, sink_b));

    const std::span<const uint8_t> sa(kFa);
    const std::span<const uint8_t> sb(kFb);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(sa.subspan(0, 2U)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, dmx_b.feed(sb.subspan(0, 2U)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_TRUE(dmx_b.busy());

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(sa.subspan(2U, 2U)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, dmx_b.feed(sb.subspan(2U, 2U)));

    TEST_ASSERT_EQUAL_UINT(1U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT(1U, sink_b.event_count_);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFa.data(), s_sink.events_[0].data.data(), kFa.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFb.data(), sink_b.events_[0].data.data(), kFb.size());
}

static void test_static_demux()
{
    using SingleDemux = demux::StaticDemux<kDecFixed4>;
    using PairDemux = demux::StaticDemux<kDecFixed4, kDecBulk>;
    using EightDemux =
        demux::StaticDemux<kDecSlot0, kDecSlot1, kDecSlot2, kDecSlot3, kDecSlot4, kDecSlot5, kDecSlot6, kDecSlot7>;

    // 编译期断言：kCount、kBufSize、index_of
    static_assert(SingleDemux::kCount == 1U);
    static_assert(SingleDemux::kBufSize == kDecFixed4.max_frame);
    static_assert(SingleDemux::index_of<kDecFixed4>() == 0U);

    static_assert(PairDemux::kCount == 2U);
    static_assert(PairDemux::kBufSize == kDecBulk.max_frame);
    static_assert(PairDemux::index_of<kDecFixed4>() == 0U);
    static_assert(PairDemux::index_of<kDecBulk>() == 1U);

    static_assert(EightDemux::kCount == demux::kMaxDecoders);
    static_assert(EightDemux::kBufSize == 2U);
    static_assert(EightDemux::index_of<kDecSlot0>() == 0U);
    static_assert(EightDemux::index_of<kDecSlot7>() == 7U);

    PairDemux sd;
    s_sink.reset(nullptr);
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.init(s_sink));
    TEST_ASSERT_FALSE(sd.busy());

    static constexpr std::array<uint8_t, 4> kF4{0xA1U, 0x03U, 0x04U, 0xA6U};
    static constexpr std::array<uint8_t, 5> kFbulk{0xA2U, 0x05U, 0x11U, 0x22U, 0xEEU};
    static constexpr std::array<uint8_t, 2> kHalf{0xA1U, 0x55U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.feed(kF4));
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.feed(kFbulk));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT8(PairDemux::index_of<kDecFixed4>(), s_sink.events_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT8(PairDemux::index_of<kDecBulk>(), s_sink.events_[1].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecBulk, s_sink.events_[1].decoder);

    TEST_ASSERT_EQUAL_UINT32(1U, sd.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(1U, sd.stats().decoder[1].frames);

    // 半帧 -> busy -> reset_stats -> abort 转发验证
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.feed(kHalf));
    TEST_ASSERT_TRUE(sd.busy());
    sd.reset_stats();
    TEST_ASSERT_TRUE(sd.busy());
    TEST_ASSERT_EQUAL_UINT32(0U, sd.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.abort());
    TEST_ASSERT_FALSE(sd.busy());
    TEST_ASSERT_EQUAL_UINT32(1U, sd.stats().aborts);
    TEST_ASSERT_EQUAL_UINT32(2U, sd.stats().unknown_bytes);

    // 8 槽 StaticDemux 转发验证
    EightDemux eight;
    s_sink.reset(nullptr);
    TEST_ASSERT_EQUAL(demux::Error::kOk, eight.init(s_sink));
    static constexpr std::array<uint8_t, 4> kSlots{0xC0U, 0x3FU, 0xC7U, 0x38U};
    TEST_ASSERT_EQUAL(demux::Error::kOk, eight.feed(kSlots));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.event_count_);
    TEST_ASSERT_EQUAL_UINT8(EightDemux::index_of<kDecSlot0>(), s_sink.events_[0].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecSlot0, s_sink.events_[0].decoder);
    TEST_ASSERT_EQUAL_UINT8(EightDemux::index_of<kDecSlot7>(), s_sink.events_[1].decoder_index);
    TEST_ASSERT_EQUAL_PTR(&kDecSlot7, s_sink.events_[1].decoder);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_uninitialized_and_constinit_globals);
    RUN_TEST(test_init_validation_and_poison_on_fail);
    RUN_TEST(test_feed_empty_span_and_zero_len);
    RUN_TEST(test_idle_unknown_and_default_on_unknown);
    RUN_TEST(test_bulk_accumulation_and_split_chunks);
    RUN_TEST(test_same_first_byte_priority_and_midbuffer_replay);
    RUN_TEST(test_all_reject_resync_recovers_nested_frame);
    RUN_TEST(test_reject_callback_causes_and_last_flag);
    RUN_TEST(test_contract_violations_and_overflow);
    RUN_TEST(test_reentry_from_callbacks);
    RUN_TEST(test_abort_busy_and_recovery);
    RUN_TEST(test_reset_stats_midframe_and_eight_decoders);
    RUN_TEST(test_two_instances_independent);
    RUN_TEST(test_static_demux);
    return UNITY_END();
}
