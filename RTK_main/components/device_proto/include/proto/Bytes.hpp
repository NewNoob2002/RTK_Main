// 有界小端字节读写器（冻结，ADR-0002）：协议编码必须经由它们，不直接对缓冲区做下标拼装。
// 每个实例只由一个任务使用，非线程安全；无阻塞、无分配，ISR 安全。
// 不使用堆。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace proto {

/**
 * @brief  有界小端写指针；越界后置 !ok() 且不再写，实例单任务使用
 * @note   粘滞错误：任何一次写入越界后 ok() 永久为 false，已写入字节保持不变；
 *         调用方在一串写入后只需检查一次 ok()
 */
class ByteWriter {
public:
    /**
     * @brief  绑定输出缓冲，从偏移 0 开始写
     * @param  buf: 目标缓冲，容量即写入上限
     */
    explicit ByteWriter(std::span<uint8_t> buf) noexcept;

    /**
     * @brief  写一个字节
     * @param  value: 待写入的值
     * @note   空间不足时置 !ok() 且不写入
     */
    void u8(uint8_t value) noexcept;
    /**
     * @brief  以小端写 u16
     * @param  value: 待写入的值
     * @note   先整体检查 2 字节空间，不足时不部分写入并置 !ok()
     */
    void u16(uint16_t value) noexcept;
    /**
     * @brief  以小端写 u32
     * @param  value: 待写入的值
     * @note   先整体检查 4 字节空间，不足时不部分写入并置 !ok()
     */
    void u32(uint32_t value) noexcept;
    /**
     * @brief  原样写入一段字节
     * @param  data: 待写入数据（只读）
     * @note   空间不足时置 !ok() 且不写入
     */
    void bytes(std::span<const uint8_t> data) noexcept;
    /**
     * @brief  写入 count 个 0
     * @param  count: 字节数
     * @note   空间不足时置 !ok() 且不写入
     */
    void zeros(size_t count) noexcept;
    /**
     * @brief  写定宽字段：拷贝 min(text.size(), width) 字节，其余补 0
     * @param  text:  文本，不要求以 0 结尾
     * @param  width: 字段宽度（字节）
     * @note   按字节拷贝、不解释字符集；空间不足时置 !ok() 且不写入
     */
    void fixedStr(std::string_view text, size_t width) noexcept;

    /**
     * @brief  查询是否仍可写
     * @retval true  尚未发生越界
     * @retval false 已发生越界，后续写入被忽略
     */
    [[nodiscard]] bool ok() const noexcept;
    /**
     * @brief  已成功写入的字节数
     * @retval 当前写指针相对缓冲起点的偏移
     */
    [[nodiscard]] size_t size() const noexcept;
    /**
     * @brief  已写入部分的只读视图
     * @retval 指向缓冲起始、长度为 size() 的 span
     */
    [[nodiscard]] std::span<const uint8_t> written() const noexcept;

private:
    std::span<uint8_t> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

/**
 * @brief  有界小端读指针；越界后置 !ok() 并返回默认值，实例单任务使用
 * @note   粘滞错误：越界读返回 0 并使 ok() 为 false，之后所有读取返回默认值
 */
class ByteReader {
public:
    /**
     * @brief  绑定输入缓冲，从偏移 0 开始读
     * @param  buf: 源缓冲（只读）
     */
    explicit ByteReader(std::span<const uint8_t> buf) noexcept;

    /**
     * @brief  读一个字节
     * @retval 读到的字节；已越界时返回 0 并置 !ok()
     */
    [[nodiscard]] uint8_t u8() noexcept;
    /**
     * @brief  以小端读 u16
     * @retval 读到的值；越界部分按 0 计入并置 !ok()
     */
    [[nodiscard]] uint16_t u16() noexcept;
    /**
     * @brief  以小端读 u32
     * @retval 读到的值；越界部分按 0 计入并置 !ok()
     */
    [[nodiscard]] uint32_t u32() noexcept;
    /**
     * @brief  跳过 count 字节
     * @param  count: 跳过的字节数
     * @note   越过末尾时置 !ok() 并把读指针移到末尾
     */
    void skip(size_t count) noexcept;
    /**
     * @brief  取接下来 count 字节的视图
     * @param  count: 字节数
     * @retval 指向缓冲内部的 span；越界时返回空 span 并置 !ok()
     */
    [[nodiscard]] std::span<const uint8_t> take(size_t count) noexcept;

    /**
     * @brief  查询是否仍可读
     * @retval true  尚未越界
     * @retval false 已越界，后续读取返回默认值
     */
    [[nodiscard]] bool ok() const noexcept;
    /**
     * @brief  剩余可读字节数
     * @retval 缓冲末尾到当前读指针的距离
     */
    [[nodiscard]] size_t remaining() const noexcept;

private:
    std::span<const uint8_t> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace proto
