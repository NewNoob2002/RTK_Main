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
    uint8_t decoderIndex = 0U;
    const demux::Decoder *decoder = nullptr;
    std::size_t len = 0U;
    std::array<uint8_t, kTestBufCap> data{};
    demux::RejectCause cause = demux::RejectCause::kStep;
    bool last = false;
};

/** explain 回调 spy：记录调用次数与最后一次拿到的前缀。 */
uint32_t s_explain_calls = 0U;
std::size_t s_explain_prefix_len = 0U;

std::size_t spyExplain(std::span<const uint8_t> prefix, std::span<char> out) noexcept
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
bool decFixed4First(uint8_t byte) noexcept
{
    return byte == 0xA1U;
}

demux::Step decFixed4Step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 4U) {
        need = 4U - frame.size();
        return demux::Step::kNeedMore;
    }
    const auto expected = static_cast<uint8_t>(
        static_cast<unsigned int>(frame[0]) ^ static_cast<unsigned int>(frame[1]) ^
        static_cast<unsigned int>(frame[2])); // 3 个 uint8_t 异或结果在 0..255
    return (expected == frame[3]) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecFixed4{"fixed4", 4U, &decFixed4First, &decFixed4Step};

// 2) 批量 spy 解码器：A2 <total_len> <payload...> <0xEE>；统计 step 调用次数
uint32_t s_bulk_step_calls = 0U;

bool decBulkFirst(uint8_t byte) noexcept
{
    return byte == 0xA2U;
}

demux::Step decBulkStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
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

constexpr demux::Decoder kDecBulk{"bulk64", 64U, &decBulkFirst, &decBulkStep};

// 3) 同首字节冲突组（均以 0xAA 开头）：
//    - p0: 6 字节，要求 frame[5] == 0x55（前 5 字节不拒，凑满 6 字节后才可能拒，用于触发长前缀回放）
//    - p1: 4 字节，要求 frame[1] == 0x22 && frame[3] == 0x44
//    - p2: 3 字节，要求 frame[1] == 0x33 && frame[2] == 0x66
bool decAaFirst(uint8_t byte) noexcept
{
    return byte == 0xAAU;
}

demux::Step decAaP0Step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 6U) {
        need = 6U - frame.size();
        return demux::Step::kNeedMore;
    }
    return (frame[5] == 0x55U) ? demux::Step::kFrame : demux::Step::kReject;
}

demux::Step decAaP1Step(std::span<const uint8_t> frame, std::size_t &need) noexcept
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

demux::Step decAaP2Step(std::span<const uint8_t> frame, std::size_t &need) noexcept
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
demux::Step decAaRejectNowStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = 0U;
    return demux::Step::kReject;
}

constexpr demux::Decoder kDecAaNo0{"aa_no0", 4U, &decAaFirst, &decAaRejectNowStep};
constexpr demux::Decoder kDecAaNo1{"aa_no1", 4U, &decAaFirst, &decAaRejectNowStep};
constexpr demux::Decoder kDecAaP0{"aa_p0", 6U, &decAaFirst, &decAaP0Step};
constexpr demux::Decoder kDecAaP1{"aa_p1", 4U, &decAaFirst, &decAaP1Step};
constexpr demux::Decoder kDecAaP2{"aa_p2", 3U, &decAaFirst, &decAaP2Step};

// 4) 契约违规与溢出解码器（均以 0xBA 开头）
bool decBaFirst(uint8_t byte) noexcept
{
    return byte == 0xBAU;
}

demux::Step decNeedZeroStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = 0U; // 违规：kNeedMore 时 need == 0
    return demux::Step::kNeedMore;
}

demux::Step decExceedMaxStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    need = 17U - frame.size(); // maxFrame == 16，17 超出 maxFrame
    return demux::Step::kNeedMore;
}

demux::Step decOverflowBufStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = 20U; // 当 buf_size == 16 时，1 + 20 > 16
    return demux::Step::kNeedMore;
}

demux::Step decOverflowSizemaxStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    static_cast<void>(frame);
    need = std::numeric_limits<std::size_t>::max(); // 测试无符号加法回绕溢出
    return demux::Step::kNeedMore;
}

demux::Step decBaValid2Step(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 2U) {
        need = 1U;
        return demux::Step::kNeedMore;
    }
    return (frame[1] == 0x77U) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecNeedZero{"bad_need0", 16U, &decBaFirst, &decNeedZeroStep};
constexpr demux::Decoder kDecExceedMax{"bad_max", 16U, &decBaFirst, &decExceedMaxStep};
constexpr demux::Decoder kDecOverflowBuf{"bad_ovf", 16U, &decBaFirst, &decOverflowBufStep};
constexpr demux::Decoder kDecOverflowSizeMax{"bad_smax", 16U, &decBaFirst, &decOverflowSizemaxStep};
constexpr demux::Decoder kDecBaValid2{"ba_ok2", 2U, &decBaFirst, &decBaValid2Step};

