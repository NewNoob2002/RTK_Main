// GNSS 接收机驱动接口（ADR-0012 §2.2）：按厂家派生的驱动类，替代 ADR-0010 的函数指针方言表。
// 驱动只负责“字节怎么写、应答怎么认、帧是哪一类”，不做 IO、不计时；时序/重试集中在 gnss::Controller。
// 所有成员函数 const noexcept、无可变状态：同样输入产生同样字节，便于重试与主机逐字节测试。
// 线程安全：是（纯函数；实例为 constinit 静态对象，构造后只读）。ISR 安全：是（不阻塞、不分配）。
// 冻结接口（ADR-0012 §3）。实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "fw/Status.hpp"
#include "gnss/Types.hpp"

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
    uint8_t rate_hz = 1;    // 定位输出频率：1/2/5/10/20（驱动把不支持的值向下取到支持值）
    bool gsv = true;        // 是否输出 GSV（卫星表），GSV 与 GGA 同频率或 1 Hz 取较小者
    uint8_t port = 2;       // 模块上连接 ESP32 的串口编号（UM980 COM2；F9P UART1=1）
    uint32_t port_baud = 0; // 0 = 不改波特率；否则配置末尾切换到此波特率
};

/** 配置步骤默认等待应答时限。 */
inline constexpr uint16_t kStepTimeoutMs = 500;

/** 配置脚本中的一步。 */
struct ConfigStep {
    CmdBuf cmd{};
    bool expect_reply = true;             // false：发送后不等待应答，直接进入下一步
    uint16_t timeout_ms = kStepTimeoutMs; // 等待应答时限
    uint32_t switch_baud = 0;             // 非 0：本步确认（或无需确认时发送）后，主机 UART 改到此波特率
};

/** 应答判定（原 gnss::Reply；按 CODING-STYLE 规则 4 改名，避免与 core::Reply 重名）。 */
enum class Ack : uint8_t {
    kNone, // 与本步无关的帧
    kAck,  // 本步成功
    kNack, // 本步被模块拒绝
};

/**
 * 一次输出计划：每类报文的期望周期，0 = 关闭。下标即 OutputMsg 数值。
 * 由 main 根据 NVS gnss/rate、gnss/gsv、gnss/out 生成（ADR-0012 §2.4）。
 */
struct OutputPlan {
    std::array<uint32_t, kOutputMsgCount> period_ms{};
    uint8_t port = 2; // 模块上接 ESP32 的口号（UM980 COM2；F9P UART1=1）
};

/**
 * 接收机驱动基类。实例定义在驱动的 .cpp 内（`constinit` 文件作用域静态对象），地址即身份；
 * 通过驱动头导出的访问器取得 `const Receiver &`。
 *
 * 默认实现（本卡可用到的）：output_mask = GGA|RMC|GSA|GSV；build_output = kNotSupported；
 * classify_frame = 通用 NMEA（忽略 talker）；frame_tag = 通用 NMEA/RTCM3 标签；
 * build_readback = false；match_readback = 0。
 */
class Receiver {
public:
    // ---- 身份 ----
    [[nodiscard]] virtual Family family() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0; // "unicore" 等，诊断用
    /** 本驱动需要额外挂到 UART 分流器上的解码器（NMEA、RTCM3 由 main 固定挂载，按地址去重）。 */
    [[nodiscard]] virtual std::span<const demux::Decoder *const> decoders() const noexcept = 0;
    [[nodiscard]] virtual uint32_t caps(Model model) const noexcept = 0;

    // ---- 探测 ----
    /** 生成版本查询命令；返回 false 表示本驱动无法生成（控制器跳过该候选）。 */
    [[nodiscard]] virtual bool build_probe(CmdBuf &out) const noexcept = 0;
    /** frame 是否为版本查询的应答；是则填 out 并返回 true。dec 为分流器识别出的解码器。 */
    [[nodiscard]] virtual bool match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                           ProbeResult &out) const noexcept = 0;

    // ---- 基础配置脚本（流动站模式、RTCM 输入口、波特率） ----
    /**
     * 配置脚本第 index 步（从 0 开始）。index 超出脚本长度返回 false（脚本结束）。
     * 必须是纯函数：同样参数生成同样字节，便于重试与主机测试。
     */
    [[nodiscard]] virtual bool rover_step(size_t index, Model model, const RoverConfig &cfg,
                                          ConfigStep &out) const noexcept = 0;
    /** 判定 frame 是否为 sent（本步已发送的命令）的应答。 */
    [[nodiscard]] virtual Ack classify_reply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                             std::span<const uint8_t> sent) const noexcept = 0;

    // ---- 报文输出（ADR-0012 §2.4；本卡只提供默认与掩码，build_output 在 P3b 实现） ----
    /** 本型号能输出的报文（位 = OutputMsg）。App 订阅与控制台据此拒绝不支持的报文。 */
    [[nodiscard]] virtual uint32_t output_mask(Model model) const noexcept;
    /** 生成“把 msg 设为 period_ms（0 = 关闭）”的一条命令。不支持返回 kNotSupported。 */
    [[nodiscard]] virtual fw::Status build_output(OutputMsg msg, uint32_t period_ms, Model model,
                                                  const OutputPlan &plan, CmdBuf &out) const noexcept;
    /** 把分流器给出的一帧归类为 OutputMsg；非 App 报文返回 OutputMsg::kNone。 */
    [[nodiscard]] virtual OutputMsg classify_frame(const demux::Decoder &dec,
                                                   std::span<const uint8_t> frame) const noexcept;
    /** 日志用的短标签（"NMEA GNGGA"、"RTCM3 1005"），写入 out，返回长度（不含 NUL）。 */
    virtual size_t frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                             std::span<char> out) const noexcept;

    // ---- 配置读回（ADR-0012 §2.7；P3c 实现） ----
    /** 生成查询模块当前配置的命令；不支持返回 false。 */
    [[nodiscard]] virtual bool build_readback(CmdBuf &out) const noexcept;
    /** frame 是否属于读回结果；是则把一行可读文本写入 out 并返回长度，否则 0。 */
    virtual size_t match_readback(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  std::span<char> out) const noexcept;

    // 身份即地址（驱动以 constinit 静态对象存活），与 demux::Decoder 同约定：拷贝/移动一律删除。
    // 删除声明放在 public 段是 clang-tidy modernize-use-equals-delete 的要求（放在任何段行为相同）。
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;
    Receiver(Receiver &&) = delete;
    Receiver &operator=(Receiver &&) = delete;

protected:
    constexpr Receiver() = default;
    // 非虚、protected：多态只经基类释放是不允许的，驱动对象不会在堆上创建或删除。
    ~Receiver() = default;
};

/**
 * 本次构建启用的驱动，顺序即探测优先级（DriverList.inc 的行序）。
 * 未编译进来的驱动不出现在表中。线程/ISR 安全：是（静态常量表）。
 */
[[nodiscard]] std::span<const Receiver *const> enabled_receivers() noexcept;

/** 按家族取启用的驱动；未启用或未知返回 nullptr（调用方回落 null_receiver()）。 */
[[nodiscard]] const Receiver *receiver_for(Family family) noexcept;

} // namespace gnss
