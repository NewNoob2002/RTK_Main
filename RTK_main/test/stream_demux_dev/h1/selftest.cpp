// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
// H1 可选自测；整流 oracle 直接按协议字节规则分类，不复用引擎的回放算法。
#include "rtk/demux/demux.hpp"
#include "rtk/demux/static_demux.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

void check(bool passed)
{
    assert(passed);
}

std::size_t s_first_calls = 0;
std::size_t s_step_calls = 0;
std::size_t s_bad_need = 0;
uint32_t s_seed = 0x17C0FFEEU;

bool first_bang(uint8_t byte) noexcept
{
    ++s_first_calls;
    return byte == '!';
}

bool first_question(uint8_t byte) noexcept
{
    ++s_first_calls;
    return byte == '?';
}

bool first_star(uint8_t byte) noexcept
{
    ++s_first_calls;
    return byte == '*';
}

rtk::demux::Step long_step(std::span<const uint8_t> bytes, std::size_t &need) noexcept
{
    ++s_step_calls;
    if (bytes.size() < 6) {
        need = 6 - bytes.size();
        return rtk::demux::Step::kNeedMore;
    }
    return bytes[5] == 'T' ? rtk::demux::Step::kFrame : rtk::demux::Step::kReject;
}

rtk::demux::Step short_step(std::span<const uint8_t> bytes, std::size_t &need) noexcept
{
    ++s_step_calls;
    if (bytes.size() == 1) {
        need = 1;
        return rtk::demux::Step::kNeedMore;
    }
    return bytes[1] == 'q' ? rtk::demux::Step::kFrame : rtk::demux::Step::kReject;
}

rtk::demux::Step question_step(std::span<const uint8_t> bytes, std::size_t &need) noexcept
{
    ++s_step_calls;
    if (bytes.size() == 1) {
        need = 1;
        return rtk::demux::Step::kNeedMore;
    }
    if (bytes[1] != 'x') {
        return rtk::demux::Step::kReject;
    }
    if (bytes.size() == 2) {
        need = 2;
        return rtk::demux::Step::kNeedMore;
    }
    return bytes[3] == 'Z' ? rtk::demux::Step::kFrame : rtk::demux::Step::kReject;
}

rtk::demux::Step star_step(std::span<const uint8_t>, std::size_t &) noexcept
{
    ++s_step_calls;
    return rtk::demux::Step::kFrame;
}

rtk::demux::Step bad_step(std::span<const uint8_t>, std::size_t &need) noexcept
{
    need = s_bad_need;
    return rtk::demux::Step::kNeedMore;
}

constexpr rtk::demux::Decoder kLong{"long", 6, first_bang, long_step};
constexpr rtk::demux::Decoder kShort{"short", 2, first_bang, short_step};
constexpr rtk::demux::Decoder kQuestion{"question", 4, first_question, question_step};
constexpr rtk::demux::Decoder kStar{"star", 1, first_star, star_step};
constexpr std::array<const rtk::demux::Decoder *, 4> kDecoders{&kLong, &kShort, &kQuestion, &kStar};
constinit rtk::demux::Demux s_global;

class Capture final : public rtk::demux::Sink {
public:
    rtk::demux::Demux *dmx = nullptr;
    std::span<const uint8_t> input{};
    std::array<uint8_t, 1024> tags{};
    std::size_t used = 0;
    std::size_t frames = 0;
    std::size_t unknown_calls = 0;

    void on_frame(const rtk::demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> bytes) noexcept override
    {
        check(index < kDecoders.size());
        check(&decoder == kDecoders[index]);
        // index 范围为 0..3，加一后为 1..4，收窄无损。
        capture(bytes, static_cast<uint8_t>(index + 1U));
        ++frames;
    }

    void on_unknown(std::span<const uint8_t> bytes) noexcept override
    {
        capture(bytes, 0);
        ++unknown_calls;
    }

private:
    void capture(std::span<const uint8_t> bytes, uint8_t tag) noexcept
    {
        check(!bytes.empty());
        check(bytes.size() <= input.size() - used);
        check(std::memcmp(bytes.data(), input.data() + used, bytes.size()) == 0);
        std::fill_n(tags.begin() + used, bytes.size(), tag);
        used += bytes.size();
        check(dmx->feed({}) == rtk::demux::Error::kReentry);
        check(dmx->abort() == rtk::demux::Error::kReentry);
        check(dmx->init({}, {}, *this) == rtk::demux::Error::kReentry);
        check(std::memcmp(bytes.data(), input.data() + used - bytes.size(), bytes.size()) == 0);
    }
};

uint32_t random_word()
{
    s_seed ^= s_seed << 13U;
    s_seed ^= s_seed >> 17U;
    s_seed ^= s_seed << 5U;
    return s_seed;
}

