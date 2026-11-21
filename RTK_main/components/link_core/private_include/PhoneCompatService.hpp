// 手机兼容命令（Nav/Survey 双模式）与延迟应答的私有装配接口。
// 仅链路任务访问，启动期绑定回调，非 ISR，不阻塞，不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/CommandRouter.hpp"

namespace core {

inline constexpr uint16_t kPhoneSatTrackId = 0x06U;
inline constexpr uint16_t kPhonePppId = 0x33U;
inline constexpr uint8_t kSatDefaultCutoffDeg = 15U;
inline constexpr uint8_t kSatMaxCutoffDeg = 90U;
inline constexpr uint8_t kSatCapGps = 1U << 0U;
inline constexpr uint8_t kSatCapBds = 1U << 1U;
inline constexpr uint8_t kSatCapGlo = 1U << 2U;
inline constexpr uint8_t kSatCapGal = 1U << 3U;
inline constexpr uint8_t kSatCapSbas = 1U << 4U;
inline constexpr uint8_t kSatCapAll =
    static_cast<uint8_t>(static_cast<unsigned>(kSatCapGps) | static_cast<unsigned>(kSatCapBds) |
                         static_cast<unsigned>(kSatCapGlo) | static_cast<unsigned>(kSatCapGal) |
                         static_cast<unsigned>(kSatCapSbas));
inline constexpr size_t kAntennaHeightBytes = 8U;
inline constexpr size_t kSatConfigBlobBytes = 6U;
inline constexpr size_t kPhoneSnBytes = 16U;
inline constexpr size_t kPhonePnBytes = 16U;
inline constexpr size_t kPhoneSnPnBytes = kPhoneSnBytes + kPhonePnBytes;
inline constexpr size_t kPhoneNtripBytes = 112U;
inline constexpr size_t kPhoneJsonServerBytes = 42U;
inline constexpr size_t kPhoneThingsboardBytes = 86U;

using AntennaHeight = std::array<uint8_t, kAntennaHeightBytes>;
using SatConfigBlob = std::array<uint8_t, kSatConfigBlobBytes>;
using PhonePn = std::array<uint8_t, kPhonePnBytes>;
using PhoneSnPn = std::array<uint8_t, kPhoneSnPnBytes>;
using PhoneNtripConfig = std::array<uint8_t, kPhoneNtripBytes>;
using PhoneJsonServerConfig = std::array<uint8_t, kPhoneJsonServerBytes>;
using PhoneThingsboardConfig = std::array<uint8_t, kPhoneThingsboardBytes>;

/** Survey 0x06 截止角与星座开关配置（NVS 序列化固定 6 B；默认零初始化以保持 AppContext 在 .bss）。 */
struct SatTrackConfig {
    uint8_t cutoffDeg = 0U;
    uint8_t gps = 0U;
    uint8_t glo = 0U;
    uint8_t bds = 0U;
    uint8_t gal = 0U;
    uint8_t sbas = 0U;

    /** @brief 返回与旧固件一致的默认截止角与星座配置（15°、四系统开、SBAS 关）。 @retval 默认配置。 */
    [[nodiscard]] static constexpr SatTrackConfig defaults() noexcept
    {
        return {
            .cutoffDeg = kSatDefaultCutoffDeg,
            .gps = 1U,
            .glo = 1U,
            .bds = 1U,
            .gal = 1U,
            .sbas = 0U,
        };
    }

