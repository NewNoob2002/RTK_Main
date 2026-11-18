// GNSS 工作模式与 gnss.base 持久化绑定、轮询推进与命令处理（ADR-0014 §2.4-§2.6）。
// 线程：load*/save* 由启动期单线程或持锁读写；cmdMode()/pollWorkMode() 仅 kIo 任务；非 ISR。
// 堆：不使用。

#include "modules/gnss/GnssModule.hpp"

#include <bit>
#include <cstdint>

#include "AppContext.hpp"
#include "def/GnssSubDef.hpp"
#include "fw/Lock.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/gnss/GnssModulePrivate.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

/**
 * @brief 读 2 字节小端 u16。
 * @param in: 至少 2 字节的输入视图。
 * @retval val: 小端解码后的 uint16_t。
 */
uint16_t readU16Le(std::span<const uint8_t> in) noexcept
{
    constexpr unsigned kByteShift = 8U;
    return static_cast<uint16_t>(in[0U]) |
           static_cast<uint16_t>(static_cast<uint16_t>(in[1U]) << kByteShift);
}

/**
 * @brief 写 2 字节小端 u16。
 * @param v: 待编码 u16。
 * @param out: 目标字节切片（至少 2 字节）。
 */
void writeU16Le(uint16_t v, std::span<uint8_t> out) noexcept
{
    constexpr unsigned kByteShift = 8U;
    constexpr uint16_t kMask = 0xFFU;
    out[0U] = static_cast<uint8_t>(v & kMask);
    out[1U] = static_cast<uint8_t>((v >> kByteShift) & kMask);
}

/** @brief 仅在 GnssModule 初始化后加自旋锁（早于 data.init() 的单线程期直接访问）。 */
struct OptionalMuxGuard {
    /**
     * @brief 按需对共享自旋锁上锁。
     * @param mux: DataCenter 自旋锁。
     * @param locked: true 时持锁。
     */
    explicit OptionalMuxGuard(portMUX_TYPE &mux, bool locked) noexcept
        : lock_(locked ? rtos::spinLockOps(mux) : fw::LockOps{})
    {
        if (lock_.lock != nullptr) {
            lock_.lock(lock_.ctx);
        }
    }
    /** @brief 若构造时持锁则在析构时释放。 */
    ~OptionalMuxGuard() noexcept
    {
        if (lock_.unlock != nullptr) {
            lock_.unlock(lock_.ctx);
        }
    }
    OptionalMuxGuard(const OptionalMuxGuard &) = delete;
    OptionalMuxGuard &operator=(const OptionalMuxGuard &) = delete;

private:
    fw::LockOps lock_{};
};

} // namespace

/**
 * @brief 跨任务安全读取 gnss.base 完整配置副本。
 * @retval cfg: 当前生效或默认的 gnss.base 配置。
 */
GnssBaseConfig GnssModule::readBaseConfig() noexcept
{
    const OptionalMuxGuard guard(ctx().data.mux, initialized_);
    return base_cfg_.has_value() ? *base_cfg_ : GnssBaseConfig::defaults();
}

/**
 * @brief 跨任务安全写入 gnss.base 完整配置并标记所有键已就位。
 * @param cfg: 新配置。
 */
void GnssModule::writeBaseConfig(const GnssBaseConfig &cfg) noexcept
{
    const OptionalMuxGuard guard(ctx().data.mux, initialized_);
    base_cfg_ = cfg;
    base_loaded_mask_ = static_cast<uint16_t>((1U << gnss_store::kBaseKeys.size()) - 1U);
}