std::array<uint8_t, 1024> oracle(std::span<const uint8_t> input)
{
    std::array<uint8_t, 1024> tags{};
    for (std::size_t pos = 0; pos < input.size();) {
        const std::size_t remaining = input.size() - pos;
        std::size_t count = 1;
        uint8_t tag = 0;
        if (input[pos] == '!' && remaining >= 6 && input[pos + 5] == 'T') {
            count = 6;
            tag = 1;
        } else if (input[pos] == '!' && remaining >= 2 && input[pos + 1] == 'q') {
            count = 2;
            tag = 2;
        } else if (input[pos] == '?' && remaining >= 4 && input[pos + 1] == 'x' && input[pos + 3] == 'Z') {
            count = 4;
            tag = 3;
        } else if (input[pos] == '*') {
            tag = 4;
        }
        std::fill_n(tags.begin() + pos, count, tag);
        pos += count;
    }
    return tags;
}

void run_stream(std::span<const uint8_t> input, const std::array<uint8_t, 1024> &expected, std::size_t mode)
{
    rtk::demux::Demux dmx;
    std::array<uint8_t, 10> guarded{0xC1, 0, 0, 0, 0, 0, 0, 0, 0, 0xD2};
    Capture sink;
    sink.dmx = &dmx;
    sink.input = input;
    check(dmx.init(kDecoders, std::span{guarded}.subspan(1, 8), sink) == rtk::demux::Error::kOk);
    check(s_first_calls >= kDecoders.size() * 256);
    s_first_calls = 0;
    for (std::size_t pos = 0; pos < input.size();) {
        const std::size_t requested = mode == 0 ? input.size() : (mode == 1 ? 1 : random_word() % 17U + 1U);
        const std::size_t count = std::min(requested, input.size() - pos);
        check(dmx.feed({}) == rtk::demux::Error::kOk);
        check(dmx.feed(input.subspan(pos, count)) == rtk::demux::Error::kOk);
        pos += count;
    }
    check(dmx.abort() == rtk::demux::Error::kOk);
    check(!dmx.busy());
    check(sink.used == input.size());
    check(sink.tags == expected);
    check(guarded.front() == 0xC1 && guarded.back() == 0xD2);
    check(s_first_calls == 0);
    std::size_t total = dmx.stats().unknown_bytes;
    for (const auto &stats : dmx.stats().decoder) {
        total += stats.bytes;
    }
    check(total == input.size());
}

void check_api()
{
    rtk::demux::Demux dmx;
    std::array<uint8_t, 8> buf{};
    Capture sink;
    sink.dmx = &dmx;
    check(s_global.feed({}) == rtk::demux::Error::kInvalidArg);
    check(s_global.abort() == rtk::demux::Error::kInvalidArg);
    check(dmx.feed({}) == rtk::demux::Error::kInvalidArg);
    check(dmx.abort() == rtk::demux::Error::kInvalidArg);
    check(!dmx.busy());
    check(dmx.init({}, buf, sink) == rtk::demux::Error::kInvalidArg);
    std::array<const rtk::demux::Decoder *, 9> table{};
    table.fill(&kLong);
    check(dmx.init(table, buf, sink) == rtk::demux::Error::kInvalidArg);
    table[0] = nullptr;
    check(dmx.init(std::span{table}.first(1), buf, sink) == rtk::demux::Error::kInvalidArg);
    rtk::demux::Decoder invalid = kLong;
    table[0] = &invalid;
    invalid.name = nullptr;
    check(dmx.init(std::span{table}.first(1), buf, sink) == rtk::demux::Error::kInvalidArg);
    invalid = kLong;
    invalid.first_byte = nullptr;
    check(dmx.init(std::span{table}.first(1), buf, sink) == rtk::demux::Error::kInvalidArg);
    invalid = kLong;
    invalid.step = nullptr;
    check(dmx.init(std::span{table}.first(1), buf, sink) == rtk::demux::Error::kInvalidArg);
    invalid = kLong;
    for (std::size_t max_frame : {std::size_t{0}, std::size_t{65536}, SIZE_MAX}) {
        invalid.max_frame = max_frame;
        check(dmx.init(std::span{table}.first(1), buf, sink) == rtk::demux::Error::kInvalidArg);
    }
    check(dmx.init(kDecoders, std::span{buf}.first(5), sink) == rtk::demux::Error::kInvalidArg);
    check(dmx.init(kDecoders, {}, sink) == rtk::demux::Error::kInvalidArg);
    s_first_calls = 0;
    check(dmx.init(kDecoders, buf, sink) == rtk::demux::Error::kOk);
    check(s_first_calls == 4 * 256);
    constexpr std::array<uint8_t, 5> kInput{'a', 'b', '*', '!', 'q'};
    sink.input = kInput;
    check(dmx.feed(kInput) == rtk::demux::Error::kOk);
    check(sink.unknown_calls == 1 && sink.frames == 1 && dmx.busy());
    check(dmx.abort() == rtk::demux::Error::kOk);
    check(sink.frames == 2 && dmx.stats().aborts == 1);
    check(dmx.stats().decoder[0].rejects == 1 && dmx.stats().decoder[1].frames == 1);
    check(dmx.stats().unknown_bytes == 2);
    check(dmx.abort() == rtk::demux::Error::kOk && dmx.stats().aborts == 1);
    sink.used = 0;
    check(dmx.init(kDecoders, buf, sink) == rtk::demux::Error::kOk);
    check(dmx.feed(kInput) == rtk::demux::Error::kOk);
    dmx.reset_stats();
    check(dmx.busy() && dmx.stats().unknown_bytes == 0);
    check(dmx.abort() == rtk::demux::Error::kOk);
    check(dmx.init({}, buf, sink) == rtk::demux::Error::kInvalidArg);
    check(!dmx.busy() && dmx.stats().aborts == 0);
    check(dmx.feed(kInput) == rtk::demux::Error::kInvalidArg && dmx.abort() == rtk::demux::Error::kInvalidArg);
}

