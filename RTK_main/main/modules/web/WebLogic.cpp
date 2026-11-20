// 网页配置协议层：配置、能力和状态快照的 JSON 编码。
// 仅 kLink 任务或主机测试单线程调用，不阻塞、不分配堆，非 ISR。
#include "modules/web/WebLogic.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/services/GnssService.hpp"
#include "def/GnssDef.hpp"
#include "def/StorageDef.hpp"
#include "modules/gnss/GnssCmdPack.hpp"
#include "web_proto/Writer.hpp"

namespace app::web_cfg {
namespace {

constexpr int32_t kProtoVersion = 1;
constexpr size_t kMacTextBytes = 18U; // "AA:BB:CC:DD:EE:FF\0"
constexpr size_t kMacChars = 17U;

/**
 * @brief 将 6 字节蓝牙地址格式化为大写 `AA:BB:CC:DD:EE:FF`
 * @param addr: 6 字节地址
 * @param[out] out: 18 字节输出缓冲
 * @retval 指向 out 前 17 字符的视图
 * @note 纯函数，不阻塞、不分配堆。
 */
std::string_view formatMacUpper(const core::BtAddr &addr,
                                std::array<char, kMacTextBytes> &out) noexcept
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    constexpr uint8_t kNibbleMask = 0x0FU;
    constexpr unsigned kHighShift = 4U;
    size_t pos = 0U;
    for (size_t i = 0U; i < core::kAddrLen; ++i) {
        if (i > 0U) {
            out[pos++] = ':';
        }
        out[pos++] = kHex[static_cast<size_t>((addr.bytes[i] >> kHighShift) & kNibbleMask)];
        out[pos++] = kHex[static_cast<size_t>(addr.bytes[i] & kNibbleMask)];
    }
    out[pos] = '\0';
    return std::string_view{out.data(), kMacChars};
}

/**
 * @brief 取定长字符数组中的安全 C 字符串视图（防止缺 NUL 越界）
 * @param buf: 字符数组视图
 * @retval 截至首个 '\0' 的视图
 * @note 纯函数，不阻塞。
 */
std::string_view boundedView(std::span<const char> buf) noexcept
{
    size_t len = 0U;
    while ((len < buf.size()) && (buf[len] != '\0')) {
        ++len;
    }
    return std::string_view{buf.data(), len};
}

/**
 * @brief 写入 `status.dev` 嵌套对象
 * @param writer: 已打开根对象的 JsonWriter
 * @param dev: 设备状态分组
 * @retval true 写入成功
 * @retval false 缓冲不足或状态错误
 */
bool writeDevGroup(web::JsonWriter &writer, const DevWebStatus &dev) noexcept
{
    return writer.beginNested("dev") && writer.writeInt("uptime_s", dev.uptimeS) &&
           writer.writeInt("heap_free", dev.heapFree) && writer.writeInt("heap_min", dev.heapMin) &&
           writer.writeString("reset", dev.reset) && writer.endNested();
}

/**
 * @brief 写入 `status.gnss` 嵌套对象（数值不可得时省略对应字段）
 * @param writer: 已打开根对象的 JsonWriter
 * @param gnss: GNSS 状态分组
 * @retval true 写入成功
 * @retval false 缓冲不足或状态错误
 */
bool writeGnssGroup(web::JsonWriter &writer, const GnssWebStatus &gnss) noexcept
{
    bool ok = writer.beginNested("gnss") && writer.writeString("state", gnss.state);
    if (ok && gnss.hasModel) {
        ok = writer.writeString("model", boundedView(gnss.model));
    }
    if (ok && gnss.hasFw) {
        ok = writer.writeString("fw", boundedView(gnss.fw));
    }
    if (ok) {
        ok = writer.writeString("fix", gnss.fix);
    }
    if (ok && gnss.hasSats) {
        ok = writer.writeInt("sats", gnss.sats);
    }
    if (ok && gnss.hasHacc) {
        ok = writer.writeInt("hacc_mm", gnss.haccMm);
    }
    if (ok && gnss.hasHz) {
        ok = writer.writeInt("hz", gnss.hz);
    }
    return ok && writer.endNested();
}

/**
 * @brief 写入 `status.bt` 嵌套对象
 * @param writer: 已打开根对象的 JsonWriter
 * @param bt: 蓝牙状态分组
 * @retval true 写入成功
 * @retval false 缓冲不足或状态错误
 */
bool writeBtGroup(web::JsonWriter &writer, const BtWebStatus &bt) noexcept
{
    std::array<std::array<char, kMacTextBytes>, kMaxAlarms> formatted{};
    std::array<std::string_view, kMaxAlarms> views{};
    const size_t count = std::min(bt.whitelistCount, kMaxAlarms);
    for (size_t i = 0U; i < count; ++i) {
        views[i] = formatMacUpper(bt.whitelist[i], formatted[i]);
    }
    return writer.beginNested("bt") &&
           writer.writeStringArray("whitelist",
                                   std::span<const std::string_view>{views}.first(count)) &&
           writer.endNested();
}

} // namespace

