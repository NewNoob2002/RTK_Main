// 网页配置协议纯逻辑层（ws-protocol.md v1）：set 命令翻译、JSON 帧构造与状态差分。
// 纯 C++20 无 IDF/Wi-Fi/httpd 依赖，支持主机与 ASan/UBSan 测试；不阻塞、不分配堆，非 ISR。
// 堆档位：禁止区；全部输出经 web::JsonWriter 定长缓冲完成。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "AppConfig.hpp"
#include "core/Types.hpp"
#include "settings/Service.hpp"
#include "def/GnssSubDef.hpp"
#include "fw/Command.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#include "web_proto/Error.hpp"

namespace app::web_cfg {

/** `hello` 帧快照数据。 */
struct HelloSnapshot {
    std::string_view board{};
    std::string_view fw{};
    std::string_view btName{};
    std::string_view console{};
    bool factoryUnlocked = false;
};

/** `status.dev` 分组快照。 */
struct DevWebStatus {
    int32_t uptimeS = 0;
    int32_t heapFree = 0;
    int32_t heapMin = 0;
    std::string_view reset{};

    friend constexpr bool operator==(const DevWebStatus &, const DevWebStatus &) = default;
};

/** `status.gnss` 分组快照（可省略字段带 has* 标记）。 */
struct GnssWebStatus {
    static constexpr size_t kModelBufBytes = 20U;

    std::string_view state{};
    std::array<char, kModelBufBytes> model{};
    std::array<char, ::gnss::kVersionLen> fw{};
    std::string_view fix{};
    int32_t sats = 0;
    int32_t haccMm = 0;
    int32_t hz = 0;
    bool hasModel = false;
    bool hasFw = false;
    bool hasSats = false;
    bool hasHacc = false;
    bool hasHz = false;

    friend constexpr bool operator==(const GnssWebStatus &, const GnssWebStatus &) = default;
};

/** `status.bt` 分组快照。 */
struct BtWebStatus {
    size_t whitelistCount = 0U;
    std::array<core::BtAddr, kMaxAlarms> whitelist{};

    friend constexpr bool operator==(const BtWebStatus &, const BtWebStatus &) = default;
};

/** 完整 `status` 帧快照（三组可独立差分）。 */
struct StatusSnapshot {
    DevWebStatus dev{};
    GnssWebStatus gnss{};
    BtWebStatus bt{};
};

/**
 * @brief 将 GNSS 控制器状态映射为 ws-protocol.md §2 规定的状态名
 * @param state: 控制器内部状态枚举
 * @retval off/probing/configuring/running/failed/maintenance 之一
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *gnssStateName(::gnss::State state) noexcept;

/**
 * @brief 将 GNSS 定位解映射为 ws-protocol.md §2 规定的定位类型名
 * @param fix: 当前 GNSS 定位解快照
 * @retval none/single/dgps/float/fixed 之一
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *gnssFixName(const ::gnss::Fix &fix) noexcept;

/**
 * @brief 编码 `hello` 文本帧
 * @param snap: 设备静态信息与当前会话解锁状态
 * @param service: 设置服务
 * @param caps: 同一版本的 GNSS 能力快照
 * @param gnssEnabled: GNSS 是否可用
 * @param[out] out: 输出缓冲（建议 >= web::kMaxJsonBytes）
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 缓冲不足或编码失败
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildHelloFrame(const HelloSnapshot &snap, const settings::Service &service,
                                     const settings::GnssCaps &caps, bool gnssEnabled,
                                     std::span<char> out) noexcept;

/**
 * @brief 编码 `config` 文本帧（扁平配置键 + 嵌套 `gga_gate` 对象）
 * @param service: 设置服务
 * @param[out] out: 输出缓冲
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 缓冲不足或编码失败
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildConfigFrame(const settings::Service &service,
                                      std::span<char> out) noexcept;

/** @brief 编码更新后的能力帧。 @param service: 设置服务。 @param caps: 已发布快照。 @param gnssEnabled: GNSS 是否可用。 @param[out] out: 帧缓冲。 @retval 字节数，失败为 0。 */
[[nodiscard]] size_t buildCapsFrame(const settings::Service &service,
                                    const settings::GnssCaps &caps, bool gnssEnabled,
                                    std::span<char> out) noexcept;

/** @brief 映射设置服务错误码。 @param error: 服务状态。 @retval 网页协议错误码。 */
[[nodiscard]] web::ErrorCode fromSettingsError(settings::Error error) noexcept;

/**
 * @brief 编码 `status` 文本帧；prev 非空时仅输出发生变化的分组
 * @param cur: 当前状态快照
 * @param prev: 上次已发送给该客户端的快照；nullptr 表示首次全量发送
 * @param[out] out: 输出缓冲
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 三组均无变化，或输出缓冲不足
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildStatusFrame(const StatusSnapshot &cur, const StatusSnapshot *prev,
                                      std::span<char> out) noexcept;

/**
 * @brief 编码 `ack` 文本帧
 * @param id: 回显的请求 ID
 * @param error: kNone 输出 ok=true；其他输出 ok=false 与 err 字符串
 * @param[out] out: 输出缓冲
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 缓冲不足或编码失败
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildAckFrame(uint16_t id, web::ErrorCode error, std::span<char> out) noexcept;

/**
 * @brief 编码 `bye` 文本帧
 * @param reason: 退出原因（"exit" / "idle" / "freset"）
 * @param[out] out: 输出缓冲
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 缓冲不足或编码失败
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildByeFrame(std::string_view reason, std::span<char> out) noexcept;

} // namespace app::web_cfg
