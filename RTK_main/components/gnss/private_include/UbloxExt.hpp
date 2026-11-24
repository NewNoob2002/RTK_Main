// u-blox F9P 共享 UBX/VALSET 编码器、固件能力判定与在途 VALSET 跟踪（组件私有头）。
// 线程安全：工具函数纯函数可重入，ValsetTracker 由单任务独占。ISR 安全：纯函数是，跟踪器否；均零堆不阻塞。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "PppSequence.hpp"
#include "gnss/Receiver.hpp"

namespace gnss::ublox {

inline constexpr size_t kMaxExtractedValsetKeys = 16U;
inline constexpr size_t kSecUniqIdHexChars = 10U;
inline constexpr uint32_t kValsetResyncWindowMs = 300U;

/** 唯一共享的 CFG-VALSET 载荷写入器：定长缓冲 + 写点下标（越界即失败，零堆分配）。 */
struct ValsetWriter final {
    std::span<uint8_t> buf{};
    size_t pos = 0U;

    /**
     * @brief 追加 1 字节。
     * @param value: 待写值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool u8(uint8_t value) noexcept;

    /**
     * @brief 追加小端 2 字节。
     * @param value: 待写值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool u16(uint16_t value) noexcept;

    /**
     * @brief 追加小端 4 字节。
     * @param value: 待写值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool u32(uint32_t value) noexcept;

    /**
     * @brief 写入 4 字节 CFG-VALSET v0 头（version=0, layers, reserved[2]=0）。
     * @param layers: 配置层位图（RAM / RAM|BBR）。
     * @retval true 已写入 4 字节头；false 缓冲不足或非空缓冲起点。
     */
    [[nodiscard]] bool begin(uint8_t layers) noexcept;

    /**
     * @brief 追加 1 字节值（L/U1）键值项。
     * @param key: 32 位 UBX 配置键。
     * @param value: 无符号 1 字节值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool addU1(uint32_t key, uint8_t value) noexcept;

    /**
     * @brief 追加小端 2 字节值（U2）键值项。
     * @param key: 32 位 UBX 配置键。
     * @param value: 无符号 2 字节值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool addU2(uint32_t key, uint16_t value) noexcept;

    /**
     * @brief 追加小端 4 字节值（U4）键值项。
     * @param key: 32 位 UBX 配置键。
     * @param value: 无符号 4 字节值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool addU4(uint32_t key, uint32_t value) noexcept;

    /**
     * @brief 追加有符号 1 字节值（I1）键值项。
     * @param key: 32 位 UBX 配置键。
     * @param value: 有符号 1 字节值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool addI1(uint32_t key, int8_t value) noexcept;

    /**
     * @brief 追加小端有符号 4 字节值（I4）键值项。
     * @param key: 32 位 UBX 配置键。
     * @param value: 有符号 4 字节值。
     * @retval true 写入成功；false 空间不足。
     */
    [[nodiscard]] bool addI4(uint32_t key, int32_t value) noexcept;