const char *gnssStateName(::gnss::State state) noexcept
{
    switch (state) {
    case ::gnss::State::kOff:
        return "off";
    case ::gnss::State::kPowerUp:
    case ::gnss::State::kProbing:
        return "probing";
    case ::gnss::State::kConfiguring:
    case ::gnss::State::kOutput:
        return "configuring";
    case ::gnss::State::kRunning:
    case ::gnss::State::kReadback:
        return "running";
    case ::gnss::State::kMaintenance:
        return "maintenance";
    case ::gnss::State::kFailed:
    default:
        return "failed";
    }
}

const char *gnssFixName(const ::gnss::Fix &fix) noexcept
{
    if (!fix.valid) {
        return "none";
    }
    switch (fix.quality) {
    case ::gnss::FixQuality::kRtkFixed:
        return "fixed";
    case ::gnss::FixQuality::kRtkFloat:
        return "float";
    case ::gnss::FixQuality::kDgps:
        return "dgps";
    case ::gnss::FixQuality::kSingle:
    case ::gnss::FixQuality::kPps:
    case ::gnss::FixQuality::kDeadReckoning:
    case ::gnss::FixQuality::kManual:
    case ::gnss::FixQuality::kSimulator:
        return "single";
    case ::gnss::FixQuality::kInvalid:
    case ::gnss::FixQuality::kUnknown:
    default:
        return "none";
    }
}

namespace {

/** @brief 写单个设置值。 @param writer: JSON 编码器。 @param key: JSON 字段名。 @param value: 设置快照。 @retval 是否成功。 */
bool writeSetting(web::JsonWriter &writer, std::string_view key,
                  const settings::Value &value) noexcept
{
    switch (value.kind) {
    case settings::ValueKind::kInt:
        return writer.writeInt(key, value.number);
    case settings::ValueKind::kBool:
        return writer.writeBool(key, value.boolVal);
    case settings::ValueKind::kString:
        return writer.writeString(key, value.textView());
    case settings::ValueKind::kBytes:
    default:
        // kBytes 仅用于手机专用键（如 phone.sn_pn），由 buildConfigFrame 的 access::kWeb 过滤排除；
        // 显式归入拒绝分支防止将来放宽权限时误输出二进制字段。
        return false;
    }
}

/** @brief 写取值域。 @param writer: JSON 编码器。 @param spec: 键定义。 @param domain: 当前取值域。 @retval 是否成功。 */
bool writeDomain(web::JsonWriter &writer, const settings::KeySpec &spec,
                 const settings::Domain &domain) noexcept
{
    constexpr std::array<int32_t, 2U> kBoolean{0, 1};
    switch (domain.kind) {
    case settings::KeyKind::kEnum:
        return writer.writeIntArray(spec.name, domain.values());
    case settings::KeyKind::kBool:
        return writer.writeIntArray(spec.name, kBoolean);
    case settings::KeyKind::kMac:
    case settings::KeyKind::kSn:
    case settings::KeyKind::kIntRange:
        return writer.beginNested(spec.name) && writer.writeInt("min", domain.minValue) &&
               writer.writeInt("max", domain.maxValue) && writer.endNested();
    default:
        return false;
    }
}

/** @brief 写完整 caps 对象。 @param writer: JSON 编码器。 @param service: 设置服务。 @param caps: 已发布快照。 @param gnssEnabled: GNSS 是否可用。 @retval 是否成功。 */
bool writeCaps(web::JsonWriter &writer, const settings::Service &service,
               const settings::GnssCaps &caps, bool gnssEnabled) noexcept
{
    bool ok = writer.beginNested("caps");
    service.forEach([&](const settings::KeySpec &spec) noexcept {
        if (!ok || (spec.access & settings::access::kWeb) == 0U || spec.encode == nullptr) {
            return;
        }
        settings::Domain domain{};
        spec.domain(caps, gnssEnabled, domain);
        if (domain.supported) {
            ok = writeDomain(writer, spec, domain);
        }
    });
    return ok && writer.endNested();
}

} // namespace

