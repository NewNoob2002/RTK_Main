// net.* 设置的 NVS 持久化（ADR-0021 §2.7）：写入只由 StorageModule（kLink 任务）执行，
// 读取只在启动期单线程（BootConfig / NetModule::init）。kNet 运行期任务不打开 NVS。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cellular/LinkFsm.hpp"
#include "esp_err.h"

namespace app::net_store {

using Field = std::array<char, cellular::LinkConfig::kFieldBytes>;

/** 已持久化的 net.* 配置（密码仅在启动期交给 LinkFsm，不进入 DataCenter）。 */
struct Saved {
    bool enable = false;
    Field apn{};
    Field user{};
    Field pass{};
};

/** 文本键种类。 */
enum class TextKey : uint8_t { kApn, kUser, kPass };

/**
 * @brief 检查文本是否可作为 4G 文本键（0..31 个可见 ASCII，禁止双引号与反斜杠）
 * @param text: 候选文本
 * @retval true 合法
 * @note 纯函数，线程/ISR 安全；双引号/反斜杠会破坏 AT+QICSGP 命令行。
 */
[[nodiscard]] bool validText(std::string_view text) noexcept;

/**
 * @brief 读取全部 net.* 配置；缺失或格式非法的键保持默认
 * @param[out] out: 配置
 * @note 仅启动期单线程；可阻塞于 NVS，非 ISR。
 */
void load(Saved &out) noexcept;

/**
 * @brief 只读取 net.enable（启动早期，用于决定是否分配 4G 资源）
 * @retval true NVS 中存在且为 1；缺失/非法/读失败为 false
 * @note 仅启动期单线程；可阻塞于 NVS，非 ISR。
 */
[[nodiscard]] bool loadEnable() noexcept;

/**
 * @brief 写入 net.enable
 * @param enable: 使能
 * @retval ESP_OK 已提交到 NVS
 * @note 仅 StorageModule（kLink 任务）；可阻塞于 NVS，非 ISR。
 */
[[nodiscard]] esp_err_t saveEnable(bool enable) noexcept;

/**
 * @brief 写入文本键
 * @param key: 键种类
 * @param text: 文本（调用方已用 validText 校验）
 * @retval ESP_OK 已提交到 NVS
 * @note 仅 StorageModule（kLink 任务）；可阻塞于 NVS，非 ISR。
 */
[[nodiscard]] esp_err_t saveText(TextKey key, std::string_view text) noexcept;

} // namespace app::net_store