// 5) 8 个解码器满配测试（首字节 0xC0..0xC7，每帧 2 字节：0xCx <~0xCx>）
template <uint8_t Tag> bool decSlotFirst(uint8_t byte) noexcept
{
    return byte == Tag;
}

demux::Step decSlotStep(std::span<const uint8_t> frame, std::size_t &need) noexcept
{
    if (frame.size() < 2U) {
        need = 1U;
        return demux::Step::kNeedMore;
    }
    // 0xFFU 与 uint8_t 异或结果为 0..255
    const auto expected = static_cast<uint8_t>(frame[0] ^ 0xFFU);
    return (frame[1] == expected) ? demux::Step::kFrame : demux::Step::kReject;
}

constexpr demux::Decoder kDecSlot0{"slot0", 2U, &decSlotFirst<0xC0U>, &decSlotStep};
constexpr demux::Decoder kDecSlot1{"slot1", 2U, &decSlotFirst<0xC1U>, &decSlotStep};
constexpr demux::Decoder kDecSlot2{"slot2", 2U, &decSlotFirst<0xC2U>, &decSlotStep};
constexpr demux::Decoder kDecSlot3{"slot3", 2U, &decSlotFirst<0xC3U>, &decSlotStep};
constexpr demux::Decoder kDecSlot4{"slot4", 2U, &decSlotFirst<0xC4U>, &decSlotStep};
constexpr demux::Decoder kDecSlot5{"slot5", 2U, &decSlotFirst<0xC5U>, &decSlotStep};
constexpr demux::Decoder kDecSlot6{"slot6", 2U, &decSlotFirst<0xC6U>, &decSlotStep};
constexpr demux::Decoder kDecSlot7{"slot7", 2U, &decSlotFirst<0xC7U>, &decSlotStep};

// --------------------------------------------------------------------------
// Sink 实现与全局测试对象
// --------------------------------------------------------------------------

class TestSink final : public demux::Sink {
public:
    void reset(demux::Demux *demuxPtr) noexcept
    {
        dmx = demuxPtr;
        eventCount = 0U;
        unknownLen = 0U;
        testReentryInFrame = false;
        testReentryInUnknown = false;
        resetStatsInFrame = false;
        reentryFeedErr = demux::Error::kOk;
        reentryAbortErr = demux::Error::kOk;
        reentryInitErr = demux::Error::kOk;
        reentryDecs = {};
        reentryBuf = {};
        rejectCount = 0U;
    }

    void onFrame(const demux::Decoder &decoder, uint8_t index,
                 std::span<const uint8_t> frame) noexcept override
    {
        static constexpr std::array<uint8_t, 1> kExtra{0x5AU};
        if (testReentryInFrame && (dmx != nullptr)) {
            reentryFeedErr = dmx->feed(kExtra);
            reentryAbortErr = dmx->abort();
            reentryInitErr = dmx->init(reentryDecs, reentryBuf, *this);
        }
        if (resetStatsInFrame && (dmx != nullptr)) {
            dmx->resetStats();
        }
        if (eventCount < kMaxEvents) {
            RecordedEvent &ev = events[eventCount++];
            ev.kind = EventKind::kFrame;
            ev.decoderIndex = index;
            ev.decoder = &decoder;
            ev.len = frame.size();
            if (frame.size() <= ev.data.size()) {
                std::copy(frame.begin(), frame.end(), ev.data.begin());
            }
        }
    }

    void onReject(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                  demux::RejectCause cause, bool last) noexcept override
    {
        if (rejectCount < rejects.size()) {
            RecordedEvent &ev = rejects[rejectCount++];
            ev.kind = EventKind::kReject;
            ev.decoderIndex = index;
            ev.decoder = &decoder;
            ev.len = prefix.size();
            ev.cause = cause;
            ev.last = last;
            if (prefix.size() <= ev.data.size()) {
                std::copy(prefix.begin(), prefix.end(), ev.data.begin());
            }
        }
    }

    void onUnknown(std::span<const uint8_t> bytes) noexcept override
    {
        static constexpr std::array<uint8_t, 1> kExtra{0x5AU};
        if (testReentryInUnknown && (dmx != nullptr)) {
            reentryFeedErr = dmx->feed(kExtra);
            reentryAbortErr = dmx->abort();
            reentryInitErr = dmx->init(reentryDecs, reentryBuf, *this);
        }
        if (eventCount < kMaxEvents) {
            RecordedEvent &ev = events[eventCount++];
            ev.kind = EventKind::kUnknown;
            ev.decoderIndex = 0U;
            ev.decoder = nullptr;
            ev.len = bytes.size();
            if (bytes.size() <= ev.data.size()) {
                std::copy(bytes.begin(), bytes.end(), ev.data.begin());
            }
        }
        if ((unknownLen + bytes.size()) <= unknownStream.size()) {
            std::copy(bytes.begin(), bytes.end(),
                      unknownStream.begin() + static_cast<std::ptrdiff_t>(unknownLen));
        }
        unknownLen += bytes.size();
    }

