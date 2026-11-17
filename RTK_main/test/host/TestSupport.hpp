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
    int crcErrors = 0;
    int lengthErrors = 0;
    proto::FrameView last{};
    std::array<uint8_t, proto::kMaxPayload> payload{};
};

inline FeedResult feedAll(proto::FrameParser &parser, std::span<const uint8_t> data)
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
            res.crcErrors++;
            break;
        case proto::ParseResult::kLengthError:
            res.lengthErrors++;
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

    fw::Status write(core::SessionHandle handle, std::span<const uint8_t> data,
                     uint32_t timeoutMs) noexcept override
    {
        lastTimeoutMs = timeoutMs;
        if (failWrites) {
            return fw::Status::kTimeout;
        }
        if (writeCount < kMaxRecords) {
            Write &rec = writes[writeCount];
            rec.handle = handle;
            rec.len = data.size();
            for (size_t i = 0; i < data.size() && i < rec.data.size(); ++i) {
                rec.data[i] = data[i];
            }
        }
        writeCount++;
        return fw::Status::kOk;
    }

    fw::Status disconnect(core::SessionHandle handle) noexcept override
    {
        if (disconnectCount < kMaxRecords) {
            disconnects[disconnectCount] = handle;
        }
        disconnectCount++;
        return fw::Status::kOk;
    }

    void clear()
    {
        writeCount = 0;
        disconnectCount = 0;
    }

    std::array<Write, kMaxRecords> writes{};
    size_t writeCount = 0;
    std::array<core::SessionHandle, kMaxRecords> disconnects{};
    size_t disconnectCount = 0;
    bool failWrites = false;
    uint32_t lastTimeoutMs = 0;
};

} // namespace test