web::ErrorCode fromSettingsError(settings::Error error) noexcept
{
    switch (error) {
    case settings::Error::kOk:
        return web::ErrorCode::kNone;
    case settings::Error::kUnknownKey:
        return web::ErrorCode::kUnknownKey;
    case settings::Error::kBadValue:
        return web::ErrorCode::kBadValue;
    case settings::Error::kLocked:
        return web::ErrorCode::kLocked;
    case settings::Error::kBusy:
        return web::ErrorCode::kBusy;
    case settings::Error::kUnsupported:
        return web::ErrorCode::kUnsupported;
    case settings::Error::kTimeout:
        return web::ErrorCode::kTimeout;
    case settings::Error::kFailed:
    default:
        return web::ErrorCode::kFailed;
    }
}

size_t buildHelloFrame(const HelloSnapshot &snap, const settings::Service &service,
                       const settings::GnssCaps &caps, bool gnssEnabled,
                       std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    const bool ok = writer.beginObject() && writer.writeString("t", "hello") &&
                    writer.writeInt("proto", kProtoVersion) &&
                    writer.writeString("board", snap.board) && writer.writeString("fw", snap.fw) &&
                    writer.writeString("bt_name", snap.btName) &&
                    writer.writeString("console", snap.console) &&
                    writer.writeBool("factory", snap.factoryUnlocked) &&
                    writeCaps(writer, service, caps, gnssEnabled) && writer.endObject();
    return ok ? writer.finish(out) : 0U;
}

size_t buildCapsFrame(const settings::Service &service, const settings::GnssCaps &caps,
                      bool gnssEnabled, std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    const bool ok = writer.beginObject() && writer.writeString("t", "caps") &&
                    writeCaps(writer, service, caps, gnssEnabled) && writer.endObject();
    return ok ? writer.finish(out) : 0U;
}

size_t buildConfigFrame(const settings::Service &service, std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    bool ok = writer.beginObject() && writer.writeString("t", "config");
    bool gateOpen = false;
    constexpr std::string_view kGatePrefix = "gnss.gga_gate.";
    service.forEach([&](const settings::KeySpec &spec) noexcept {
        settings::Value value{};
        if (!ok || (spec.access & settings::access::kWeb) == 0U || !service.get(spec.id, value)) {
            return;
        }
        if (spec.name.starts_with(kGatePrefix)) {
            auto member = spec.name;
            member.remove_prefix(kGatePrefix.size());
            ok =
                (gateOpen || writer.beginNested("gga_gate")) && writeSetting(writer, member, value);
            gateOpen = true;
            return;
        }
        ok = !gateOpen || writer.endNested();
        if (ok) {
            // v1 将配对开关编码为整数，设置服务中的值使用布尔类型。
            ok = spec.id == settings::KeyId::kBtPairNext
                     ? writer.writeInt(spec.name, value.boolVal ? 1 : 0)
                     : writeSetting(writer, spec.name, value);
        }
        gateOpen = false;
    });
    ok = ok && (!gateOpen || writer.endNested()) && writer.endObject();
    return ok ? writer.finish(out) : 0U;
}

size_t buildStatusFrame(const StatusSnapshot &cur, const StatusSnapshot *prev,
                        std::span<char> out) noexcept
{
    const bool sendDev = (prev == nullptr) || (cur.dev != prev->dev);
    const bool sendGnss = (prev == nullptr) || (cur.gnss != prev->gnss);
    const bool sendBt = (prev == nullptr) || (cur.bt != prev->bt);
    if (!sendDev && !sendGnss && !sendBt) {
        return 0U;
    }
    web::JsonWriter writer{};
    bool ok = writer.beginObject() && writer.writeString("t", "status");
    if (ok && sendDev) {
        ok = writeDevGroup(writer, cur.dev);
    }
    if (ok && sendGnss) {
        ok = writeGnssGroup(writer, cur.gnss);
    }
    if (ok && sendBt) {
        ok = writeBtGroup(writer, cur.bt);
    }
    return (ok && writer.endObject()) ? writer.finish(out) : 0U;
}

size_t buildAckFrame(uint16_t id, web::ErrorCode error, std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    bool ok = writer.beginObject() && writer.writeString("t", "ack") &&
              writer.writeInt("id", static_cast<int32_t>(id)) &&
              writer.writeBool("ok", error == web::ErrorCode::kNone);
    if (ok && (error != web::ErrorCode::kNone)) {
        ok = writer.writeString("err", web::toString(error));
    }
    return (ok && writer.endObject()) ? writer.finish(out) : 0U;
}

size_t buildByeFrame(std::string_view reason, std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    const bool ok = writer.beginObject() && writer.writeString("t", "bye") &&
                    writer.writeString("reason", reason) && writer.endObject();
    return ok ? writer.finish(out) : 0U;
}

} // namespace app::web_cfg
