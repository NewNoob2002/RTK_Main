// 网页 OTA 的镜像头、长度和流式写入顺序（ADR-0019 §2.2–2.4）。
// 纯逻辑不依赖 IDF；流操作由 httpd 任务或主机测试单线程注入。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace app::web_ota {

inline constexpr size_t kBufferBytes = 2048U; // B，启动期外供
inline constexpr unsigned char kImageMagic = 0xE9U;
inline constexpr uint32_t kAppDescMagic = 0xABCD5432U;
inline constexpr size_t kHeaderBytes = 288U;  // B，ESP image header + segment header + app desc
inline constexpr size_t kChipIdOffset = 12U;  // esp_image_header_t::chip_id
inline constexpr size_t kAppDescOffset = 32U; // B，首段 payload 起点
inline constexpr size_t kVersionOffset = kAppDescOffset + 16U;
inline constexpr size_t kProjectOffset = kAppDescOffset + 48U;
inline constexpr size_t kTextBytes = 32U; // B，app desc version / project_name
inline constexpr uint32_t kTimeoutMs = 30000U;
// 低于浏览器端 OTA_TOTAL_MS=180000（main/web/ui/app.js），保证 408 能在客户端超时前送达。
inline constexpr uint32_t kTotalTimeoutMs = 150000U;
inline constexpr uint32_t kRebootMs = 1500U;

enum class UploadCode : uint8_t {
    kOk,
    kBusy,
    kState,
    kLength,
    kTooLarge,
    kBadImage,
    kValidate,
    kTimeout,
    kFlash,
    kAborted,
};

struct ReceiveResult {
    size_t bytes = 0U;
    bool timedOut = false;
};

struct TransferResult {
    UploadCode code = UploadCode::kOk;
    size_t received = 0U;
    std::array<char, kTextBytes> version{};
};

/**
 * @brief 判定声明长度是否能容纳镜像头和镜像
 * @param length: 声明的 Content-Length（B）
 * @param capacity: 目标槽容量（B）
 * @retval kOk 有效
 * @retval kLength 缺失或为零
 * @retval kTooLarge 超过目标容量
 * @retval kBadImage 不能包含头后的镜像数据
 */
[[nodiscard]] constexpr UploadCode checkLength(uint64_t length, size_t capacity) noexcept
{
    if (length == 0U) {
        return UploadCode::kLength;
    }
    if (length > capacity) {
        return UploadCode::kTooLarge;
    }
    return length <= kHeaderBytes ? UploadCode::kBadImage : UploadCode::kOk;
}

/**
 * @brief 校验 ESP32 镜像头与 app desc，成功时提取版本
 * @param header: 已接收的镜像前缀
 * @param project: 运行镜像 project_name
 * @param[out] version: 成功时写入 32 字节版本字段，失败时不修改
 * @retval true 头部匹配
 * @retval false 头不足或字段不匹配
 */
[[nodiscard]] inline bool checkHeader(std::span<const char> header, std::string_view project,
                                      std::array<char, kTextBytes> &version) noexcept
{
    if (header.size() < kHeaderBytes || static_cast<unsigned char>(header.front()) != kImageMagic ||
        header[kChipIdOffset] != 0 || header[kChipIdOffset + 1U] != 0) {
        return false; // ESP_CHIP_ID_ESP32 的两个小端字节均为零。
    }
    constexpr size_t kBitsPerByte = 8U;
    constexpr uint32_t kByteMask = 0xFFU;
    for (size_t i = 0U; i < sizeof(kAppDescMagic); ++i) {
        const auto expected = (kAppDescMagic >> (i * kBitsPerByte)) & kByteMask;
        // 以无符号字节比较，char 的符号性不影响线格式。
        if (static_cast<unsigned char>(header[kAppDescOffset + i]) != expected) {
            return false;
        }
    }
    const std::string_view name{header.subspan(kProjectOffset).data(), kTextBytes};
    if (name.substr(0U, name.find('\0')) != project) {
        return false;
    }
    std::ranges::copy(header.subspan(kVersionOffset, kTextBytes), version.begin());
    return true;
}

/**
 * @brief 判断自起点以来的单调耗时是否达到上限
 * @param startMs: 起始单调时钟（ms）
 * @param nowMs: 当前单调时钟（ms）
 * @param limitMs: 时长上限（ms），小于 uint32_t 时钟周期
 * @retval true 已达到或超过上限
 * @retval false 尚未超时
 * @note 使用 uint32_t 无符号差值处理回绕。
 */
