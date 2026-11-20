// 网页配置协议纯逻辑层实现：set 命令翻译、JSON 帧构造与 status 分组差分。
// 仅 kLink 任务或主机测试单线程调用，不阻塞、不分配堆，非 ISR。
#include "modules/web/WebLogic.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/services/GnssService.hpp"
#include "def/GnssDef.hpp"
#include "def/StorageDef.hpp"
#include "modules/gnss/GnssCmdPack.hpp"
#include "web_proto/KeyTable.hpp"
#include "web_proto/Writer.hpp"

namespace app::web_cfg {
namespace {

constexpr int32_t kProtoVersion = 1;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr size_t kMacTextBytes = 18U; // "AA:BB:CC:DD:EE:FF\0"
constexpr size_t kMacChars = 17U;

/**
 * @brief 将协议 `gnss.out.*` 周期毫秒转换为 GnssOutConfig 的有符号间隔编码
 * @param periodMs: 允许的周期毫秒值（0/50/100/200/500/1000）
 * @param[out] interval: 成功时写入 int8_t 间隔（0/-1/2/5/10/20）
 * @retval true 周期受支持
 * @retval false 不在支持集合内
 * @note 纯函数；显式 1000 ms 使用 -1（与控制台 gnss out 一致，避免随定位频率变化）。
 */
bool periodMsToInterval(int32_t periodMs, int8_t &interval) noexcept
{
    constexpr int32_t kPeriod20Hz = 50;
    constexpr int32_t kPeriod10Hz = 100;
    constexpr int32_t kPeriod5Hz = 200;
    constexpr int32_t kPeriod2Hz = 500;
    constexpr int32_t kPeriod1Hz = 1000;
    constexpr int8_t kInterval20Hz = 20;
    constexpr int8_t kInterval10Hz = 10;
    constexpr int8_t kInterval5Hz = 5;
    constexpr int8_t kInterval2Hz = 2;
    constexpr int8_t kInterval1Hz = -1;
    switch (periodMs) {
    case 0:
        interval = 0;
        return true;
    case kPeriod1Hz:
        interval = kInterval1Hz;
        return true;
    case kPeriod2Hz:
        interval = kInterval2Hz;
        return true;
    case kPeriod5Hz:
        interval = kInterval5Hz;
        return true;
    case kPeriod10Hz:
        interval = kInterval10Hz;
        return true;
    case kPeriod20Hz:
        interval = kInterval20Hz;
        return true;
    default:
        return false;
    }
}

/**
 * @brief 将 `gnss.out.*` 键名映射到 App 输出语句下标
 * @param key: 键名
 * @param[out] outIdx: 成功时写入 0..5 下标（gsv 映射到 kSatsInfo）
 * @retval true 匹配某条输出键
 * @retval false 非输出键
 * @note 纯函数，与 GnssCmds.cpp 保持同一别名映射（gsv -> sats）。
 */
bool outKeyToIndex(std::string_view key, size_t &outIdx) noexcept
{
    if (key == "gnss.out.gga") {
        outIdx = static_cast<size_t>(::gnss::OutputMsg::kGga);
        return true;
    }
    if (key == "gnss.out.gsv") {
        outIdx = static_cast<size_t>(::gnss::OutputMsg::kSatsInfo);
        return true;
    }
    if (key == "gnss.out.gst") {
        outIdx = static_cast<size_t>(::gnss::OutputMsg::kGst);
        return true;
    }
    if (key == "gnss.out.rmc") {
        outIdx = static_cast<size_t>(::gnss::OutputMsg::kRmc);
        return true;
    }
    if (key == "gnss.out.gsa") {
        outIdx = static_cast<size_t>(::gnss::OutputMsg::kGsa);
        return true;
    }
    return false;
}

/**
 * @brief 构造 GNSS 输出频率变更（`gnss.out.*`）的 SetPlan
 * @param typeIdx: App 输出语句下标
 * @param periodMs: 校验后的周期毫秒
 * @param curOutCfg: 当前生效输出配置
 * @retval 生成的 SetPlan
 * @note 纯函数，不阻塞、不分配堆。
 */
SetPlan buildOutSetPlan(size_t typeIdx, int32_t periodMs,
                        const data::GnssOutConfig &curOutCfg) noexcept
{
    int8_t interval = 0;
    if (!periodMsToInterval(periodMs, interval)) {
        return {.error = web::ErrorCode::kBadValue};
    }
    data::GnssOutConfig next = curOutCfg.isValid() ? curOutCfg : data::GnssOutConfig::defaults();
    if (!fw::isOk(next.applyInterval(typeIdx, interval))) {
        return {.error = web::ErrorCode::kBadValue};
    }
    bool encodedOk = false;
    const fw::Command cmd = detail::packGnssOut(next, encodedOk);
    if (!encodedOk) {
        return {.error = web::ErrorCode::kFailed};
    }
    return {
        .error = web::ErrorCode::kNone,
        .target = CommandTarget::kGnss,
        .waitGnssApply = true,
        .notifyBtStatus = false,
        .command = cmd,
    };
}

/**
 * @brief 构造存储侧（白名单、pair_next、SN）的 SetPlan
 * @param key: 已通过校验的键名
 * @param val: 规范值
 * @retval 生成的 SetPlan
 * @note 纯函数，不阻塞、不分配堆。
 */
SetPlan buildStorageSetPlan(std::string_view key, const web::CanonicalValue &val) noexcept
{
    SetPlan plan{};
    plan.target = CommandTarget::kStorage;
    plan.command.target = id(ModuleKind::kStorage);
    if ((key == "bt.wl.add") || (key == "bt.wl.del")) {
        if (val.size != core::kAddrLen) {
            return {.error = web::ErrorCode::kBadValue};
        }
        plan.command.verb =
            verb((key == "bt.wl.add") ? StorageVerb::kWhitelistAdd : StorageVerb::kWhitelistDel);
        plan.command.argLen = static_cast<uint8_t>(core::kAddrLen);
        std::copy_n(val.bytes.begin(), core::kAddrLen, plan.command.args.begin());
        plan.notifyBtStatus = true;
        return plan;
    }
    if (key == "bt.pair_next") {
        plan.command.verb = verb(StorageVerb::kSetPairNext);
        plan.command.argLen = 1U;
        plan.command.args[0] = (val.number != 0) ? 1U : 0U;
        return plan;
    }
    if (key == "factory.sn") {
        if ((val.size == 0U) || (val.size > plan.command.args.size())) {
            return {.error = web::ErrorCode::kBadValue};
        }
        plan.command.verb = verb(StorageVerb::kSaveSn);
        plan.command.argLen = val.size;
        std::copy_n(val.bytes.begin(), val.size, plan.command.args.begin());
        return plan;
    }
    return {.error = web::ErrorCode::kUnknownKey};
}

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

const char *webGnssStateName(::gnss::State state) noexcept
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

const char *webGnssFixName(const ::gnss::Fix &fix) noexcept
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

int32_t effectiveOutPeriodMs(const data::GnssOutConfig &outCfg, uint8_t rateHz,
                             size_t typeIdx) noexcept
{
    if (typeIdx >= data::kGnssAppMsgCount) {
        return 0;
    }
    const data::GnssOutConfig effective =
        outCfg.isValid() ? outCfg : data::GnssOutConfig::defaults();
    if ((effective.mask & (1U << typeIdx)) == 0U) {
        return 0;
    }
    const uint8_t hz = (rateHz == 0U) ? 1U : rateHz;
    if (data::followsFixRate(effective, typeIdx)) {
        // 1000 / hz 结果落在 50..1000，转为 int32_t 安全。
        return static_cast<int32_t>(kMsPerSecond / static_cast<uint32_t>(hz));
    }
    const uint32_t periodMs =
        core::services::GnssService::intervalToPeriodMs(effective.intervals[typeIdx]);
    // 周期上限为 128000 ms，远小于 INT32_MAX，转换安全。
    return static_cast<int32_t>(periodMs);
}

SetPlan translateSetRequest(std::string_view key, std::string_view value, bool unlocked,
                            const data::GnssOutConfig &curOutCfg) noexcept
{
    const web::KeySpec *spec = web::findKey(key);
    if (spec == nullptr) {
        return {.error = web::ErrorCode::kUnknownKey};
    }
    if (spec->needsUnlock && !unlocked) {
        return {.error = web::ErrorCode::kLocked};
    }
    // W0 中 CM680 星座命令尚未实机确认，v1 按协议返回 unsupported。
    if (spec->optional) {
        return {.error = web::ErrorCode::kUnsupported};
    }
    web::CanonicalValue canonical{};
    const web::ErrorCode valErr = web::validateValue(*spec, value, canonical);
    if (valErr != web::ErrorCode::kNone) {
        return {.error = valErr};
    }
    if (key == "gnss.rate") {
        fw::Command cmd{};
        cmd.target = id(ModuleKind::kGnss);
        cmd.verb = static_cast<uint8_t>(GnssVerb::kSetRate);
        cmd.argLen = 1U;
        // validateValue 已限制 rate 在 {1,2,5,10,20} 内，窄化到 uint8_t 安全。
        cmd.args[0] = static_cast<uint8_t>(canonical.number);
        return {.error = web::ErrorCode::kNone,
                .target = CommandTarget::kGnss,
                .waitGnssApply = true,
                .notifyBtStatus = false,
                .command = cmd};
    }
    size_t outIdx = 0U;
    if (outKeyToIndex(key, outIdx)) {
        return buildOutSetPlan(outIdx, canonical.number, curOutCfg);
    }
    if (key == "gnss.gga_gate.enabled") {
        return {.error = web::ErrorCode::kNone,
                .target = CommandTarget::kGnss,
                .waitGnssApply = false,
                .notifyBtStatus = false,
                .command = detail::packGnssGate(true, canonical.number != 0, false, 0U)};
    }
    if (key == "gnss.gga_gate.min_sats") {
        // validateValue 已限制 min_sats 在 0..40，窄化到 uint8_t 安全。
        return {.error = web::ErrorCode::kNone,
                .target = CommandTarget::kGnss,
                .waitGnssApply = false,
                .notifyBtStatus = false,
                .command = detail::packGnssGate(false, false, true,
                                                static_cast<uint8_t>(canonical.number))};
    }
    return buildStorageSetPlan(key, canonical);
}

size_t buildHelloFrame(const HelloSnapshot &snap, std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    const bool ok = writer.beginObject() && writer.writeString("t", "hello") &&
                    writer.writeInt("proto", kProtoVersion) &&
                    writer.writeString("board", snap.board) && writer.writeString("fw", snap.fw) &&
                    writer.writeString("bt_name", snap.btName) &&
                    writer.writeString("console", snap.console) &&
                    writer.writeBool("factory", snap.factoryUnlocked) && writer.endObject();
    return ok ? writer.finish(out) : 0U;
}

size_t buildConfigFrame(const ConfigSnapshot &snap, std::span<char> out) noexcept
{
    const uint8_t rateHz = (snap.gnssRateHz == 0U) ? 1U : snap.gnssRateHz;
    web::JsonWriter writer{};
    const bool ok =
        writer.beginObject() && writer.writeString("t", "config") &&
        writer.writeInt("gnss.rate", static_cast<int32_t>(rateHz)) &&
        writer.writeInt("gnss.out.gga",
                        effectiveOutPeriodMs(snap.gnssOut, rateHz,
                                             static_cast<size_t>(::gnss::OutputMsg::kGga))) &&
        writer.writeInt("gnss.out.gsa",
                        effectiveOutPeriodMs(snap.gnssOut, rateHz,
                                             static_cast<size_t>(::gnss::OutputMsg::kGsa))) &&
        writer.writeInt("gnss.out.gsv",
                        effectiveOutPeriodMs(snap.gnssOut, rateHz,
                                             static_cast<size_t>(::gnss::OutputMsg::kSatsInfo))) &&
        writer.writeInt("gnss.out.gst",
                        effectiveOutPeriodMs(snap.gnssOut, rateHz,
                                             static_cast<size_t>(::gnss::OutputMsg::kGst))) &&
        writer.writeInt("gnss.out.rmc",
                        effectiveOutPeriodMs(snap.gnssOut, rateHz,
                                             static_cast<size_t>(::gnss::OutputMsg::kRmc))) &&
        writer.beginNested("gga_gate") && writer.writeBool("enabled", snap.ggaGateEnabled) &&
        writer.writeInt("min_sats", static_cast<int32_t>(snap.ggaGateMinSats)) &&
        writer.endNested() && writer.writeInt("bt.pair_next", snap.btPairNext ? 1 : 0) &&
        writer.writeString("factory.sn", snap.sn) && writer.endObject();
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
