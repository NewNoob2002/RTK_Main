// 设备 → 浏览器 WebSocket 帧的 JSON 输出器（docs/web/ws-protocol.md §2）。
// 先写入内部定长暂存，finish() 一次拷入调用方缓冲：缓冲不足时不输出半截 JSON。
// 由网页模块任务调用；不阻塞、非 ISR 安全、不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace web {

/** 输出器暂存上限；与单帧上限一致，超过即失败。 */
constexpr size_t kMaxJsonBytes = 1024;

/**
 * 逐字段构造 JSON 对象（根对象 + 最多两层嵌套），只输出完整帧。
 *
 * 使用约束：带可变状态，同一个对象只能由单个任务使用（典型为网页模块任务），
 * 不可在 ISR 中调用；不阻塞、不分配堆。任何写入失败都会置失败态，
 * 之后所有调用返回 false、finish() 返回 0，须 clear() 后才能复用。
 */
class JsonWriter final {
public:
    /** 构造空输出器；不访问硬件/RTOS。 */
    constexpr JsonWriter() noexcept = default;

    /**
     * @brief  清空输出器，复用内部缓冲。
     * @note   不分配堆，O(1)；单任务使用，非 ISR 安全。
     */
    void clear() noexcept;

    /**
     * @brief  开始根对象（`{`）。
     * @retval true  成功
     * @retval false 已关闭/已失败/重复调用
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool beginObject() noexcept;

    /**
     * @brief  在当前对象内开始子对象（`"key":{`）。
     * @param  key: 成员名。
     * @retval true  成功
     * @retval false 未在对象内、达到深度上限、缓冲不足或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool beginNested(std::string_view key) noexcept;

    /**
     * @brief  结束最近的一层嵌套对象（`}`）。
     * @retval true  成功
     * @retval false 不在嵌套对象内或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool endNested() noexcept;

    /**
     * @brief  结束根对象（`}`）。
     * @retval true  成功
     * @retval false 仍有未关闭的嵌套、非对象状态或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool endObject() noexcept;

    /**
     * @brief  写入 int32 成员。
     * @param  key:   成员名。
     * @param  value: 数值。
     * @retval true  成功
     * @retval false 未在对象内、缓冲不足或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool writeInt(std::string_view key, int32_t value) noexcept;

    /**
     * @brief  写入 bool 成员（`true`/`false`）。
     * @param  key:   成员名。
     * @param  value: 布尔值。
     * @retval true  成功
     * @retval false 未在对象内、缓冲不足或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool writeBool(std::string_view key, bool value) noexcept;

    /**
     * @brief  写入字符串成员，自动转义 `"`、`\` 与控制字符。
     * @param  key:   成员名。
     * @param  value: 字符串内容（UTF-8 原样透传）。
     * @retval true  成功
     * @retval false 未在对象内、缓冲不足或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool writeString(std::string_view key, std::string_view value) noexcept;

    /**
     * @brief  写入字符串数组成员（用于 `bt.whitelist`）。
     * @param  key:    成员名。
     * @param  values: 字符串数组；空数组输出 `[]`。
     * @retval true  成功
     * @retval false 未在对象内、缓冲不足或已失败
     * @note   单任务使用，非 ISR 安全。
     */
    [[nodiscard]] bool writeStringArray(std::string_view key,
                                        std::span<const std::string_view> values) noexcept;

    /**
     * @brief  完成并一次拷入调用方缓冲；任何错误状态都不写调用方缓冲。
     * @param[out] out: 输出缓冲；容量不足时保持原内容不变（不会写半截帧）。
     * @retval >0   完整帧字节数，已写入 out
     * @retval 0    未关闭根对象、发生过错误或 out 容量不足
     * @note   成功后输出器保持关闭状态；复用前调用 clear()。单任务使用，非 ISR 安全。
     */
    [[nodiscard]] size_t finish(std::span<char> out) noexcept;

    /** @brief 写入整数数组。 @param key: 字段名。 @param values: 元素。 @retval 是否成功。 @note 单任务，非 ISR。 */
    [[nodiscard]] bool writeIntArray(std::string_view key,
                                     std::span<const int32_t> values) noexcept;

private:
    static constexpr size_t kMaxDepth = 3; // 根对象、caps 对象与闭区间

    /**
     * @brief  追加一个字节到暂存。
     * @param  c: 待追加字符。
     * @retval true  成功
     * @retval false 暂存已满或已失败（置失败态）
     */
    [[nodiscard]] bool append(char c) noexcept;

    /**
     * @brief  追加一段原文到暂存。
     * @param  text: 待追加内容。
     * @retval true  成功
     * @retval false 暂存不足或已失败
     */
    [[nodiscard]] bool appendRaw(std::string_view text) noexcept;

    /**
     * @brief  追加带引号的转义字符串。
     * @param  text: 字符串内容。
     * @retval true  成功
     * @retval false 暂存不足或已失败
     */
    [[nodiscard]] bool appendEscaped(std::string_view text) noexcept;

    /**
     * @brief  追加一个已转义的字符串字符。
     * @param  c: 原字符。
     * @retval true  成功
     * @retval false 暂存不足或已失败
     */
    [[nodiscard]] bool appendEscapedChar(char c) noexcept;

    /**
     * @brief  以 `\u00XX` 形式追加控制字符。
     * @param  byte: 0..31 的控制字符。
     * @retval true  成功
     * @retval false 暂存不足或已失败
     */
    [[nodiscard]] bool appendControlEscape(unsigned char byte) noexcept;

    /**
     * @brief  追加 int32 十进制文本。
     * @param  value: 数值。
     * @retval true  成功
     * @retval false 暂存不足或已失败
     */
    [[nodiscard]] bool appendInt(int32_t value) noexcept;

    /**
     * @brief  写入成员前缀（逗号、转义键名、冒号）。
     * @param  key: 成员名。
     * @retval true  成功
     * @retval false 未在对象内、暂存不足或已失败
     */
    [[nodiscard]] bool memberPrefix(std::string_view key) noexcept;

    /**
     * @brief  置失败态并返回 false。
     * @retval false 恒为 false
     */
    [[nodiscard]] bool fail() noexcept;

    std::array<char, kMaxJsonBytes> buffer_{};
    size_t size_ = 0;
    std::array<uint16_t, kMaxDepth + 1> memberCount_{};
    size_t depth_ = 0;
    bool closed_ = false;
    bool failed_ = false;
};

} // namespace web
