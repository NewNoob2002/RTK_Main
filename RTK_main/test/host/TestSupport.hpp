// 测试辅助（冻结输入）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/Transport.hpp"
#include "proto/Frame.hpp"

namespace test {

// 把整段字节喂给解析器，统计结果；最后一帧的 payload 拷贝出来（视图会在下次 feed 时失效）。
struct FeedResult {
    int frames = 0;
    int crc_errors = 0;
    int length_errors = 0;
    proto::FrameView last{};
    std::array<uint8_t, proto::kMaxPayload> payload{};
};

inline FeedResult feed_all(proto::FrameParser &parser, std::span<const uint8_t> data)
{
    FeedResult res{};
    for (uint8_t byte : data) {
        proto::FrameView frame{};
        switch (parser.feed(byte, frame)) {
        case proto::ParseResult::kFrame:
            res.frames++;
            res.last = frame;
            for (size_t i = 0; i < frame.payload.size(); ++i) {
                res.payload[i] = frame.payload[i];
            }
            break;
        case proto::ParseResult::kCrcError:
            res.crc_errors++;
            break;
        case proto::ParseResult::kLengthError:
            res.length_errors++;
            break;
        case proto::ParseResult::kNone:
        default:
            break;
        }
    }
    return res;
}

// 记录写入与断开的假传输。
class FakeTransport final : public core::Transport {
public:
    struct Write {
        core::SessionHandle handle = 0;
        std::array<uint8_t, proto::kMaxFrame> data{};
        size_t len = 0;
    };
    static constexpr size_t kMaxRecords = 32;

    fw::Status write(core::SessionHandle handle, std::span<const uint8_t> data, uint32_t timeout_ms) noexcept override
    {
        last_timeout_ms = timeout_ms;
        if (fail_writes) {
            return fw::Status::kTimeout;
        }
        if (write_count < kMaxRecords) {
            Write &rec = writes[write_count];
            rec.handle = handle;
            rec.len = data.size();
            for (size_t i = 0; i < data.size() && i < rec.data.size(); ++i) {
                rec.data[i] = data[i];
            }
        }
        write_count++;
        return fw::Status::kOk;
    }

    fw::Status disconnect(core::SessionHandle handle) noexcept override
    {
        if (disconnect_count < kMaxRecords) {
            disconnects[disconnect_count] = handle;
        }
        disconnect_count++;
        return fw::Status::kOk;
    }

    void clear()
    {
        write_count = 0;
        disconnect_count = 0;
    }

    std::array<Write, kMaxRecords> writes{};
    size_t write_count = 0;
    std::array<core::SessionHandle, kMaxRecords> disconnects{};
    size_t disconnect_count = 0;
    bool fail_writes = false;
    uint32_t last_timeout_ms = 0;
};

} // namespace test
