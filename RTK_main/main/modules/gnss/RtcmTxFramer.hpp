// RTCM3 串口发送帧边界跟踪器：保证 UBX/ASCII 配置写操作仅插入在完整 RTCM3 帧边界（RU1-07）。
// 线程安全：否（由 kIo 单任务独占）。ISR 安全：是（纯字节状态机，零堆分配、不阻塞）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace app {

/** 跟踪已写入 UART 的 RTCM3 字节流帧边界（0xD3 + 10 位长度 + 载荷 + 3 字节 CRC24Q）。 */
class RtcmTxFramer final {
public:
    static constexpr uint8_t kPreamble = 0xD3U;
    static constexpr uint8_t kReservedMask = 0xFCU;
    static constexpr uint8_t kLenHighMask = 0x03U;
    static constexpr size_t kCrcBytes = 3U;
    static constexpr unsigned kLenHighShift = 8U;

    /** @brief 复位为帧边界空闲态（丢弃管道或串口写错时调用）。 */
    void reset() noexcept
    {
        state_ = State::kIdle;
        len_hi_ = 0U;
        remaining_ = 0U;
    }

    /** @brief 查询当前是否处于完整 RTCM3 帧边界（可安全插入 UBX/ASCII 命令）。 @retval true 已对齐帧边界。 */
    [[nodiscard]] bool atFrameBoundary() const noexcept
    {
        return state_ == State::kIdle;
    }

    /**
     * @brief 用已实际写入 UART 的 RTCM 字节推进帧边界状态。
     * @param written: 本次已写入串口的字节切片。
     */
    void onBytesWritten(std::span<const uint8_t> written) noexcept
    {
        for (const uint8_t byte : written) {
            step(state_, len_hi_, remaining_, byte);
        }
    }

    /**
     * @brief 计算从待发缓冲中需要写多少前导字节才能结束当前未完 RTCM3 帧（不启动下一帧）。
     * @param pending: 待发送的 RTCM 字节切片。
     * @retval 结束当前帧所需字节数；若已在帧边界返回 0，若整段均属于当前帧则返回 pending.size()。
     */
    [[nodiscard]] size_t bytesToFinishCurrentFrame(std::span<const uint8_t> pending) const noexcept
    {
        if (state_ == State::kIdle) {
            return 0U;
        }
        State simState = state_;
        uint8_t simLenHi = len_hi_;
        size_t simRem = remaining_;
        size_t count = 0U;
        while ((count < pending.size()) && (simState != State::kIdle)) {
            step(simState, simLenHi, simRem, pending[count]);
            ++count;
        }
        return count;
    }

private:
    enum class State : uint8_t {
        kIdle = 0U,
        kHdr1,
        kHdr2,
        kBody,
    };

    /**
     * @brief 单字节推进 RTCM3 帧解析状态。
     * @param state: 当前状态（入参与输出）。
     * @param lenHi: 10 位长度高 2 位暂存（入参与输出）。
     * @param remaining: 当前帧剩余载荷+CRC 字节数（入参与输出）。
     * @param byte: 输入字节。
     */
    static void step(State &state, uint8_t &lenHi, size_t &remaining, uint8_t byte) noexcept
    {
        switch (state) {
        case State::kIdle:
            if (byte == kPreamble) {
                state = State::kHdr1;
            }
            break;
        case State::kHdr1:
            if ((byte & kReservedMask) == 0U) {
                lenHi = static_cast<uint8_t>(byte & kLenHighMask);
                state = State::kHdr2;
            } else if (byte != kPreamble) {
                state = State::kIdle;
            }
            break;
        case State::kHdr2: {
            const size_t payloadLen =
                (static_cast<size_t>(lenHi) << kLenHighShift) | static_cast<size_t>(byte);
            remaining = payloadLen + kCrcBytes;
            state = State::kBody;
            break;
        }
        case State::kBody:
            if (remaining > 0U) {
                --remaining;
            }
            if (remaining == 0U) {
                state = State::kIdle;
            }
            break;
        default:
            state = State::kIdle;
            break;
        }
    }

    State state_ = State::kIdle;
    uint8_t len_hi_ = 0U;
    size_t remaining_ = 0U;
};

} // namespace app