[[nodiscard]] constexpr bool elapsedAtLeast(uint32_t startMs, uint32_t nowMs,
                                            uint32_t limitMs) noexcept
{
    return nowMs - startMs >= limitMs;
}

/**
 * @brief 接收一段有界数据，连续无进展超时、总时长超限或中止时返回
 * @param io: 接收、时钟和停止请求操作
 * @param dst: 当次需要填满的缓冲视图
 * @param[in,out] received: 加上已实际收到的字节数
 * @param startMs: 本次上传准入时的单调毫秒起点
 * @retval kOk 此段接收完成
 * @retval kTimeout 连续无进展达到 kTimeoutMs 或总耗时达到 kTotalTimeoutMs
 * @retval kAborted 关机或接收端断开
 * @note 仅上传所有者调用，可等待 IO；dst 必须非空。
 */
template <typename Io>
[[nodiscard]] UploadCode receiveBlock(Io &io, std::span<char> dst, size_t &received,
                                      uint32_t startMs) noexcept
{
    uint32_t progressMs = io.nowMs();
    while (!dst.empty()) {
        if (io.stopRequested()) {
            return UploadCode::kAborted;
        }
        if (elapsedAtLeast(startMs, io.nowMs(), kTotalTimeoutMs)) {
            return UploadCode::kTimeout;
        }
        const auto rx = io.receive(dst);
        const uint32_t now = io.nowMs();
        if (rx.bytes != 0U && rx.bytes <= dst.size()) {
            received += rx.bytes;
            dst = dst.subspan(rx.bytes);
            progressMs = now;
            if (elapsedAtLeast(startMs, now, kTotalTimeoutMs)) {
                return io.stopRequested() ? UploadCode::kAborted : UploadCode::kTimeout;
            }
        } else if (!rx.timedOut) {
            return UploadCode::kAborted;
        } else if (elapsedAtLeast(progressMs, now, kTimeoutMs) ||
                   elapsedAtLeast(startMs, now, kTotalTimeoutMs)) {
            return io.stopRequested() ? UploadCode::kAborted : UploadCode::kTimeout;
        }
    }
    return io.stopRequested() ? UploadCode::kAborted : UploadCode::kOk;
}

/**
 * @brief 先校验头，再顺序写入，完成校验后提交启动槽
 * @param io: 接收、停止、时钟及 OTA 操作；调用方持有句柄并负责失败清理
 * @param buffer: 外供接收缓冲，至少容纳头部
 * @param length: 已验证的请求长度，须大于头部
 * @param project: 运行镜像 project_name
 * @param startMs: 本次上传准入时的单调毫秒起点
 * @retval 上传结果、接收总字节数与已解析版本
 * @note 仅上传所有者调用，可等待 IO/Flash；提交是唯一修改启动槽的步骤。
 */
template <typename Io>
[[nodiscard]] TransferResult transfer(Io &io, std::span<char> buffer, size_t length,
                                      std::string_view project, uint32_t startMs) noexcept
{
    TransferResult result{};
    auto block = buffer.first(kHeaderBytes);
    result.code = receiveBlock(io, block, result.received, startMs);
    if (result.code != UploadCode::kOk) {
        return result;
    }
    if (!checkHeader(block, project, result.version)) {
        result.code = UploadCode::kBadImage;
        return result;
    }
    result.code = io.begin();
    while (result.code == UploadCode::kOk) {
        if (io.stopRequested()) {
            result.code = UploadCode::kAborted;
            break;
        }
        if (elapsedAtLeast(startMs, io.nowMs(), kTotalTimeoutMs)) {
            result.code = UploadCode::kTimeout;
            break;
        }
        result.code = io.write(block);
        if (result.code != UploadCode::kOk || result.received == length) {
            break;
        }
        block = buffer.first(std::min(buffer.size(), length - result.received));
        result.code = receiveBlock(io, block, result.received, startMs);
    }
    if (result.code == UploadCode::kOk) {
        result.code = io.stopRequested() ? UploadCode::kAborted : io.end(length);
    }
    if (result.code == UploadCode::kOk) {
        result.code = io.stopRequested() ? UploadCode::kAborted : io.setBoot();
    }
    return result;
}

} // namespace app::web_ota