    friend constexpr bool operator==(const SatTrackConfig &, const SatTrackConfig &) = default;
};

/** @brief 校验小端 IEEE 754 天线高。 @param bytes: 线协议字节。 @retval true 有限且在 0～10 m 内。 */
[[nodiscard]] bool validAntennaHeight(std::span<const uint8_t, kAntennaHeightBytes> bytes) noexcept;

/** @brief 校验截止角（0..90°）与星座开关（0/1）。 @param cfg: 卫星配置。 @retval true 全部字段合法。 */
[[nodiscard]] constexpr bool validSatTrackConfig(const SatTrackConfig &cfg) noexcept
{
    return (cfg.cutoffDeg <= kSatMaxCutoffDeg) && (cfg.gps <= 1U) && (cfg.glo <= 1U) &&
           (cfg.bds <= 1U) && (cfg.gal <= 1U) && (cfg.sbas <= 1U);
}

/** @brief 将 SatTrackConfig 编码为 6 B 字节数组。 @param cfg: 配置值。 @retval 6 字节定长编码。 */
[[nodiscard]] constexpr SatConfigBlob encodeSatTrackConfig(const SatTrackConfig &cfg) noexcept
{
    return {cfg.cutoffDeg, cfg.gps, cfg.glo, cfg.bds, cfg.gal, cfg.sbas};
}

/** @brief 从 6 B 字节流解码并校验 SatTrackConfig。 @param bytes: 原始字节。 @retval 有效配置，否则空。 */
[[nodiscard]] constexpr auto decodeSatTrackConfig(std::span<const uint8_t> bytes) noexcept
    -> std::optional<SatTrackConfig>
{
    if (bytes.size() != kSatConfigBlobBytes) {
        return std::nullopt;
    }
    const SatTrackConfig cfg{
        .cutoffDeg = bytes[0U],
        .gps = bytes[1U],
        .glo = bytes[2U],
        .bds = bytes[3U],
        .gal = bytes[4U],
        .sbas = bytes[5U],
    };
    if (!validSatTrackConfig(cfg)) {
        return std::nullopt;
    }
    return cfg;
}

namespace services {

struct PhoneCompatOps {
    void *ctx = nullptr;
    AntennaHeight (*readHeight)(const void *) noexcept = nullptr;
    bool (*startHeight)(void *, const AntennaHeight &) noexcept = nullptr;
    std::optional<bool> (*heightResult)(const void *) noexcept = nullptr;
    void (*cancelHeight)(void *) noexcept = nullptr;
    bool (*pppSupported)(const void *) noexcept = nullptr;
    bool (*readPpp)(const void *) noexcept = nullptr;
    bool (*startPpp)(void *, bool) noexcept = nullptr;
    std::optional<bool> (*pppResult)(const void *) noexcept = nullptr;
    void (*cancelPpp)(void *) noexcept = nullptr;
    uint8_t (*satCapsMask)(const void *) noexcept = nullptr;
    SatTrackConfig (*readSatConfig)(const void *) noexcept = nullptr;
    bool (*startSatConfig)(void *, const SatTrackConfig &) noexcept = nullptr;
    std::optional<bool> (*satConfigResult)(const void *) noexcept = nullptr;
    void (*cancelSatConfig)(void *) noexcept = nullptr;
    PhoneSnPn (*readSnPn)(const void *) noexcept = nullptr;
    bool (*startSnPn)(void *, const PhoneSnPn &) noexcept = nullptr;
    std::optional<bool> (*snPnResult)(const void *) noexcept = nullptr;
    void (*cancelSnPn)(void *) noexcept = nullptr;
    size_t (*readLatestGsv)(const void *, std::span<uint8_t>) noexcept = nullptr;
    PhoneNtripConfig (*readNtrip)(const void *) noexcept = nullptr;
    bool (*setNtrip)(void *, const PhoneNtripConfig &, uint32_t) noexcept = nullptr;
    PhoneJsonServerConfig (*readJsonServer)(const void *) noexcept = nullptr;
    bool (*setJsonServer)(void *, const PhoneJsonServerConfig &, uint32_t) noexcept = nullptr;
    PhoneThingsboardConfig (*readThingsboard)(const void *) noexcept = nullptr;
    bool (*setThingsboard)(void *, const PhoneThingsboardConfig &, uint32_t) noexcept = nullptr;
    bool (*scheduleReset)(void *, bool, bool, uint32_t) noexcept = nullptr;
    bool (*wifiConfigSupported)(const void *) noexcept = nullptr;
    bool (*scheduleWifiConfig)(void *, uint32_t) noexcept = nullptr;
};

struct PhoneCompatService {
    PhoneCompatOps ops{};
    bool pppWaiting = false;
    bool satWaiting = false;
    bool heightWaiting = false;
    bool snPnWaiting = false;

    /** @brief 返回只允许 PHONE 访问的路由表。 @retval 路由表与本实例。 */
    [[nodiscard]] RouteTable table() noexcept;

    /**
     * @brief Survey App 的 0x06 截止角/星座处理入口（绑定到 DeviceService::setSatTrackSurveyHook）。
     * @param self: 指向 PhoneCompatService 实例。
     * @param frame: 已校验的请求帧。
     * @param nowMs: 当前单调时钟（ms）。
     * @param[out] reply: 应答缓冲或延迟应答钩子。
     * @retval 路由分发结果（kReply / kDeferred / kDenied）。
     */
    [[nodiscard]] static DispatchResult handleSurveySatTrack(void *self,
                                                             const proto::FrameView &frame,
                                                             uint32_t nowMs, Reply &reply) noexcept;
};

} // namespace services
} // namespace core