    demux::Demux *dmx = nullptr;
    std::array<RecordedEvent, kMaxEvents> events{};
    std::size_t eventCount = 0U;
    std::array<uint8_t, kMaxStreamCap> unknownStream{};
    std::size_t unknownLen = 0U;
    bool testReentryInFrame = false;
    bool testReentryInUnknown = false;
    bool resetStatsInFrame = false;
    /** 拒绝事件单独存放，避免改变 events 的既有下标记号（其它用例按序号断言）。 */
    std::array<RecordedEvent, kMaxEvents> rejects{};
    uint32_t rejectCount = 0U;
    demux::Error reentryFeedErr = demux::Error::kOk;
    demux::Error reentryAbortErr = demux::Error::kOk;
    demux::Error reentryInitErr = demux::Error::kOk;
    std::span<const demux::Decoder *const> reentryDecs{};
    std::span<uint8_t> reentryBuf{};
};

// 仅实现 onFrame、继承默认 Sink::onUnknown 的极简 Sink
class MinimalSink final : public demux::Sink {
public:
    void onFrame(const demux::Decoder &decoder, uint8_t index,
                 std::span<const uint8_t> frame) noexcept override
    {
        lastDecoder = &decoder;
        lastIndex = index;
        lastLen = frame.size();
        frameCount++;
    }

    const demux::Decoder *lastDecoder = nullptr;
    uint8_t lastIndex = 0U;
    std::size_t lastLen = 0U;
    uint32_t frameCount = 0U;
};

// constinit 全局实例验证（CPP-020 / ADR-0005 §2）
constinit demux::Demux s_global_demux;
constinit demux::StaticDemux<kDecFixed4, kDecBulk> s_global_static_demux;

TestSink s_sink;
std::array<uint8_t, kTestBufCap + kTestCanaryLen> s_guarded_buf{};
demux::Demux s_dmx;

std::span<uint8_t> guardedSlice(std::size_t size) noexcept
{
    return std::span<uint8_t>(s_guarded_buf).subspan(0, size);
}

void resetTestEnv() noexcept
{
    s_sink.reset(&s_dmx);
    s_guarded_buf.fill(kTestCanaryByte);
    s_bulk_step_calls = 0U;
}

} // namespace

extern "C" void setUp(void)
{
    resetTestEnv();
}

extern "C" void tearDown(void) {}

// --------------------------------------------------------------------------
// 测试用例
// --------------------------------------------------------------------------

static void testUninitializedAndConstinitGlobals()
{
    static constexpr std::array<uint8_t, 4> kFrame{0xA1U, 0x10U, 0x20U, 0x91U};
    demux::Demux localDmx;
    demux::StaticDemux<kDecFixed4> localStatic;

    // 1) 未 init 状态下 feed（含空 span）与 abort 均返回 kInvalidArg
    TEST_ASSERT_FALSE(localDmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, localDmx.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, localDmx.feed({}));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, localDmx.abort());

    TEST_ASSERT_FALSE(localStatic.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, localStatic.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, localStatic.feed({}));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, localStatic.abort());

    // 2) constinit 全局实例初始处于未初始化状态，随后可正常 init 并解析
    TEST_ASSERT_FALSE(s_global_demux.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_demux.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_demux.abort());

    TEST_ASSERT_FALSE(s_global_static_demux.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_static_demux.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_global_static_demux.abort());

    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    s_sink.reset(&s_global_demux);
    TEST_ASSERT_EQUAL(demux::Error::kOk,
                      s_global_demux.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_demux.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_global_demux.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events[0].decoder);

    s_sink.reset(nullptr);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_static_demux.init(s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_global_static_demux.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_global_static_demux.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT8((demux::StaticDemux<kDecFixed4, kDecBulk>::indexOf<kDecFixed4>()),
                            s_sink.events[0].decoderIndex);
}