void GnssModule::initBaseDefaults() noexcept
{
    const auto def = GnssBaseConfig::defaults();
    auto &cfg = ensureBaseCfg();
    const uint16_t mask = base_loaded_mask_;
    const auto missing = [mask](size_t idx) noexcept { return (mask & (1U << idx)) == 0U; };
    if (missing(gnss_store::kBaseModeKeyIdx)) {
        cfg.mode = def.mode;
    }
    if (missing(gnss_store::kBaseTypeKeyIdx)) {
        cfg.baseType = def.baseType;
    }
    if (missing(gnss_store::kBaseSvySKeyIdx)) {
        cfg.svyS = def.svyS;
    }
    if (missing(gnss_store::kBaseSvyAccCmKeyIdx)) {
        cfg.svyAccCm = def.svyAccCm;
    }
    if (missing(gnss_store::kBaseSvyToSKeyIdx)) {
        cfg.svyToS = def.svyToS;
    }
    if (missing(gnss_store::kBaseFixLatKeyIdx)) {
        cfg.fixLatE9 = def.fixLatE9;
    }
    if (missing(gnss_store::kBaseFixLonKeyIdx)) {
        cfg.fixLonE9 = def.fixLonE9;
    }
    if (missing(gnss_store::kBaseFixHMmKeyIdx)) {
        cfg.fixHeightMm = def.fixHeightMm;
    }
    if (missing(gnss_store::kBaseMsmKeyIdx)) {
        cfg.msm = def.msm;
    }
    if (missing(gnss_store::kBaseRtcmOutKeyIdx)) {
        cfg.rtcmOut = GnssBaseConfig::defaultRtcmOut(cfg.msm);
    }
}

/**
 * @brief 从 NVS 恢复 gnss.base/mode。
 * @param in: 1 字节编码。
 * @retval kOk: 加载成功；kInvalidArg: 值越界。
 */
fw::Result GnssModule::loadBaseMode(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || (in[0U] > static_cast<uint8_t>(WorkModeKind::kBase))) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().mode = static_cast<WorkModeKind>(in[0U]);
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseModeKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/mode 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（1）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseMode(const void * /*obj*/, std::span<uint8_t> out,
                                    size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kNoSpace;
    }
    out[0U] = static_cast<uint8_t>(appContext().gnss.readBaseConfig().mode);
    len = 1U;
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/base_type。
 * @param in: 1 字节编码。
 * @retval kOk: 加载成功；kInvalidArg: 值越界。
 */
fw::Result GnssModule::loadBaseType(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || (in[0U] > static_cast<uint8_t>(BaseType::kFixed))) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().baseType = static_cast<BaseType>(in[0U]);
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseTypeKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/base_type 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（1）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseType(const void * /*obj*/, std::span<uint8_t> out,
                                    size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kNoSpace;
    }
    out[0U] = static_cast<uint8_t>(appContext().gnss.readBaseConfig().baseType);
    len = 1U;
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/svy_s。
 * @param in: 2 字节小端 u16。
 * @retval kOk: 加载成功；kInvalidArg: 值越界或长度不足。
 */