    /**
     * @brief 返回已写入字节的只读视图。
     * @retval 指向缓冲前 pos 字节的视图。
     */
    [[nodiscard]] std::span<const uint8_t> view() const noexcept
    {
        return buf.first(pos);
    }
};

/**
 * @brief 累积 UBX Fletcher-8 校验字节。
 * @param byte: 输入字节。
 * @param ckA: 入参与输出，校验字节 A。
 * @param ckB: 入参与输出，校验字节 B。
 */
void stepFletcher(uint8_t byte, uint8_t &ckA, uint8_t &ckB) noexcept;

/**
 * @brief 组装一条完整 UBX 帧（同步字、class/id、小端长度、载荷与 Fletcher 校验）。
 * @param[out] out: 目标命令缓冲（先清空）。
 * @param cls: 消息 class。
 * @param id: 消息 id。
 * @param payload: 载荷字节。
 * @retval true 组装成功；false 载荷超长或缓冲不足。
 */
[[nodiscard]] bool encodeUbxFrame(CmdBuf &out, uint8_t cls, uint8_t id,
                                  std::span<const uint8_t> payload) noexcept;

/**
 * @brief 校验一条 UBX 帧的同步字、长度字段与 Fletcher-8 校验和。
 * @param frame: 完整 UBX 帧字节。
 * @retval true 帧长一致且校验和匹配；false 帧残缺或校验失败。
 */
[[nodiscard]] bool validUbxChecksum(std::span<const uint8_t> frame) noexcept;

/**
 * @brief 生成关闭目标端口 12 条标准 NMEA 语句的单帧 4 字节头 CFG-VALSET。
 * @param[out] out: 目标命令缓冲（先清空）。
 * @param layer: 配置层位图（kLayerRam 或 kLayerRamBbr）。
 * @param port: 目标端口（1=UART1，2=UART2）。
 * @retval true 已编码；false 端口非法或缓冲不足。
 */
[[nodiscard]] bool buildSilenceNmeaValset(CmdBuf &out, uint8_t layer, uint8_t port) noexcept;

/**
 * @brief 判断一帧是否为 F9P 12 键 NMEA 静默 VALSET（UART1 或 UART2）。
 * @param frame: 完整 UBX 帧字节。
 * @retval true 键集合与静默 12 键一致；false 非静默帧或损坏。
 */
[[nodiscard]] bool isSilenceNmeaValset(std::span<const uint8_t> frame) noexcept;

/**
 * @brief 检测含 NMEA 高精度扩展键的端口配置 VALSET，并构造去掉这两键的 3 键降级帧。
 * @param frame: 被接收机 NAK 的完整 UBX-CFG-VALSET 帧。
 * @param[out] out: 成功时写入保留原配置层与基础键的降级 VALSET 帧。
 * @retval true 原帧含 CFG-NMEA-HIGHPREC/SVNUMBERING 且已生成降级帧；false 非此类帧或编码失败。
 */
[[nodiscard]] bool buildPortConfigFallbackValset(std::span<const uint8_t> frame,
                                                 CmdBuf &out) noexcept;

/**
 * @brief 判断键集合中是否包含 NMEA 高精度扩展键（CFG-NMEA-HIGHPREC 或 CFG-NMEA-SVNUMBERING）。
 * @param keys: VALSET 键 ID 列表。
 * @retval true 包含至少一个高精度扩展键；false 不包含。
 */
[[nodiscard]] bool containsNmeaHighPrecKeys(std::span<const uint32_t> keys) noexcept;
/**
 * @brief 解析 MON-VER 中的 F9P HPG 固件版本号（如 "HPG 1.32" -> 132）。
 * @param fwVersion: MON-VER FWVER 文本（允许定长缓冲内含尾 NUL）。
 * @param[out] versionOut: 解析成功时输出 major * 100 + minor。
 * @retval true 解析成功；false 非 HPG 文本或格式无效。
 */
[[nodiscard]] bool parseF9pHpgVersion(std::string_view fwVersion, uint16_t &versionOut) noexcept;

/**
 * @brief 判断指定 MON-VER 固件版本是否支持 CFG-SIGNAL-SBAS_ENA 控制（SparkFun GNSS_ZED.h:36）。
 * @param fwVersion: MON-VER FWVER 文本。
 * @retval true HPG >= 1.13；false 1.12 或未知版本。
 */
[[nodiscard]] bool f9pSupportsSbas(std::string_view fwVersion) noexcept;

/**
 * @brief 生成 F9P 截止角与星座单帧原子 CFG-VALSET（RAM|BBR，显式写 GPS+GPS L1C/A 与 QZSS+QZSS L1C/A）。
 * @param[out] out: 目标命令缓冲（先清空）。
 * @param cfg: 截止角与星座配置（F9P 强制要求 cfg.gps 为 true）。
 * @param includeSbas: 是否包含 CFG-SIGNAL-SBAS_ENA（仅 HPG >= 1.13 置 true）。
 * @retval true 已编码；false 截止角越界、GPS 关闭或缓冲不足。
 */
[[nodiscard]] bool buildSatTrackValset(CmdBuf &out, const SatMaskConfig &cfg,
                                       bool includeSbas) noexcept;

/**
 * @brief 生成 UBX-SEC-UNIQID 轮询帧（空载荷，8 字节）。
 * @param[out] out: 目标命令缓冲（先清空）。
 * @retval true 已写入完整帧；false 缓冲不足。
 */
[[nodiscard]] bool buildPollSecUniqId(CmdBuf &out) noexcept;

/**
 * @brief 解析 UBX-SEC-UNIQID（version 1，9 B 载荷）为 10 位大写十六进制序列号。
 * @param frame: 完整 UBX 帧字节。
 * @param[out] out: 输出文本缓冲（至少 11 字节以容纳 10 字符 + NUL）。
 * @retval 10 解析成功并写入 NUL 结尾十六进制串；0 非 SEC-UNIQID、校验失败、截断或缓冲不足。
 */
[[nodiscard]] size_t parseSecUniqId(std::span<const uint8_t> frame, std::span<char> out) noexcept;

/**
 * @brief 判断帧是否为合法的 UBX-CFG-VALSET（4 字节 v0 头、含有效校验和）。
 * @param frame: 完整 UBX 帧字节。
 * @retval true 合法 VALSET；false 非 VALSET 或损坏。
 */
[[nodiscard]] bool isValsetFrame(std::span<const uint8_t> frame) noexcept;

/**
 * @brief 提取 UBX-CFG-VALSET 帧中的全部配置键 ID（按 UBX 键位宽解码）。
 * @param frame: 完整 UBX-CFG-VALSET 帧。
 * @param[out] keysOut: 接收键 ID 的定长缓冲。
 * @retval 提取到的键个数；0 表示帧非法、载荷截断或未知位宽。
 */
[[nodiscard]] size_t extractValsetKeys(std::span<const uint8_t> frame,
                                       std::span<uint32_t> keysOut) noexcept;

/**
 * @brief 将 UBX 键 ID 列表格式化为逗号分隔的十六进制文本（如 "0x201100A4,0x1031001F"）。
 * @param keys: 键 ID 列表（空时输出 "none"）。
 * @param[out] out: 输出文本缓冲（始终 NUL 结尾，超长时安全截断）。
 * @retval 写入字符数（不含 NUL）。
 */
[[nodiscard]] size_t formatValsetKeys(std::span<const uint32_t> keys, std::span<char> out) noexcept;

/**
 * @brief 判断帧是否为针对 UBX-CFG-VALSET（0x06/0x8A）的合法 UBX-ACK-ACK。
 * @param frame: 完整 UBX 帧字节。
 * @retval true 是 VALSET 的 ACK-ACK；false 否。
 */
[[nodiscard]] bool isValsetAck(std::span<const uint8_t> frame) noexcept;

/**
 * @brief 判断帧是否为针对 UBX-CFG-VALSET（0x06/0x8A）的合法 UBX-ACK-NAK。
 * @param frame: 完整 UBX 帧字节。
 * @retval true 是 VALSET 的 ACK-NAK；false 否。
 */
[[nodiscard]] bool isValsetNak(std::span<const uint8_t> frame) noexcept;

/** VALSET 应答分类结果：区分匹配当前在途请求的 ACK/NAK 与需丢弃的未匹配迟到应答。 */
enum class ValsetReplyKind : uint8_t {
    kNone = 0U,
    kAck,
    kNak,
    kUnmatched,
};

/** 单在途 UBX-CFG-VALSET 事务跟踪器：绑定在途键表并在超时后执行 300 ms 重同步丢弃窗口。 */
class ValsetTracker final {
public:
    /** @brief 清空在途请求与重同步状态（重探或复位时调用）。 */
    void reset() noexcept;

