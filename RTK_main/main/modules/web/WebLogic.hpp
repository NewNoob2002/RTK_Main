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
#include "data/GnssOutput.hpp"
#include "def/GnssSubDef.hpp"
#include "fw/Command.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#include "web_proto/Error.hpp"

namespace app::web_cfg {

/** `set` 命令翻译后的执行目标分类。 */
enum class CommandTarget : uint8_t {
    kNone = 0,
    kStorage,
    kGnss,
};

/** `set` 请求翻译结果（校验、权限与目标 fw::Command）。 */
struct SetPlan {
    web::ErrorCode error = web::ErrorCode::kNone;
    CommandTarget target = CommandTarget::kNone;
    bool waitGnssApply = false;  // gnss.rate / gnss.out.* 须等待两阶段提交 applySeq
    bool notifyBtStatus = false; // bt.wl.add / bt.wl.del 成功后立即推送 status.bt
    fw::Command command{};
};

/** `hello` 帧快照数据。 */
struct HelloSnapshot {
    std::string_view board{};
    std::string_view fw{};
    std::string_view btName{};
    std::string_view console{};
    bool factoryUnlocked = false;
};

/** `config` 帧快照数据。 */
struct ConfigSnapshot {
    uint8_t gnssRateHz = 0U;
    data::GnssOutConfig gnssOut{};
    bool ggaGateEnabled = false;
    uint8_t ggaGateMinSats = 0U;
    bool btPairNext = false;
    std::string_view sn{};
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
[[nodiscard]] const char *webGnssStateName(::gnss::State state) noexcept;

/**
 * @brief 将 GNSS 定位解映射为 ws-protocol.md §2 规定的定位类型名
 * @param fix: 当前 GNSS 定位解快照
 * @retval none/single/dgps/float/fixed 之一
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *webGnssFixName(const ::gnss::Fix &fix) noexcept;

/**
 * @brief 计算指定 App 输出语句在当前配置与定位频率下的生效周期（ms）
 * @param outCfg: 持久化 GNSS 输出配置
 * @param rateHz: 当前定位频率（Hz，0 按 1 Hz 处理）
 * @param typeIdx: App 输出语句下标（0..5，对应 gnss::OutputMsg 前 6 项）
 * @retval 0 表示关闭；正数为毫秒周期
 * @note 纯函数，线程/ISR 安全；与 makeOutputPlan 保持同一规则。
 */
[[nodiscard]] int32_t effectiveOutPeriodMs(const data::GnssOutConfig &outCfg, uint8_t rateHz,
                                           size_t typeIdx) noexcept;

/**
 * @brief 校验 `set` 键值对并翻译为发往模块命令总线的 fw::Command
 * @param key: 协议键名（如 "gnss.rate"、"gnss.gga_gate.min_sats"）
 * @param value: 浏览器传入的字符串值
 * @param unlocked: 当前 WebSocket 会话是否已工厂解锁
 * @param curOutCfg: 当前 GNSS 输出配置（用于修改单个 gnss.out.* 项时保留其余语句）
 * @retval SetPlan error==kNone 表示已生成可执行命令；否则为协议 §5 错误码
 * @note 纯函数，复用 web_proto 查表与校验，不阻塞、不分配堆，非 ISR。
 */
[[nodiscard]] SetPlan translateSetRequest(std::string_view key, std::string_view value,
                                          bool unlocked,
                                          const data::GnssOutConfig &curOutCfg) noexcept;

/**
 * @brief 编码 `hello` 文本帧
 * @param snap: 设备静态信息与当前会话解锁状态
 * @param[out] out: 输出缓冲（建议 >= web::kMaxJsonBytes）
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 缓冲不足或编码失败
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildHelloFrame(const HelloSnapshot &snap, std::span<char> out) noexcept;

/**
 * @brief 编码 `config` 文本帧（扁平配置键 + 嵌套 `gga_gate` 对象）
 * @param snap: 当前全部可改配置快照
 * @param[out] out: 输出缓冲
 * @retval >0 编码后的 JSON 字节数
 * @retval 0 缓冲不足或编码失败
 * @note 不阻塞、不分配堆，单任务调用，非 ISR。
 */
[[nodiscard]] size_t buildConfigFrame(const ConfigSnapshot &snap, std::span<char> out) noexcept;

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