static void testInitValidationAndPoisonOnFail()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecsOk{&kDecFixed4};
    static constexpr std::array<const demux::Decoder *, 2> kDecsNullItem{&kDecFixed4, nullptr};
    static constexpr std::array<const demux::Decoder *, 1> kDecsOnlyNull{nullptr};
    static constexpr std::array<const demux::Decoder *, 2> kDecsTwo{&kDecFixed4, &kDecBulk};
    static constexpr std::array<const demux::Decoder *, demux::kMaxDecoders + 1U> kDecsNine{
        &kDecSlot0, &kDecSlot1, &kDecSlot2, &kDecSlot3,  &kDecSlot4,
        &kDecSlot5, &kDecSlot6, &kDecSlot7, &kDecFixed4,
    };
    static constexpr std::array<uint8_t, 4> kFrame{0xA1U, 0x10U, 0x20U, 0x91U};
    static std::array<uint8_t, 65536> sHugeBuf{};

    // 空表、超长表、含空指针
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init({}, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(kDecsNine, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(kDecsNullItem, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(kDecsOnlyNull, guardedSlice(kTestBufCap), s_sink));

    // 缓冲区为空或小于 maxFrame（含后项 maxFrame 更大）
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(kDecsOk, {}, s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(kDecsOk, guardedSlice(kDecFixed4.maxFrame - 1U), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(kDecsTwo, guardedSlice(kDecBulk.maxFrame - 1U), s_sink));

    // 解码器字段非法
    demux::Decoder badDec = kDecFixed4;
    const std::array<const demux::Decoder *, 1> badList{&badDec};

    badDec = kDecFixed4;
    badDec.name = nullptr;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(badList, guardedSlice(kTestBufCap), s_sink));

    badDec = kDecFixed4;
    badDec.maxFrame = 0U;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(badList, guardedSlice(kTestBufCap), s_sink));

    badDec = kDecFixed4;
    badDec.maxFrame = 65536U;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init(badList, sHugeBuf, s_sink));

    badDec = kDecFixed4;
    badDec.firstByte = nullptr;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(badList, guardedSlice(kTestBufCap), s_sink));

    badDec = kDecFixed4;
    badDec.step = nullptr;
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg,
                      s_dmx.init(badList, guardedSlice(kTestBufCap), s_sink));

    // 先成功 init，再用非法配置 re-init 失败后，实例必须回到未初始化状态（毒化）
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecsOk, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);

    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.init({}, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL(demux::Error::kInvalidArg, s_dmx.abort());

    // 重新有效 init 后恢复可用且统计已清空
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecsOk, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kFrame));
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
}

static void testFeedEmptySpanAndZeroLen()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 1> kDummy{0x00U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed({}));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kDummy.data(), 0U)));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(0U, s_sink.eventCount);
}

static void testIdleUnknownAndDefaultOnUnknown()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 8> kStream{0x11U, 0x22U, 0xA1U, 0x01U,
                                                    0x02U, 0xA2U, 0x33U, 0x44U};
    static constexpr std::array<uint8_t, 4> kUnk{0x11U, 0x22U, 0x33U, 0x44U};
    static constexpr std::array<uint8_t, 2> kHalf{0xA1U, 0x99U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kStream));
    TEST_ASSERT_FALSE(s_dmx.busy());

    // 流顺序：未知字节 {0x11,0x22} 在帧前，{0x33,0x44} 在帧后
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.eventCount);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events[1].decoder);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[2].kind);
    TEST_ASSERT_EQUAL_UINT(kUnk.size(), s_sink.unknownLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kUnk.data(), s_sink.unknownStream.data(), kUnk.size());

    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(4U, s_dmx.stats().decoder[0].bytes);
    TEST_ASSERT_EQUAL_UINT32(4U, s_dmx.stats().unknownBytes);

    // 验证 Sink::onUnknown 默认实现（丢弃且统计照常计数）
    MinimalSink minSink;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), minSink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kStream));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kHalf));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_EQUAL_UINT32(1U, minSink.frameCount);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, minSink.lastDecoder);
    TEST_ASSERT_EQUAL_UINT(4U, minSink.lastLen);
    TEST_ASSERT_EQUAL_UINT32(6U, s_dmx.stats().unknownBytes);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().aborts);
}

static void testBulkAccumulationAndSplitChunks()
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
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(frame));
    TEST_ASSERT_EQUAL_UINT32(3U, s_bulk_step_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_PTR(&kDecBulk, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT(frame.size(), s_sink.events[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(frame.data(), s_sink.events[0].data.data(), frame.size());

    // 分块喂入（10 + 10 + 20），step 依然只在凑满 need 时触发（共 3 次）
    resetTestEnv();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    const std::span<const uint8_t> fspan(frame);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(fspan.subspan(0, 10)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(0U, s_sink.eventCount);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(fspan.subspan(10, 10)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(fspan.subspan(20, 20)));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(3U, s_bulk_step_calls);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.eventCount);
}

static void testSameFirstBytePriorityAndMidbufferReplay()
{
    static constexpr std::array<const demux::Decoder *, 4> kDecs{&kDecAaP0, &kDecAaP1, &kDecAaP2,
                                                                 &kDecFixed4};
    static constexpr std::array<uint8_t, 6> kMatchP0{0xAAU, 0x22U, 0x00U, 0x44U, 0x00U, 0x55U};
    static constexpr std::array<uint8_t, 6> kMatchP1Unk{0xAAU, 0x22U, 0x99U, 0x44U, 0x77U, 0x88U};
    static constexpr std::array<uint8_t, 7> kMatchP2ThenFixed4{0xAAU, 0x33U, 0x66U, 0xA1U,
                                                               0x05U, 0x06U, 0xA2U};
    static constexpr std::array<uint8_t, 9> kMatchP2ThenP1{
        0xAAU, 0x33U, 0x66U, 0xAAU, 0x22U, 0x99U, 0x44U, 0x77U, 0x88U,
    };

    // 1) 命中 p0（优先级最高）
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP0));
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT8(0U, s_sink.rejects[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP0, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[1].rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);

    // 2) p0 收满 6 字节拒 -> 回放 p1 在 p=4 成帧 -> 剩余 {0x77, 0x88} 未知
    resetTestEnv();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP1Unk));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP1, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kMatchP1Unk.data(), s_sink.events[0].data.data(), 4U);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknownLen);
    TEST_ASSERT_EQUAL_HEX8(0x77U, s_sink.unknownStream[0]);
    TEST_ASSERT_EQUAL_HEX8(0x88U, s_sink.unknownStream[1]);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[1].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);

    // 3a) p0 拒 -> p1 拒 -> p2 在 p=3 成帧 -> buf[3..6) 与第 7 字节拼成 fixed4 帧
    resetTestEnv();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP2ThenFixed4));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP2, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.events[0].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kMatchP2ThenFixed4.data(), s_sink.events[0].data.data(), 3U);
    TEST_ASSERT_EQUAL_UINT8(3U, s_sink.events[1].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events[1].decoder);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kMatchP2ThenFixed4[3], s_sink.events[1].data.data(), 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);

    // 3b) p2 在 p=3 成帧 -> 后续 AA 冲突回放 p1 成帧并输出尾部未知字节
    resetTestEnv();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kMatchP2ThenP1));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(3U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP2, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events[1].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP1, s_sink.events[1].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kMatchP2ThenP1[3], s_sink.events[1].data.data(), 4U);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[2].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknownLen);
}

