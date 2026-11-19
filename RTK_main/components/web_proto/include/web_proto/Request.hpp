// 浏览器 → 设备 WebSocket 请求帧的解析接口（docs/web/ws-protocol.md §3）。
// parseRequest 由网页模块任务调用；不阻塞、非 ISR 安全、不分配堆。
// 字段视图有两种归属：未转义字段借用调用方的输入帧，含转义字段借用 Request 的内部缓冲。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "web_proto/Error.hpp"

namespace web {

/** 单帧上限（协议 §1）：超过即 kTooLong，不再解析。 */
constexpr size_t kMaxFrameBytes = 1024;

/** 请求类型；对应协议 `t` 字段。 */
enum class RequestType : uint8_t { kSet, kCmd, kUnlock };

/**
 * 解析后的请求。
 *
 * 视图寿命约束（Architect 2026-11-19 决定，双重存储）：
 * - 帧内没有转义的字段（key/value/name/token）直接指向 parseRequest 的输入帧；
 * - 含 JSON 转义的字段解码到本对象的内部缓冲。
 * 因此调用方必须保证：
 * 1. 输入帧在视图使用期间存活且内容不变（复用接收缓冲会改变原始视图）；
 * 2. 对同一对象再次调用 parseRequest 会使上一份视图失效（内部缓冲被重写）；
 * 3. 异步提交（如把 key/value 交给别的任务）前必须把载荷复制到自己的存储。
 */
// 纯数据载体：字段公开供调用方直接读取，解析器内部状态放在私有尾部。
// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
struct Request final {
    RequestType type = RequestType::kSet;
    uint16_t id = 0;
    std::string_view key{};   // set
    std::string_view value{}; // set
    std::string_view name{};  // cmd
    std::string_view token{}; // unlock

    /** 构造空请求；不访问硬件/RTOS。 */
    constexpr Request() noexcept = default;
    /** 默认析构；不释放资源。 */
    ~Request() = default;
    // 视图可能指向 decoded_；复制会留下指向旧对象的悬空视图，因此禁止复制。
    Request(const Request &) = delete;
    Request &operator=(const Request &) = delete;

private:
    friend class Parser;
    // 含 JSON 转义的字符串解码到该缓冲；未转义的字段直接指向输入帧（无拷贝）。
    static constexpr size_t kDecodedCapacity = kMaxFrameBytes;
    std::array<char, kDecodedCapacity> decoded_{};
    size_t decodedSize_ = 0;
};
// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

/**
 * @brief  解析一条浏览器请求帧（单层 JSON 对象）。
 * @param  frame: 输入文本帧；长度超过 kMaxFrameBytes 直接返回 kTooLong。必须为合法 UTF-8。
 * @param[out] out: 解析结果；成功时字段有效，任何失败（含 kTooLong）都已清空。
 * @retval kNone       解析成功
 * @retval kTooLong    帧超过 1024 B
 * @retval kBadJson    语法错误、非对象、嵌套、浮点、重复键、字段类型不符、缺字段、未知 t、
 *                     成员名超出协议字符集（小写 ASCII/数字/`.`/`_`）、帧内非法 UTF-8
 * @retval kBadValue   id 不在 1..65535
 * @note   不阻塞、不分配堆；由网页模块任务调用，非 ISR 安全。字段视图的寿命约束见 Request 注释。
 */
[[nodiscard]] ErrorCode parseRequest(std::span<const char> frame, Request &out) noexcept;

} // namespace web
