// 网页配置 WebSocket 协议的错误码（docs/web/ws-protocol.md §5）；纯 C++20，无 IDF/FreeRTOS 依赖。
// 无状态、不阻塞、不分配堆；任意任务或 ISR 均可调用（只读静态表，无锁）。
#pragma once

#include <cstdint>

#include "fw/Result.hpp"

namespace web {

/** 协议 §5 的错误码；ack 帧的 err 字段即 toString() 的返回值。 */
enum class ErrorCode : uint8_t {
    kNone = 0,
    kBadJson,
    kTooLong,
    kUnknownKey,
    kBadValue,
    kLocked,
    kBusy,
    kUnsupported,
    kFailed,
    kTimeout,
};

/**
 * @brief  返回错误码的协议字符串（"bad_json"…）。
 * @param  code: 待转换的错误码。
 * @retval 静态字符串；未知值返回 "failed"
 * @note   线程/ISR 安全；返回静态存储，不分配堆。
 */
[[nodiscard]] const char *toString(ErrorCode code) noexcept;

/**
 * @brief  把模块命令结果映射为协议错误码（ack.err）。
 * @param  result: 模块返回的 fw::Result。
 * @retval kNone        成功（Result::kOk）
 * @retval kBadValue    Result::kInvalidArg（模块拒绝该值）
 * @retval kBusy        Result::kBusy
 * @retval kUnsupported Result::kNotSupported
 * @retval kTimeout     Result::kTimeout
 * @retval kFailed      其余可恢复错误（空间、IO、状态、未找到）
 * @note   纯映射，无状态；线程/ISR 安全。
 */
[[nodiscard]] ErrorCode fromResult(fw::Result result) noexcept;

} // namespace web