static void testAllRejectResyncRecoversNestedFrame()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 6> kStream{0xA1U, 0x99U, 0xA1U, 0x05U, 0x06U, 0xA2U};
    static constexpr std::array<uint8_t, 2> kExpUnk{0xA1U, 0x99U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kStream));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.unknownLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kExpUnk.data(), s_sink.unknownStream.data(), kExpUnk.size());
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events[1].decoder);
    TEST_ASSERT_EQUAL_UINT(4U, s_sink.events[1].len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kStream[2], s_sink.events[1].data.data(), 4U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknownBytes);
}

static void testRejectCallbackCausesAndLastFlag()
{
    // 1) 解码器 kReject → cause kStep，单解码器下 last == true，前缀为该起点全部字节
    {
        static constexpr std::array<uint8_t, 4> kBadChecksum{0xA1U, 0x01U, 0x02U, 0x00U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
        resetTestEnv();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kBadChecksum));
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejectCount);
        const RecordedEvent &ev = s_sink.rejects[0];
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EventKind::kReject),
                                static_cast<uint8_t>(ev.kind));
        TEST_ASSERT_EQUAL_UINT8(0U, ev.decoderIndex);
        TEST_ASSERT_EQUAL_PTR(&kDecFixed4, ev.decoder);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kStep),
                                static_cast<uint8_t>(ev.cause));
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
        resetTestEnv();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(16U), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejectCount);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kTooLong),
                                static_cast<uint8_t>(s_sink.rejects[0].cause));
        TEST_ASSERT_TRUE(s_sink.rejects[0].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
    }

    // 3) need == 0 → cause kContract
    {
        static constexpr std::array<uint8_t, 2> kTrigger{0xBAU, 0x01U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecNeedZero};
        resetTestEnv();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kContract),
                                static_cast<uint8_t>(s_sink.rejects[0].cause));
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().overflows);
    }

    // 4) abort() 丢弃半帧 → cause kAbort，前缀长度等于已收字节数，last == true
    {
        static constexpr std::array<uint8_t, 2> kHalf{0xA1U, 0x07U};
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
        resetTestEnv();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kHalf));
        TEST_ASSERT_EQUAL_UINT32(0U, s_sink.rejectCount);
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejectCount);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kAbort),
                                static_cast<uint8_t>(s_sink.rejects[0].cause));
        TEST_ASSERT_EQUAL_UINT32(2U, s_sink.rejects[0].len);
        TEST_ASSERT_TRUE(s_sink.rejects[0].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().aborts);
    }

    // 5) 多解码器：同一首字节两个候选都立即拒绝 → 首个 last == false，末个 last == true
    {
        static constexpr std::array<uint8_t, 1> kTrigger{0xAAU};
        static constexpr std::array<const demux::Decoder *, 2> kDecs{&kDecAaNo0, &kDecAaNo1};
        resetTestEnv();
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(2U, s_sink.rejectCount);
        TEST_ASSERT_EQUAL_UINT8(0U, s_sink.rejects[0].decoderIndex);
        TEST_ASSERT_FALSE(s_sink.rejects[0].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejects[0].len);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(demux::RejectCause::kStep),
                                static_cast<uint8_t>(s_sink.rejects[0].cause));
        TEST_ASSERT_EQUAL_UINT8(1U, s_sink.rejects[1].decoderIndex);
        TEST_ASSERT_TRUE(s_sink.rejects[1].last);
        TEST_ASSERT_EQUAL_UINT32(1U, s_sink.rejects[1].len);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    }

    // 6) explain spy：通过 kDecoder 的 explain 指针可以直接取到前缀与原因文本
    {
        static constexpr std::array<uint8_t, 2> kPrefix{0xA1U, 0x02U};
        std::array<char, 32> text{};
        s_explain_calls = 0U;
        const std::size_t len = spyExplain(kPrefix, text);
        TEST_ASSERT_EQUAL_UINT32(1U, s_explain_calls);
        TEST_ASSERT_EQUAL_UINT32(2U, s_explain_prefix_len);
        TEST_ASSERT_EQUAL_UINT32(11U, len);
        TEST_ASSERT_EQUAL_STRING("spy says no", text.data());

        // 缓冲不足时返回 0，调用方按原因名兜底。
        std::array<char, 4> tiny{};
        TEST_ASSERT_EQUAL_UINT32(0U, spyExplain(kPrefix, tiny));
    }
}

