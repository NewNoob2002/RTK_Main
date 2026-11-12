// 冻结接口（ADR-0005）。实现方不得修改；变更须先修订 ADR-0005。
// Generated-by: Claude Opus 5.5（架构/接口），人工负责人：UNKNOWN（待人工审查签字）
//
// 解码器描述符：一个协议 = 一个无状态的 constexpr 描述符（两个纯函数 + 元数据）。
// 引擎（demux.hpp）只依赖本类型，不理解任何具体协议；增加协议 = 增加一个头文件 + 一个源文件。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rtk::demux {

/** 解码器 step 的结论。 */
enum class Step : uint8_t {
    kNeedMore, // 还不能下结论；need 写入下次调用前至少还要追加的字节数（≥1）
    kFrame,    // frame 恰好是一条完整且校验通过的消息
    kReject,   // frame 不是本协议的有效前缀（同步/长度/校验失败）
};

/**
 * 解码器描述符。以 `inline constexpr` 变量定义在各解码器头文件中，可被多个实例共享；解码器无可变状态。
 * 对象地址即身份：回调里用 `&decoder == &rtk::demux::rtcm3::kDecoder` 判断协议（inline 变量全程序唯一地址）。
 *
 * step 契约（引擎依赖这些性质保证回放重扫正确，见 ADR-0005 §3 / ADR-0004 §3）：
 * - 纯函数：结论只取决于 frame 的内容，不依赖之前的调用；可对同一前缀重复调用。
 * - 只在 frame.size() ≥ 1 且 frame[0] 通过 first_byte 时被调用。
 * - 返回 kNeedMore 时 need ≥ 1，且 frame.size() + need ≤ max_frame；否则视为实现错误，引擎按 kReject 处理。
 *   need 用于批量累积：长度已知后一次要求“剩余全部字节”，CRC 在帧尾整段计算。不知道还要多少时写 1。
 * - 返回 kFrame 表示 frame 全部属于该消息（不多不少）。返回 kFrame/kReject 时 need 的值被忽略。
 * - 尽早 kReject：同步字节、长度字段一旦不符立即拒绝，让同首字节的其他解码器尝试、减少回放量。
 * 两个函数都必须 noexcept、可重入、不访问全局可变状态（ISR 安全）。
 */
struct Decoder {
    using FirstByteFn = bool (*)(uint8_t byte) noexcept;
    using StepFn = Step (*)(std::span<const uint8_t> frame, std::size_t &need) noexcept;

    const char *name;       // 诊断用，非空
    std::size_t max_frame;  // 本协议允许的最大帧长（字节），1..65535；超长的长度字段应在 step 中 kReject
    FirstByteFn first_byte; // 该字节能否作为本协议帧的第一个字节；只在 Demux::init 时对 0..255 各调用一次
    StepFn step;
};

} // namespace rtk::demux