void check_bad_need()
{
    rtk::demux::Demux dmx;
    std::array<uint8_t, 8> buf{};
    constexpr std::array<uint8_t, 1> kInput{'!'};
    Capture sink;
    sink.dmx = &dmx;
    sink.input = kInput;
    constexpr rtk::demux::Decoder kBad{"bad", 2, first_bang, bad_step};
    const std::array<const rtk::demux::Decoder *, 1> table{&kBad};
    for (std::size_t need : {std::size_t{0}, std::size_t{2}, std::size_t{8}, SIZE_MAX}) {
        s_bad_need = need;
        sink.used = 0;
        check(dmx.init(table, buf, sink) == rtk::demux::Error::kOk);
        check(dmx.feed(kInput) == rtk::demux::Error::kOk);
        check(!dmx.busy());
        check(dmx.stats().decoder[0].rejects == 1 && dmx.stats().resyncs == 1);
        check(dmx.stats().unknown_bytes == 1);
        check(dmx.stats().overflows == (need >= 8 ? 1U : 0U));
    }
}

void check_batching()
{
    rtk::demux::Demux dmx;
    std::array<uint8_t, 8> buf{};
    constexpr std::array<uint8_t, 6> kInput{'!', 'q', '*', '*', '*', 'T'};
    Capture sink;
    sink.dmx = &dmx;
    sink.input = kInput;
    check(dmx.init(kDecoders, buf, sink) == rtk::demux::Error::kOk);
    s_step_calls = 0;
    for (std::size_t pos = 0; pos < kInput.size(); ++pos) {
        check(dmx.feed(std::span{kInput}.subspan(pos, 1)) == rtk::demux::Error::kOk);
    }
    check(s_step_calls == 2 && sink.frames == 1);
}

class DefaultSink final : public rtk::demux::Sink {
public:
    void on_frame(const rtk::demux::Decoder &, uint8_t index, std::span<const uint8_t> bytes) noexcept override
    {
        last_index = index;
        check(bytes.size() == 1 && bytes[0] == '*');
    }
    uint8_t last_index = 0;
};

void check_eighth_candidate()
{
    rtk::demux::Demux dmx;
    std::array<uint8_t, 6> buf{};
    std::array<const rtk::demux::Decoder *, 8> table{};
    table.fill(&kLong);
    table.back() = &kStar;
    DefaultSink sink;
    constexpr std::array<uint8_t, 3> kInput{'a', '*', 'b'};
    check(dmx.init(table, buf, sink) == rtk::demux::Error::kOk);
    check(dmx.feed(kInput) == rtk::demux::Error::kOk);
    check(sink.last_index == 7 && dmx.stats().decoder[7].frames == 1 && dmx.stats().unknown_bytes == 2);
    rtk::demux::StaticDemux<kStar> fixed;
    static_assert(decltype(fixed)::kBufSize == 1 && decltype(fixed)::index_of<kStar>() == 0);
    check(fixed.init(sink) == rtk::demux::Error::kOk);
    check(fixed.feed(kInput) == rtk::demux::Error::kOk);
    check(fixed.abort() == rtk::demux::Error::kOk && !fixed.busy());
}

} // namespace

int main()
{
    check_api();
    check_bad_need();
    check_batching();
    check_eighth_candidate();
    constexpr std::array<uint8_t, 14> kReplay{'!', 'q', '*', '?', 'x', 'a', 'Z', '!', 'q', '?', '!', 'q', '*', '?'};
    for (std::size_t mode = 0; mode < 3; ++mode) {
        run_stream(kReplay, oracle(kReplay), mode);
    }
    constexpr std::array<uint8_t, 10> kAlphabet{'!', 'q', '?', 'x', 'Z', '*', 'T', 0, 'a', 'b'};
    std::array<uint8_t, 1024> input{};
    for (std::size_t trial = 0; trial < 1000; ++trial) {
        const std::size_t size = random_word() % input.size();
        for (std::size_t pos = 0; pos < size; ++pos) {
            input[pos] = kAlphabet[random_word() % kAlphabet.size()];
        }
        const std::span<const uint8_t> bytes{input.data(), size};
        const auto expected = oracle(bytes);
        for (std::size_t mode = 0; mode < 3; ++mode) {
            run_stream(bytes, expected, mode);
        }
    }
    std::puts("H1 selftest PASS: API, replay, abort, reentry, invalid need, batching, 1000 streams x 3 chunk modes");
}