static void testContractViolationsAndOverflow()
{
    static constexpr std::array<uint8_t, 2> kTrigger{0xBAU, 0x01U};

    // 1) need == 0 契约违规
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecNeedZero};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknownBytes);
    }

    // 2) len + need > maxFrame 契约违规（但 <= buf_size）
    resetTestEnv();
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecExceedMax};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknownBytes);
    }

    // 3) len + need > buf_size 触发 overflow（buf_size == 16，哨兵不得被踩）
    resetTestEnv();
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecOverflowBuf};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(16U), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknownBytes);
        for (std::size_t i = 16U; i < s_guarded_buf.size(); ++i) {
            TEST_ASSERT_EQUAL_HEX8(kTestCanaryByte, s_guarded_buf[i]);
        }
    }

    // 4) need == SIZE_MAX 防无符号回绕溢出
    resetTestEnv();
    {
        static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecOverflowSizeMax};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(16U), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTrigger));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().overflows);
        TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknownBytes);
    }

    // 5) 契约违规视为 kReject，同首字节次优先级解码器仍可接管成帧
    resetTestEnv();
    {
        static constexpr std::array<const demux::Decoder *, 2> kDecs{&kDecNeedZero, &kDecBaValid2};
        static constexpr std::array<uint8_t, 2> kOk{0xBAU, 0x77U};
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kOk));
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].rejects);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[1].frames);
        TEST_ASSERT_EQUAL_PTR(&kDecBaValid2, s_sink.events[0].decoder);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
        TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);
    }
}

static void testReentryFromCallbacks()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 4> kFrame{0xA1U, 0x05U, 0x0AU, 0xAEU};
    static constexpr std::array<uint8_t, 8> kTwoFrames{
        0xA1U, 0x05U, 0x0AU, 0xAEU, 0xA1U, 0x01U, 0x02U, 0xA2U,
    };
    static constexpr std::array<uint8_t, 1> kUnk{0x55U};

    // onFrame 内重入
    s_sink.reentryDecs = kDecs;
    s_sink.reentryBuf = guardedSlice(kTestBufCap);
    s_sink.testReentryInFrame = true;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTwoFrames));
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryFeedErr);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryAbortErr);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryInitErr);
    // 重入 init 失败不破坏实例，第 2 帧照常完成
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().decoder[0].frames);

    // onUnknown 内重入
    s_sink.testReentryInFrame = false;
    s_sink.testReentryInUnknown = true;
    s_sink.reentryFeedErr = demux::Error::kOk;
    s_sink.reentryAbortErr = demux::Error::kOk;
    s_sink.reentryInitErr = demux::Error::kOk;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kUnk));
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryFeedErr);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryAbortErr);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryInitErr);

    // abort 的 onUnknown 回调内重入
    s_sink.reentryFeedErr = demux::Error::kOk;
    s_sink.reentryAbortErr = demux::Error::kOk;
    s_sink.reentryInitErr = demux::Error::kOk;
    TEST_ASSERT_EQUAL(demux::Error::kOk,
                      s_dmx.feed(std::span<const uint8_t>(kFrame).subspan(0, 2U)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryFeedErr);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryAbortErr);
    TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryInitErr);

    // abort 的 onFrame 回放回调内重入
    {
        static constexpr std::array<const demux::Decoder *, 2> kDecsConflict{&kDecAaP0, &kDecAaP1};
        static constexpr std::array<uint8_t, 4> kHalfP1{0xAAU, 0x22U, 0x99U, 0x44U};

        s_sink.testReentryInUnknown = false;
        s_sink.testReentryInFrame = true;
        s_sink.reentryDecs = kDecsConflict;
        s_sink.reentryFeedErr = demux::Error::kOk;
        s_sink.reentryAbortErr = demux::Error::kOk;
        s_sink.reentryInitErr = demux::Error::kOk;
        TEST_ASSERT_EQUAL(demux::Error::kOk,
                          s_dmx.init(kDecsConflict, guardedSlice(kTestBufCap), s_sink));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kHalfP1));
        TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
        TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryFeedErr);
        TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryAbortErr);
        TEST_ASSERT_EQUAL(demux::Error::kReentry, s_sink.reentryInitErr);
        TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[1].frames);
    }

    // 回调中调用 resetStats() 安全且不影响后续解析
    resetTestEnv();
    s_sink.resetStatsInFrame = true;
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kTwoFrames));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
}

