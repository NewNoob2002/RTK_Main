// 通用持久化键的 NVS 读写（ADR-0010 §2.10）：按 fw::StoreKey 的类型读写一个整数值。
// 线程：NVS API 自带锁，可在任何任务调用（非 ISR）；StorageModule::loadKeys/saveKeys 与 config 命令共用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "fw/StoreKey.hpp"
#include "modules/storage/StoreCatalog.hpp"

namespace app::store {

inline constexpr fw::StoreKey kPhoneHeightKey{
    .ns = "rtk",
    .key = "ant_height",
    .type = fw::StoreType::kBlob,
    .maxSize = 8U,
    .help = "Antenna height (binary64 LE, m)",
};
inline constexpr fw::StoreKey kPhonePppKey{
    .ns = "rtk",
    .key = "ppp_enable",
    .type = fw::StoreType::kU8,
    .maxSize = 1U,
    .min = 0,
    .max = 1,
    .help = "Unicore PPP setting (0/1)",
};
inline constexpr fw::StoreKey kPhoneSatCfgKey{
    .ns = "rtk",
    .key = "sat_cfg",
    .type = fw::StoreType::kBlob,
    .maxSize = 6U,
    .help = "Cutoff angle and GNSS constellation mask",
};
inline constexpr fw::StoreKey kPhonePnKey{
    .ns = "rtk",
    .key = "pn",
    .type = fw::StoreType::kBlob,
    .maxSize = 16U,
    .help = "Device part number (0x08 Nav)",
};
inline constexpr fw::StoreKey kPhoneNtripKey{
    .ns = "rtk",
    .key = "ntrip_cfg",
    .type = fw::StoreType::kBlob,
    .maxSize = 112U,
    .help = "NTRIP config blob (0x12)",
};
inline constexpr fw::StoreKey kPhoneJsonSrvKey{
    .ns = "rtk",
    .key = "json_srv",
    .type = fw::StoreType::kBlob,
    .maxSize = 42U,
    .help = "JSON server config blob (0x32 Nav)",
};
inline constexpr fw::StoreKey kPhoneTbKey{
    .ns = "rtk",
    .key = "tb_cfg",
    .type = fw::StoreType::kBlob,
    .maxSize = 86U,
    .help = "Thingsboard config blob (0x33 Nav)",
};
static_assert(fw::valid(kPhoneHeightKey) && fw::valid(kPhonePppKey) && fw::valid(kPhoneSatCfgKey) &&
              fw::valid(kPhonePnKey) && fw::valid(kPhoneNtripKey) && fw::valid(kPhoneJsonSrvKey) &&
              fw::valid(kPhoneTbKey));

inline constexpr size_t kMaxBlobKeyBytes = 128U;
inline constexpr size_t kKeyLineBytes = 132U;

/**
 * @brief 将一个持久化键读出并格式化为 config 和启动日志共用的一行。
 * @param[out] out: 末尾带 NUL 的行缓冲。
 * @param key: 键定义。
 * @param valueName: 可空的数值显示名钩子。
 * @param defaultsOnly: 显示启动缺省，无需读取 NVS。
 * @note 任务上下文，可等待 NVS，非 ISR。
 */
void formatKey(std::span<char, kKeyLineBytes> out, const fw::StoreKey &key,
               StoreValueName valueName, bool defaultsOnly = false) noexcept;

/**
 * @brief 键的值宽度（字节）：u8/u16/u32 → 1/2/4；本轮不支持的类型返回 0。
 * @param type: 持久化值类型
 * @retval 1/2/4 对应 u8/u16/u32
 * @retval 0 其他类型
 * @note 纯函数，线程/ISR 安全，不阻塞。
 */
[[nodiscard]] size_t valueWidth(fw::StoreType type) noexcept;

/**
 * @brief 读一个整数键。命名空间或键不存在 → ESP_ERR_NVS_NOT_FOUND；类型不支持/名字非法 → ESP_ERR_INVALID_ARG。
 * @param key: 持久化键定义
 * @param value: 值输出；打开前失败保持原值，u8/u16 读取失败可能置 0
 * @retval ESP_OK 已读取
 * @retval ESP_ERR_INVALID_ARG 键类型或名称非法
 * @retval ESP_ERR_NVS_NOT_FOUND 命名空间或键不存在
 * @retval 其他 NVS 读取错误
 * @note 任意任务，NVS 内部同步，可阻塞，非 ISR。
 */
[[nodiscard]] esp_err_t readValue(const fw::StoreKey &key, uint32_t &value) noexcept;

/**
 * @brief 写一个整数键并提交；value 超出该键宽度的高位被忽略。错误同 readValue，另含后端写错误。
 * @param key: 持久化键定义
 * @param value: 待转换或写入的值
 * @retval ESP_OK 已写入并提交
 * @retval ESP_ERR_INVALID_ARG 键类型或名称非法
 * @retval 其他 NVS 打开、写入或提交错误
 * @note 任意任务，NVS 内部同步，可阻塞，非 ISR。
 */
[[nodiscard]] esp_err_t writeValue(const fw::StoreKey &key, uint32_t value) noexcept;

/**
 * @brief 读一个 Blob 键到 out（len 返回实际读取字节数）。
 * @param key: 持久化键定义
 * @param[out] out: Blob 输出，后端读取失败时内容不作保证
 * @param[out] len: 入口清零，仅成功时更新为实际长度
 * @retval ESP_OK 已读取，len 为实际长度
 * @retval ESP_ERR_INVALID_ARG 键非法或输出不足 maxSize
 * @retval ESP_ERR_NVS_NOT_FOUND 命名空间或键不存在
 * @retval 其他 NVS 读取错误
 * @note 任意任务，NVS 内部同步，可阻塞，非 ISR。
 */
[[nodiscard]] esp_err_t readBlob(const fw::StoreKey &key, std::span<uint8_t> out,
                                 size_t &len) noexcept;

/**
 * @brief 写一个 Blob 键并提交。
 * @param key: 持久化键定义
 * @param data: 待存储 Blob 字节
 * @retval ESP_OK 已写入并提交
 * @retval ESP_ERR_INVALID_ARG 键非法、数据为空或超 maxSize
 * @retval 其他 NVS 打开、写入或提交错误
 * @note 任意任务，NVS 内部同步，可阻塞，非 ISR。
 */
[[nodiscard]] esp_err_t writeBlob(const fw::StoreKey &key, std::span<const uint8_t> data) noexcept;

} // namespace app::store