fw::Result GnssModule::loadBaseSvyS(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() < sizeof(uint16_t)) {
        return fw::Result::kInvalidArg;
    }
    const uint16_t v = readU16Le(in);
    if ((v < GnssBaseConfig::kMinSvyS) || (v > GnssBaseConfig::kMaxSvyS)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().svyS = v;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseSvySKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/svy_s 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（2）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseSvyS(const void * /*obj*/, std::span<uint8_t> out,
                                    size_t &len) noexcept
{
    if (out.size() < sizeof(uint16_t)) {
        return fw::Result::kNoSpace;
    }
    writeU16Le(appContext().gnss.readBaseConfig().svyS, out);
    len = sizeof(uint16_t);
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/svy_acc_cm。
 * @param in: 2 字节小端 u16。
 * @retval kOk: 加载成功；kInvalidArg: 值越界或长度不足。
 */
fw::Result GnssModule::loadBaseSvyAccCm(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() < sizeof(uint16_t)) {
        return fw::Result::kInvalidArg;
    }
    const uint16_t v = readU16Le(in);
    if ((v < GnssBaseConfig::kMinSvyAccCm) || (v > GnssBaseConfig::kMaxSvyAccCm)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().svyAccCm = v;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseSvyAccCmKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/svy_acc_cm 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（2）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseSvyAccCm(const void * /*obj*/, std::span<uint8_t> out,
                                        size_t &len) noexcept
{
    if (out.size() < sizeof(uint16_t)) {
        return fw::Result::kNoSpace;
    }
    writeU16Le(appContext().gnss.readBaseConfig().svyAccCm, out);
    len = sizeof(uint16_t);
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/svy_to_s。
 * @param in: 2 字节小端 u16。
 * @retval kOk: 加载成功；kInvalidArg: 值越界或长度不足。
 */
fw::Result GnssModule::loadBaseSvyToS(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() < sizeof(uint16_t)) {
        return fw::Result::kInvalidArg;
    }
    const uint16_t v = readU16Le(in);
    if ((v < GnssBaseConfig::kMinSvyToS) || (v > GnssBaseConfig::kMaxSvyToS)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().svyToS = v;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseSvyToSKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/svy_to_s 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（2）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseSvyToS(const void * /*obj*/, std::span<uint8_t> out,
                                      size_t &len) noexcept
{
    if (out.size() < sizeof(uint16_t)) {
        return fw::Result::kNoSpace;
    }
    writeU16Le(appContext().gnss.readBaseConfig().svyToS, out);
    len = sizeof(uint16_t);
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/fix_lat（8B i64 LE）。
 * @param in: 8 字节 blob。
 * @retval kOk: 加载成功；kInvalidArg: 长度或纬度范围非法。
 */
fw::Result GnssModule::loadBaseFixLat(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != GnssBaseConfig::kFixCoordBlobBytes) {
        return fw::Result::kInvalidArg;
    }
    const int64_t v = decodeI64Le(in);
    if ((v < -GnssBaseConfig::kMaxLatE9) || (v > GnssBaseConfig::kMaxLatE9)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().fixLatE9 = v;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseFixLatKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/fix_lat 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（8）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseFixLat(const void * /*obj*/, std::span<uint8_t> out,
                                      size_t &len) noexcept
{
    if (out.size() < GnssBaseConfig::kFixCoordBlobBytes) {
        return fw::Result::kNoSpace;
    }
    encodeI64Le(appContext().gnss.readBaseConfig().fixLatE9, out);
    len = GnssBaseConfig::kFixCoordBlobBytes;
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/fix_lon（8B i64 LE）。
 * @param in: 8 字节 blob。
 * @retval kOk: 加载成功；kInvalidArg: 长度或经度范围非法。
 */
fw::Result GnssModule::loadBaseFixLon(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != GnssBaseConfig::kFixCoordBlobBytes) {
        return fw::Result::kInvalidArg;
    }
    const int64_t v = decodeI64Le(in);
    if ((v < -GnssBaseConfig::kMaxLonE9) || (v > GnssBaseConfig::kMaxLonE9)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().fixLonE9 = v;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseFixLonKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/fix_lon 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（8）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseFixLon(const void * /*obj*/, std::span<uint8_t> out,
                                      size_t &len) noexcept
{
    if (out.size() < GnssBaseConfig::kFixCoordBlobBytes) {
        return fw::Result::kNoSpace;
    }
    encodeI64Le(appContext().gnss.readBaseConfig().fixLonE9, out);
    len = GnssBaseConfig::kFixCoordBlobBytes;
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/fix_h_mm（4B i32 LE）。
 * @param in: 4 字节 blob。
 * @retval kOk: 加载成功；kInvalidArg: 长度或高程范围非法。
 */
fw::Result GnssModule::loadBaseFixHeight(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != GnssBaseConfig::kFixHeightBlobBytes) {
        return fw::Result::kInvalidArg;
    }
    const int32_t v = decodeI32Le(in);
    if ((v < GnssBaseConfig::kMinHeightMm) || (v > GnssBaseConfig::kMaxHeightMm)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().fixHeightMm = v;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseFixHMmKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/fix_h_mm 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（4）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseFixHeight(const void * /*obj*/, std::span<uint8_t> out,
                                         size_t &len) noexcept
{
    if (out.size() < GnssBaseConfig::kFixHeightBlobBytes) {
        return fw::Result::kNoSpace;
    }
    encodeI32Le(appContext().gnss.readBaseConfig().fixHeightMm, out);
    len = GnssBaseConfig::kFixHeightBlobBytes;
    return fw::Result::kOk;
}

/**
 * @brief 从 NVS 恢复 gnss.base/rtcm_out（14B blob v1）。
 * @param in: 14 字节 blob。
 * @retval kOk: 加载成功；kInvalidArg: 版本或内容非法。
 */
fw::Result GnssModule::loadBaseRtcmOut(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    GnssBaseConfig tmp{};
    if (!tmp.decodeRtcmOut(in)) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().rtcmOut = tmp.rtcmOut;
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseRtcmOutKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/rtcm_out 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（14）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseRtcmOut(const void * /*obj*/, std::span<uint8_t> out,
                                       size_t &len) noexcept
{
    return appContext().gnss.readBaseConfig().encodeRtcmOut(out, len) ? fw::Result::kOk
                                                                      : fw::Result::kNoSpace;
}

/**
 * @brief 从 NVS 恢复 gnss.base/msm。
 * @param in: 1 字节编码（4 或 7）。
 * @retval kOk: 加载成功；kInvalidArg: 非法版本。
 */
fw::Result GnssModule::loadBaseMsm(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || ((in[0U] != GnssBaseConfig::kMsm4) && (in[0U] != GnssBaseConfig::kMsm7))) {
        return fw::Result::kInvalidArg;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    gnss.ensureBaseCfg().msm = in[0U];
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << gnss_store::kBaseMsmKeyIdx);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base/msm 供 NVS 持久化。
 * @param out: 目标字节切片。
 * @param len: 输出字节数（1）。
 * @retval kOk: 写入成功；kNoSpace: 空间不足。
 */
fw::Result GnssModule::saveBaseMsm(const void * /*obj*/, std::span<uint8_t> out,
                                   size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kNoSpace;
    }
    out[0U] = appContext().gnss.readBaseConfig().msm;
    len = 1U;
    return fw::Result::kOk;
}

/**
 * @brief 处理 GnssSubVerb::kMode（工作模式切换）命令。
 * @param cmd: 指令载荷（args[0]=目标模式，args[1]=是否自动保存）。
 * @param nowMs: 当前单调时间（ms）。
 * @param result: 受理结果及 WorkMode 序号。
 */
void GnssModule::cmdMode(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept
{
    constexpr size_t kModeArgBytes = 2U;
    if ((cmd.argLen != kModeArgBytes) || !work_mode_.has_value()) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    if (cmd.args[0U] == static_cast<uint8_t>(WorkModeKind::kSmartBase)) {
        result.status = fw::Result::kNotSupported;
        return;
    }
    if (cmd.args[0U] > static_cast<uint8_t>(WorkModeKind::kBase)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    auto &wm = *work_mode_;
    const auto target = static_cast<WorkModeKind>(cmd.args[0U]);
    const bool saveOnSuccess = cmd.args[1U] != 0U;
    wm.requestMode(target, saveOnSuccess);
    pollWorkMode(nowMs);
    result.detail = std::bit_cast<int32_t>(wm.status().seq);
    result.status = fw::Result::kOk;
}

/**
 * @brief 驱动工作模式状态机并发布 gnssWorkMode 快照及 SAVECONFIG 请求。
 * @param nowMs: 当前单调时间（ms）。
 */
void GnssModule::pollWorkMode(uint32_t nowMs) noexcept
{
    if (!work_mode_.has_value()) {
        return;
    }
    auto &wm = *work_mode_;
    // 启动波特率对齐未完成前先让出控制器，避免 modeStep 干扰首次波特率对齐与提示保存。
    if (boot_baud_pending_ || boot_baud_in_flight_) {
        return;
    }
    const auto cfgSnapshot = readBaseConfig();
    const auto &rover = ensureRoverCfg();
    bool needSave = false;
    const bool changed = wm.poll(controller(), cfgSnapshot, rover.port, rover.rateHz,
                                 rover.useConfiguredPort, parser().fix().hAccCm, nowMs, needSave);
    if (wm.inFlight() && controller().state() == ::gnss::State::kMaintenance) {
        active_maintenance_ = ::gnss::Maintenance::kMode;
    }
    if (needSave) {
        requestReceiverSave();
    }
    if (changed) {
        ctx().data.gnssWorkMode.write(wm.status());
    }
    if (wm.inFlight() && controller().state() == ::gnss::State::kMaintenance) {
        pollController(nowMs);
    }
}

} // namespace app