static void testAbortBusyAndRecovery()
{
    static constexpr std::array<const demux::Decoder *, 3> kDecs{&kDecAaP0, &kDecAaP1, &kDecFixed4};
    static constexpr std::array<uint8_t, 5> kNestedInHalf{0xAAU, 0xA1U, 0x01U, 0x02U, 0xA2U};
    static constexpr std::array<uint8_t, 5> kReplayInAbort{0xAAU, 0x22U, 0x99U, 0x44U, 0x77U};
    static constexpr std::array<uint8_t, 2> kPartial{0xA1U, 0x07U};

    // 空闲 abort 不计数
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().aborts);

    // 纯半帧 abort
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kPartial));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().aborts);
    TEST_ASSERT_EQUAL_UINT(kPartial.size(), s_sink.unknownLen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kPartial.data(), s_sink.unknownStream.data(), kPartial.size());

    // 半帧内嵌套另一解码器完整帧
    resetTestEnv();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kNestedInHalf));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.unknownLen);
    TEST_ASSERT_EQUAL_HEX8(0xAAU, s_sink.unknownStream[0]);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT8(2U, s_sink.events[1].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events[1].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&kNestedInHalf[1], s_sink.events[1].data.data(), 4U);

    // 同首字节候选在 abort 时回放成帧
    resetTestEnv();
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kReplayInAbort));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.abort());
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL(EventKind::kFrame, s_sink.events[0].kind);
    TEST_ASSERT_EQUAL_UINT8(1U, s_sink.events[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecAaP1, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kReplayInAbort.data(), s_sink.events[0].data.data(), 4U);
    TEST_ASSERT_EQUAL(EventKind::kUnknown, s_sink.events[1].kind);
    TEST_ASSERT_EQUAL_UINT(1U, s_sink.unknownLen);
    TEST_ASSERT_EQUAL_HEX8(0x77U, s_sink.unknownStream[0]);
}

static void testResetStatsMidframeAndEightDecoders()
{
    static constexpr std::array<const demux::Decoder *, demux::kMaxDecoders> kDecs{
        &kDecSlot0, &kDecSlot1, &kDecSlot2, &kDecSlot3,
        &kDecSlot4, &kDecSlot5, &kDecSlot6, &kDecSlot7,
    };
    static constexpr std::array<uint8_t, 2> kF0{0xC0U, 0x3FU};
    static constexpr std::array<uint8_t, 2> kF7{0xC7U, 0x38U};
    static constexpr std::array<uint8_t, 2> kBad{0xC3U, 0x00U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kF0));
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(kBad));
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[3].rejects);
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().unknownBytes);

    // 喂入 slot7 第 1 字节，半帧期间 resetStats 必须保持解析状态
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kF7).subspan(0, 1U)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    s_dmx.resetStats();
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().decoder[3].rejects);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().resyncs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_dmx.stats().unknownBytes);

    // 喂入第 2 字节完成 slot7 帧
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(std::span<const uint8_t>(kF7).subspan(1U, 1U)));
    TEST_ASSERT_FALSE(s_dmx.busy());
    TEST_ASSERT_EQUAL_UINT32(1U, s_dmx.stats().decoder[7].frames);
    TEST_ASSERT_EQUAL_UINT32(2U, s_dmx.stats().decoder[7].bytes);
    TEST_ASSERT_EQUAL_UINT8(7U, s_sink.events[s_sink.eventCount - 1U].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecSlot7, s_sink.events[s_sink.eventCount - 1U].decoder);
}

static void testTwoInstancesIndependent()
{
    static constexpr std::array<const demux::Decoder *, 1> kDecs{&kDecFixed4};
    static constexpr std::array<uint8_t, 4> kFa{0xA1U, 0x01U, 0x02U, 0xA2U};
    static constexpr std::array<uint8_t, 4> kFb{0xA1U, 0x10U, 0x20U, 0x91U};
    demux::Demux dmxB;
    std::array<uint8_t, kTestBufCap> bufB{};
    TestSink sinkB;

    sinkB.reset(&dmxB);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.init(kDecs, guardedSlice(kTestBufCap), s_sink));
    TEST_ASSERT_EQUAL(demux::Error::kOk, dmxB.init(kDecs, bufB, sinkB));

    const std::span<const uint8_t> sa(kFa);
    const std::span<const uint8_t> sb(kFb);
    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(sa.subspan(0, 2U)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, dmxB.feed(sb.subspan(0, 2U)));
    TEST_ASSERT_TRUE(s_dmx.busy());
    TEST_ASSERT_TRUE(dmxB.busy());

    TEST_ASSERT_EQUAL(demux::Error::kOk, s_dmx.feed(sa.subspan(2U, 2U)));
    TEST_ASSERT_EQUAL(demux::Error::kOk, dmxB.feed(sb.subspan(2U, 2U)));

    TEST_ASSERT_EQUAL_UINT(1U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT(1U, sinkB.eventCount);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFa.data(), s_sink.events[0].data.data(), kFa.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kFb.data(), sinkB.events[0].data.data(), kFb.size());
}

