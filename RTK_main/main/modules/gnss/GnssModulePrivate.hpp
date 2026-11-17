// GNSS 模块内部契约：组件实现间共用的小工具与常量。不属于公共头，只有 modules/gnss/*.cpp 包含。
// 线程：全部 inline 纯函数，由 kIo 任务或启动前单线程调用；不阻塞、不分配堆、非 ISR。

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "def/GnssSubDef.hpp"
#include "demux/Demux.hpp"
#include "fw/Status.hpp"
#include "fw/Trace.hpp"

namespace app::detail {

/** 逐字节掩码（32 位值取低 8 位）。 */
inline constexpr uint32_t kByteMask = 0xFFU;
/** 每轮 poll 最多落地的控制器动作数（防止一次唤醒里长时间占住 kIo 任务）。 */
inline constexpr size_t kMaxActionsPerPoll = 16U;
/** 接受的最慢/最快主机波特率（低于它无法承载 NMEA + RTCM）。 */
inline constexpr uint32_t kBaudMin = 1200U;
inline constexpr uint32_t kBaudMax = 2000000U;
/** 输出/频率变更等待控制器落地的上限（毫秒）。 */
inline constexpr uint32_t kApplyTimeoutMs = 5000U;
/** 读回行缓冲：UnicoreResp 单帧上限 256 B，正文必小于它（ADR-0012 §2.7）。 */
inline constexpr size_t kReadbackLineBytes = 256U;

// kGnss 子事件包装：枚举 → uint8_t 载荷、session 恒 0（ADR-0009），参数求值昂贵时调用方先用 fw::traceOn()。
/**
 * @brief 记录一条 GNSS 子类追踪事件
 * @param ev: 事件本体
 * @param v0: v0 见调用点
 * @param v1: v1 见调用点
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
inline void traceGnssSub(GnssSubTrace ev, uint32_t v0, uint32_t v1) noexcept
{
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(ev), 0U, v0, v1);
}

/**
 * @brief 把拒绝原因码翻成文本
 * @param cause: 拒绝原因码
 * @retval 静态文本，不为空
 */
inline const char *rejectCauseName(demux::RejectCause cause) noexcept
{
    switch (cause) {
    case demux::RejectCause::kTooLong:
        return "frame too long";
    case demux::RejectCause::kContract:
        return "invalid length field";
    case demux::RejectCause::kAbort:
        return "aborted";
    case demux::RejectCause::kStep:
    default:
        return "rejected";
    }
}

/** `GNSS ` + 帧标识 + 长度文本的整行缓冲。 */
inline constexpr size_t kLogTagBytes = 48U;
/** 拒绝原因文本缓冲（explain 输出在此缓冲内截断）。 */
inline constexpr size_t kRejectReasonBytes = 96U;

/**
 * @brief 判断输出频率是否在允许范围内
 * @param hz: hz 见调用点
 * @retval true 表示成功/命中
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
inline bool isValidRateHz(uint8_t hz) noexcept
{
    constexpr std::array<uint8_t, 5> kRates{1U, 2U, 5U, 10U, 20U};
    return std::find(kRates.begin(), kRates.end(), hz) != kRates.end();
}

/**
 * @brief 把控制器结果码转成框架错误码
 * @param s: s 见调用点
 * @retval kOk 表示成功，其他为失败原因
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
inline fw::Result toResult(fw::Status s) noexcept
{
    switch (s) {
    case fw::Status::kOk:
        return fw::Result::kOk;
    case fw::Status::kInvalidArg:
        return fw::Result::kInvalidArg;
    case fw::Status::kNoSpace:
        return fw::Result::kNoSpace;
    case fw::Status::kTimeout:
        return fw::Result::kTimeout;
    case fw::Status::kNotFound:
        return fw::Result::kNotFound;
    case fw::Status::kNotSupported:
        return fw::Result::kNotSupported;
    case fw::Status::kBusy:
        return fw::Result::kBusy;
    case fw::Status::kInvalidState:
        return fw::Result::kInvalidState;
    case fw::Status::kCorrupt:
    case fw::Status::kIoError:
    default:
        return fw::Result::kIoError;
    }
}

/**
 * @brief 按小端读出 4 字节无符号数
 * @param in: 输入字节
 * @retval 按小端读出的数值
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
inline uint32_t readU32(std::span<const uint8_t> in) noexcept
{
    uint32_t out = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        // in.size() >= 4 已在调用前校验；i < 4 左移不会超出 32 位。
        out |= static_cast<uint32_t>(in[i]) << (8U * i);
    }
    return out;
}

/**
 * @brief 按小端写入 4 字节无符号数
 * @param value: 值类型
 * @param out: 输出缓冲
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
inline void writeU32(uint32_t value, std::span<uint8_t> out) noexcept
{
    for (size_t i = 0U; i < 4U; ++i) {
        // out.size() >= 4 已在调用前校验；取字节无窄化丢失。
        out[i] = static_cast<uint8_t>((value >> (8U * i)) & kByteMask);
    }
}

/**
 * @brief 登记解码器（重复则跳过）
 * @param out: 输出缓冲
 * @param count: count 见调用点
 * @param dec: dec 见调用点
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
inline void addUniqueDecoder(std::array<const demux::Decoder *, demux::kMaxDecoders> &out,
                             size_t &count, const demux::Decoder *dec) noexcept
{
    if ((dec == nullptr) || (count >= out.size())) {
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (out[i] == dec) {
            return;
        }
    }
    out[count] = dec;
    ++count;
}

} // namespace app::detail