    /**
     * @brief 记录新发送的命令帧；若为期望应答的 VALSET 则登记在途键表与超时截止。
     * @param frame: 已写入串口的命令帧字节。
     * @param nowMs: 当前单调时钟（ms）。
     * @param timeoutMs: 本条命令的应答超时（ms）。
     * @param expectReply: false 表示无应答命令（如切波特率），清空在途跟踪。
     */
    void onTx(std::span<const uint8_t> frame, uint32_t nowMs, uint32_t timeoutMs,
              bool expectReply = true) noexcept;

    /**
     * @brief 处理收到的一帧 UBX，判定是否匹配当前在途 VALSET。
     * @param frame: 已校验的完整 UBX 帧。
     * @param nowMs: 当前单调时钟（ms）。
     * @param[out] keysOut: 返回 kAck/kNak 时指向被匹配的在途键表，其余情况为空。
     * @retval kNone 非 VALSET ACK/NAK；kAck/kNak 匹配当前在途命令；kUnmatched 空闲或重同步期迟到/重复应答。
     */
    [[nodiscard]] ValsetReplyKind onFrame(std::span<const uint8_t> frame, uint32_t nowMs,
                                          std::span<const uint32_t> &keysOut) noexcept;

    /**
     * @brief 轮询在途 VALSET 超时与重同步窗口结束；超时瞬间触发一次并进入 300 ms 重同步。
     * @param nowMs: 当前单调时钟（ms）。
     * @param[out] keysOut: 超时触发时返回该次在途键表（供单次告警日志输出）。
     * @retval true 本轮刚发生超时（仅触发一次）；false 未超时。
     */
    [[nodiscard]] bool pollTimeout(uint32_t nowMs, std::span<const uint32_t> &keysOut) noexcept;

    /**
     * @brief 主动进入 300 ms 重同步窗口（丢弃迟到或重复 ACK/NAK）。
     * @param nowMs: 当前单调时钟（ms）。
     */
    void enterResync(uint32_t nowMs) noexcept;

    /**
     * @brief 查询是否仍在 300 ms 重同步窗口内。
     * @param nowMs: 当前单调时钟（ms）。
     * @retval true 仍在重同步窗口内；false 已结束或未处于重同步。
     */
    [[nodiscard]] bool resyncing(uint32_t nowMs) const noexcept;

    /**
     * @brief 查询当前是否有等待应答的在途 VALSET。
     * @retval true 有在途 VALSET；false 无。
     */
    [[nodiscard]] bool inFlight() const noexcept
    {
        return in_flight_;
    }

    /**
     * @brief 返回最近记录的在途键表只读视图。
     * @retval 键 ID 切片。
     */
    [[nodiscard]] std::span<const uint32_t> keys() const noexcept
    {
        return std::span<const uint32_t>{keys_.data(), key_count_};
    }

private:
    std::array<uint32_t, kMaxExtractedValsetKeys> keys_{};
    size_t key_count_ = 0U;
    bool in_flight_ = false;
    bool resyncing_ = false;
    uint32_t deadline_ms_ = 0U;
    uint32_t resync_until_ms_ = 0U;
};

} // namespace gnss::ublox
