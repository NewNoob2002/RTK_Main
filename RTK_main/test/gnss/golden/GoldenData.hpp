// 黄金向量的数据形状：由 Golden*.inc 以聚合初始化填充，测试逐项与驱动行为比对（ADR-0012 §2.4）。
// 本头文件只声明纯数据与解码器枚举，不含任何断言逻辑；可在任意 host 测试中包含。
// 线程/ISR 安全：全是 constexpr 数据，只读。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace golden {

/** 黄金向量里出现过的解码器；测试按此名映射到实际 demux::Decoder 对象。 */
enum class DecoderKind : uint8_t {
    kNmea = 0,
    kRtcm3,
    kUnicoreHash,
    kUbx,
    kUnicoreResp,
    kComnavPrompt,
};

/**
 * 定长字节串：由字符串字面量构造并保留字面量的完整长度。
 * 不能用 std::string_view 直接存字面量——二进制帧含 0x00，隐式 strlen 会在第一个 0 处截断。
 */
struct Bytes {
    const char *text = nullptr;
    std::size_t size = 0;

    constexpr Bytes() noexcept = default;

    template <std::size_t N>
    constexpr Bytes(const char (&literal)[N]) noexcept : text(literal), size(N - 1)
    {}

    // 不是 constexpr：指针到 uint8_t* 的转换不允许出现在常量表达式里。
    [[nodiscard]] std::span<const uint8_t> view() const noexcept
    {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text), size);
    }

    [[nodiscard]] constexpr std::string_view sv() const noexcept
    {
        return std::string_view(text, size);
    }
};

/** 探测命令：ok=false 表示 buildProbe() 当时返回 false。 */
struct Probe {
    Bytes bytes{};
    bool ok = false;
};

/** 配置脚本中的一步（对应 gnss::ConfigStep）。 */
struct Step {
    Bytes bytes{};
    bool expectReply = true;
    uint16_t timeoutMs = 0;
    uint32_t switchBaud = 0;
};

/**
 * 一组 roverStep 入参（型号 × rate × gsv × port × baud）及其完整脚本。
 * steps 指向同一文件里的 kSteps，[stepFirst, stepFirst + stepCount) 是本组合的步骤。
 */
struct RoverCase {
    uint8_t model = 0; // gnss::Model 的数值
    uint8_t rateHz = 0;
    bool gsv = false;
    uint8_t port = 0;
    uint32_t portBaud = 0;
    uint16_t stepFirst = 0;
    uint8_t stepCount = 0;
};

/** 一个探测应答样本。fw 仅在 hit 为 true 时比较。 */
struct ProbeMatchCase {
    DecoderKind decoder = DecoderKind::kNmea;
    Bytes frame{};
    bool hit = false;
    uint8_t model = 0; // gnss::Model 的数值
    Bytes fw{};
};

/** 一个应答判定样本：已发送的 sent 与收到的 frame 应判定为 ack（gnss::Ack 的数值）。 */
struct ReplyCase {
    DecoderKind decoder = DecoderKind::kNmea;
    Bytes frame{};
    Bytes sent{};
    uint8_t ack = 0;
};

/** 能力位样本。 */
struct CapsCase {
    uint8_t model = 0;
    uint32_t caps = 0;
};

} // namespace golden