static void testStaticDemux()
{
    using SingleDemux = demux::StaticDemux<kDecFixed4>;
    using PairDemux = demux::StaticDemux<kDecFixed4, kDecBulk>;
    using EightDemux = demux::StaticDemux<kDecSlot0, kDecSlot1, kDecSlot2, kDecSlot3, kDecSlot4,
                                          kDecSlot5, kDecSlot6, kDecSlot7>;

    // 编译期断言：kCount、kBufSize、indexOf
    static_assert(SingleDemux::kCount == 1U);
    static_assert(SingleDemux::kBufSize == kDecFixed4.maxFrame);
    static_assert(SingleDemux::indexOf<kDecFixed4>() == 0U);

    static_assert(PairDemux::kCount == 2U);
    static_assert(PairDemux::kBufSize == kDecBulk.maxFrame);
    static_assert(PairDemux::indexOf<kDecFixed4>() == 0U);
    static_assert(PairDemux::indexOf<kDecBulk>() == 1U);

    static_assert(EightDemux::kCount == demux::kMaxDecoders);
    static_assert(EightDemux::kBufSize == 2U);
    static_assert(EightDemux::indexOf<kDecSlot0>() == 0U);
    static_assert(EightDemux::indexOf<kDecSlot7>() == 7U);

    PairDemux sd;
    s_sink.reset(nullptr);
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.init(s_sink));
    TEST_ASSERT_FALSE(sd.busy());

    static constexpr std::array<uint8_t, 4> kF4{0xA1U, 0x03U, 0x04U, 0xA6U};
    static constexpr std::array<uint8_t, 5> kFbulk{0xA2U, 0x05U, 0x11U, 0x22U, 0xEEU};
    static constexpr std::array<uint8_t, 2> kHalf{0xA1U, 0x55U};

    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.feed(kF4));
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.feed(kFbulk));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT8(PairDemux::indexOf<kDecFixed4>(), s_sink.events[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecFixed4, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT8(PairDemux::indexOf<kDecBulk>(), s_sink.events[1].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecBulk, s_sink.events[1].decoder);

    TEST_ASSERT_EQUAL_UINT32(1U, sd.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL_UINT32(1U, sd.stats().decoder[1].frames);

    // 半帧 -> busy -> resetStats -> abort 转发验证
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.feed(kHalf));
    TEST_ASSERT_TRUE(sd.busy());
    sd.resetStats();
    TEST_ASSERT_TRUE(sd.busy());
    TEST_ASSERT_EQUAL_UINT32(0U, sd.stats().decoder[0].frames);
    TEST_ASSERT_EQUAL(demux::Error::kOk, sd.abort());
    TEST_ASSERT_FALSE(sd.busy());
    TEST_ASSERT_EQUAL_UINT32(1U, sd.stats().aborts);
    TEST_ASSERT_EQUAL_UINT32(2U, sd.stats().unknownBytes);

    // 8 槽 StaticDemux 转发验证
    EightDemux eight;
    s_sink.reset(nullptr);
    TEST_ASSERT_EQUAL(demux::Error::kOk, eight.init(s_sink));
    static constexpr std::array<uint8_t, 4> kSlots{0xC0U, 0x3FU, 0xC7U, 0x38U};
    TEST_ASSERT_EQUAL(demux::Error::kOk, eight.feed(kSlots));
    TEST_ASSERT_EQUAL_UINT(2U, s_sink.eventCount);
    TEST_ASSERT_EQUAL_UINT8(EightDemux::indexOf<kDecSlot0>(), s_sink.events[0].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecSlot0, s_sink.events[0].decoder);
    TEST_ASSERT_EQUAL_UINT8(EightDemux::indexOf<kDecSlot7>(), s_sink.events[1].decoderIndex);
    TEST_ASSERT_EQUAL_PTR(&kDecSlot7, s_sink.events[1].decoder);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testUninitializedAndConstinitGlobals);
    RUN_TEST(testInitValidationAndPoisonOnFail);
    RUN_TEST(testFeedEmptySpanAndZeroLen);
    RUN_TEST(testIdleUnknownAndDefaultOnUnknown);
    RUN_TEST(testBulkAccumulationAndSplitChunks);
    RUN_TEST(testSameFirstBytePriorityAndMidbufferReplay);
    RUN_TEST(testAllRejectResyncRecoversNestedFrame);
    RUN_TEST(testRejectCallbackCausesAndLastFlag);
    RUN_TEST(testContractViolationsAndOverflow);
    RUN_TEST(testReentryFromCallbacks);
    RUN_TEST(testAbortBusyAndRecovery);
    RUN_TEST(testResetStatsMidframeAndEightDecoders);
    RUN_TEST(testTwoInstancesIndependent);
    RUN_TEST(testStaticDemux);
    return UNITY_END();
}
