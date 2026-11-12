// GNSS 命令方言描述符：一个家族 = 一个无可变状态的 constexpr 函数指针表（同 demux::Decoder 的做法，不用 virtual）。
// 方言只负责“字节怎么写、应答怎么认”，不做 IO、不计时；时序与重试由 gnss::Controller 统一处理。
// 所有函数 noexcept、可重入、不访问全局可变状态。线程安全：是（纯函数）。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoder.hpp"
#include "gnss/types.hpp"

namespace gnss {

inline constexpr size_t kMaxCommand = 160; // 单条命令最大字节（UBX CFG-VALSET 多键约 100 B）

/** 定长命令缓冲；append 失败时不改变内容并返回 false。 */
struct CmdBuf {
    std::array<uint8_t, kMaxCommand> data{};
    size_t len = 0;

    void clear() noexcept
    {
        len = 0;
    }
    [[nodiscard]] bool append(std::span<const uint8_t> bytes) noexcept
    {
        if (bytes.size() > (data.size() - len)) {
            return false;
        }
        for (const uint8_t b : bytes) {
            data[len] = b;
            ++len;
        }
        return true;
    }
    [[nodiscard]] bool append(std::string_view text) noexcept
    {
        if (text.size() > (data.size() - len)) {
            return false;
        }
        for (const char c : text) {
            data[len] = static_cast<uint8_t>(c);
            ++len;
        }
        return true;
    }
    [[nodiscard]] std::span<const uint8_t> view() const noexcept
    {
        return {data.data(), len};
    }
};

inline constexpr size_t kVersionLen = 40;

/** 探测结果：型号与固件版本文本（NUL 结尾，截断）。 */
struct ProbeResult {
    Model model = Model::kUnknown;
    std::array<char, kVersionLen> fw_version{};
};

/** 流动站配置参数（ADR-0010 §2.4）。 */
struct RoverConfig {
    uint8_t rate_hz = 1;    // 定位输出频率：1/2/5/10/20（方言把不支持的值向下取到支持值）
    bool gsv = true;        // 是否输出 GSV（卫星表），GSV 与 GGA 同频率或 1 Hz 取较小者
    uint8_t port = 2;       // 模块上连接 ESP32 的串口编号（UM980 COM2；F9P UART1=1）
    uint32_t port_baud = 0; // 0 = 不改波特率；否则配置末尾切换到此波特率
};

/** 配置脚本中的一步。 */
struct ConfigStep {
    CmdBuf cmd{};
    bool expect_reply = true;  // false：发送后不等待应答，直接进入下一步
    uint16_t timeout_ms = 500; // 等待应答时限
    uint32_t switch_baud = 0;  // 非 0：本步确认（或无需确认时发送）后，主机 UART 改到此波特率
};

/** 应答判定。 */
enum class Reply : uint8_t {
    kNone, // 与本步无关的帧
    kAck,  // 本步成功
    kNack, // 本步被模块拒绝
};

/**
 * 方言描述符。以 `inline constexpr` / `extern const` 变量定义在 gnss/dialects/<family>.hpp，地址即身份。
 * decoders：本家族应答需要的额外解码器（模块把它们与 NMEA、RTCM3 一起挂到 UART 分流器上，按地址去重）。
 */
struct Dialect {
    const char *name; // 诊断用，非空
    Family family;
    std::span<const demux::Decoder *const> decoders;

    /** 生成版本查询命令。 */
    bool (*build_probe)(CmdBuf &out) noexcept;
    /** frame 是否为版本查询的应答；是则填 out 并返回 true。dec 为分流器识别出的解码器。 */
    bool (*match_probe)(const demux::Decoder &dec, std::span<const uint8_t> frame, ProbeResult &out) noexcept;
    /**
     * 流动站配置脚本第 index 步（从 0 开始）。index 超出脚本长度返回 false（脚本结束）。
     * 必须是纯函数：同样参数生成同样字节，便于重试与主机测试。
     */
    bool (*rover_step)(size_t index, Model model, const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 判定 frame 是否为 sent（本步已发送的命令）的应答。 */
    Reply (*match_reply)(const demux::Decoder &dec, std::span<const uint8_t> frame,
                         std::span<const uint8_t> sent) noexcept;
    /** 型号能力位（gnss::cap::*）。 */
    uint32_t (*caps)(Model model) noexcept;
};

/** 型号的显示名（"UM980"、"ZED-F9P"……）；未知返回 "unknown"。 */
[[nodiscard]] const char *model_name(Model model) noexcept;
/** 型号所属家族。 */
[[nodiscard]] Family family_of(Model model) noexcept;

/**
 * 本次构建启用的方言（IDF 下按 CONFIG_RTK_GNSS_DIALECT_*，主机 CMake 下按同名 option），按探测优先级排序。
 * 返回的表与描述符生命周期为整个程序。
 */
[[nodiscard]] std::span<const Dialect *const> enabled_dialects() noexcept;
/** 按家族查找启用的方言；未启用返回 nullptr。 */
[[nodiscard]] const Dialect *dialect_for(Family family) noexcept;

} // namespace gnss
