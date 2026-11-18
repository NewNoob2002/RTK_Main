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
#include "modules/storage/StorageModule.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

/** @brief gnss.base 单个持久化键的编解码分类。 */
enum class BaseFieldKind : uint8_t {
    kWorkMode,
    kBaseType,
    kU16,
    kI64Blob,
    kI32Blob,
    kRtcmOutBlob,
    kMsm,
};

/** @brief gnss.base 字段描述表项（关联 NVS 槽位、编解码类别、范围与成员指针）。 */
struct BaseFieldDesc {
    size_t keyIdx = 0U;
    BaseFieldKind kind = BaseFieldKind::kWorkMode;
    int64_t minVal = 0;
    int64_t maxVal = 0;
    WorkModeKind GnssBaseConfig::*modePtr = nullptr;
    BaseType GnssBaseConfig::*typePtr = nullptr;
    uint16_t GnssBaseConfig::*u16Ptr = nullptr;
    int64_t GnssBaseConfig::*i64Ptr = nullptr;
    int32_t GnssBaseConfig::*i32Ptr = nullptr;
    uint8_t GnssBaseConfig::*u8Ptr = nullptr;
};

constexpr std::array<BaseFieldDesc, gnss_store::kBaseKeys.size()> kBaseFieldDescs{{
    {gnss_store::kBaseModeKeyIdx, BaseFieldKind::kWorkMode, 0,
     static_cast<int64_t>(WorkModeKind::kBase), &GnssBaseConfig::mode},
    {gnss_store::kBaseTypeKeyIdx, BaseFieldKind::kBaseType, 0,
     static_cast<int64_t>(BaseType::kFixed), nullptr, &GnssBaseConfig::baseType},
    {gnss_store::kBaseSvySKeyIdx, BaseFieldKind::kU16, GnssBaseConfig::kMinSvyS,
     GnssBaseConfig::kMaxSvyS, nullptr, nullptr, &GnssBaseConfig::svyS},
    {gnss_store::kBaseSvyAccCmKeyIdx, BaseFieldKind::kU16, GnssBaseConfig::kMinSvyAccCm,
     GnssBaseConfig::kMaxSvyAccCm, nullptr, nullptr, &GnssBaseConfig::svyAccCm},
    {gnss_store::kBaseSvyToSKeyIdx, BaseFieldKind::kU16, GnssBaseConfig::kMinSvyToS,
     GnssBaseConfig::kMaxSvyToS, nullptr, nullptr, &GnssBaseConfig::svyToS},
    {gnss_store::kBaseFixLatKeyIdx, BaseFieldKind::kI64Blob, -GnssBaseConfig::kMaxLatE9,
     GnssBaseConfig::kMaxLatE9, nullptr, nullptr, nullptr, &GnssBaseConfig::fixLatE9},
    {gnss_store::kBaseFixLonKeyIdx, BaseFieldKind::kI64Blob, -GnssBaseConfig::kMaxLonE9,
     GnssBaseConfig::kMaxLonE9, nullptr, nullptr, nullptr, &GnssBaseConfig::fixLonE9},
    {gnss_store::kBaseFixHMmKeyIdx, BaseFieldKind::kI32Blob, GnssBaseConfig::kMinHeightMm,
     GnssBaseConfig::kMaxHeightMm, nullptr, nullptr, nullptr, nullptr,
     &GnssBaseConfig::fixHeightMm},
    {gnss_store::kBaseRtcmOutKeyIdx, BaseFieldKind::kRtcmOutBlob, 0, 0},
    {gnss_store::kBaseMsmKeyIdx, BaseFieldKind::kMsm, GnssBaseConfig::kMsm4, GnssBaseConfig::kMsm7,
     nullptr, nullptr, nullptr, nullptr, nullptr, &GnssBaseConfig::msm},
}};

static_assert(kBaseFieldDescs[gnss_store::kBaseModeKeyIdx].keyIdx == gnss_store::kBaseModeKeyIdx);
static_assert(kBaseFieldDescs[gnss_store::kBaseMsmKeyIdx].keyIdx == gnss_store::kBaseMsmKeyIdx);

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

/**
 * @brief 解码 gnss.base 组 blob 类（fix_lat / fix_lon / fix_h_mm / rtcm_out）键值。
 * @param desc: 字段描述。
 * @param in: 输入字节。
 * @param[out] parsed: 写入解码字段后的临时配置。
 * @retval kOk: 合法；kInvalidArg: 长度或取值非法。
 */
fw::Result decodeBaseBlobField(const BaseFieldDesc &desc, std::span<const uint8_t> in,
                               GnssBaseConfig &parsed) noexcept
{
    if (desc.kind == BaseFieldKind::kI64Blob) {
        if (in.size() != GnssBaseConfig::kFixCoordBlobBytes) {
            return fw::Result::kInvalidArg;
        }
        const int64_t v = decodeI64Le(in);
        if ((v < desc.minVal) || (v > desc.maxVal)) {
            return fw::Result::kInvalidArg;
        }
        parsed.*(desc.i64Ptr) = v;
        return fw::Result::kOk;
    }
    if (desc.kind == BaseFieldKind::kI32Blob) {
        if (in.size() != GnssBaseConfig::kFixHeightBlobBytes) {
            return fw::Result::kInvalidArg;
        }
        const int32_t v = decodeI32Le(in);
        if ((v < desc.minVal) || (v > desc.maxVal)) {
            return fw::Result::kInvalidArg;
        }
        parsed.*(desc.i32Ptr) = v;
        return fw::Result::kOk;
    }
    return parsed.decodeRtcmOut(in) ? fw::Result::kOk : fw::Result::kInvalidArg;
}

/**
 * @brief 按描述表项解码单个 NVS 输入值并校验范围到临时结构。
 * @param desc: 字段描述。
 * @param in: 输入字节。
 * @param[out] parsed: 写入解码字段后的临时配置。
 * @retval kOk: 合法；kInvalidArg: 长度或范围非法。
 */
fw::Result decodeBaseField(const BaseFieldDesc &desc, std::span<const uint8_t> in,
                           GnssBaseConfig &parsed) noexcept
{
    switch (desc.kind) {
    case BaseFieldKind::kWorkMode:
    case BaseFieldKind::kBaseType: {
        if (in.empty() || (static_cast<int64_t>(in[0U]) > desc.maxVal)) {
            return fw::Result::kInvalidArg;
        }
        if (desc.kind == BaseFieldKind::kWorkMode) {
            parsed.*(desc.modePtr) = static_cast<WorkModeKind>(in[0U]);
        } else {
            parsed.*(desc.typePtr) = static_cast<BaseType>(in[0U]);
        }
        return fw::Result::kOk;
    }
    case BaseFieldKind::kU16: {
        if (in.size() < sizeof(uint16_t)) {
            return fw::Result::kInvalidArg;
        }
        const uint16_t v = readU16Le(in);
        const auto vSigned = static_cast<int64_t>(v);
        if ((vSigned < desc.minVal) || (vSigned > desc.maxVal)) {
            return fw::Result::kInvalidArg;
        }
        parsed.*(desc.u16Ptr) = v;
        return fw::Result::kOk;
    }
    case BaseFieldKind::kI64Blob:
    case BaseFieldKind::kI32Blob:
    case BaseFieldKind::kRtcmOutBlob:
        return decodeBaseBlobField(desc, in, parsed);
    case BaseFieldKind::kMsm:
        if (in.empty() ||
            ((in[0U] != GnssBaseConfig::kMsm4) && (in[0U] != GnssBaseConfig::kMsm7))) {
            return fw::Result::kInvalidArg;
        }
        parsed.*(desc.u8Ptr) = in[0U];
        return fw::Result::kOk;
    default:
        return fw::Result::kInvalidArg;
    }
}

/**
 * @brief 将已验证的单个字段从临时对象写入模块配置。
 * @param desc: 字段描述。
 * @param src: 已解码临时配置。
 * @param[out] dst: 模块持有的目标配置。
 */
void assignBaseField(const BaseFieldDesc &desc, const GnssBaseConfig &src,
                     GnssBaseConfig &dst) noexcept
{
    switch (desc.kind) {
    case BaseFieldKind::kWorkMode:
        dst.*(desc.modePtr) = src.*(desc.modePtr);
        break;
    case BaseFieldKind::kBaseType:
        dst.*(desc.typePtr) = src.*(desc.typePtr);
        break;
    case BaseFieldKind::kU16:
        dst.*(desc.u16Ptr) = src.*(desc.u16Ptr);
        break;
    case BaseFieldKind::kI64Blob:
        dst.*(desc.i64Ptr) = src.*(desc.i64Ptr);
        break;
    case BaseFieldKind::kI32Blob:
        dst.*(desc.i32Ptr) = src.*(desc.i32Ptr);
        break;
    case BaseFieldKind::kRtcmOutBlob:
        dst.rtcmOut = src.rtcmOut;
        break;
    case BaseFieldKind::kMsm:
        dst.*(desc.u8Ptr) = src.*(desc.u8Ptr);
        break;
    default:
        break;
    }
}

/**
 * @brief 按描述表项将 GnssBaseConfig 字段编码到 NVS 输出缓冲。
 * @param desc: 字段描述。
 * @param cfg: 当前配置副本。
 * @param[out] out: 输出缓冲区。
 * @param[out] len: 写入字节数。
 * @retval kOk: 写入成功；kNoSpace: 输出缓冲不足；kInvalidArg: 未知字段类型。
 */
fw::Result encodeBaseField(const BaseFieldDesc &desc, const GnssBaseConfig &cfg,
                           std::span<uint8_t> out, size_t &len) noexcept
{
    switch (desc.kind) {
    case BaseFieldKind::kWorkMode:
    case BaseFieldKind::kBaseType:
    case BaseFieldKind::kMsm:
        if (out.empty()) {
            return fw::Result::kNoSpace;
        }
        if (desc.kind == BaseFieldKind::kWorkMode) {
            out[0U] = static_cast<uint8_t>(cfg.*(desc.modePtr));
        } else if (desc.kind == BaseFieldKind::kBaseType) {
            out[0U] = static_cast<uint8_t>(cfg.*(desc.typePtr));
        } else {
            out[0U] = cfg.*(desc.u8Ptr);
        }
        len = 1U;
        return fw::Result::kOk;
    case BaseFieldKind::kU16:
        if (out.size() < sizeof(uint16_t)) {
            return fw::Result::kNoSpace;
        }
        writeU16Le(cfg.*(desc.u16Ptr), out);
        len = sizeof(uint16_t);
        return fw::Result::kOk;
    case BaseFieldKind::kI64Blob:
        if (out.size() < GnssBaseConfig::kFixCoordBlobBytes) {
            return fw::Result::kNoSpace;
        }
        encodeI64Le(cfg.*(desc.i64Ptr), out);
        len = GnssBaseConfig::kFixCoordBlobBytes;
        return fw::Result::kOk;
    case BaseFieldKind::kI32Blob:
        if (out.size() < GnssBaseConfig::kFixHeightBlobBytes) {
            return fw::Result::kNoSpace;
        }
        encodeI32Le(cfg.*(desc.i32Ptr), out);
        len = GnssBaseConfig::kFixHeightBlobBytes;
        return fw::Result::kOk;
    case BaseFieldKind::kRtcmOutBlob:
        return cfg.encodeRtcmOut(out, len) ? fw::Result::kOk : fw::Result::kNoSpace;
    default:
        return fw::Result::kInvalidArg;
    }
}

/**
 * @brief 解析并校验 kSurvey 子包。
 * @param args: 子包载荷。
 * @param[out] cfg: 待更新配置。
 * @retval true: 参数合法；false: 非法。
 */
bool decodeBaseSurveyPatch(std::span<const uint8_t> args, GnssBaseConfig &cfg) noexcept
{
    constexpr size_t kSurveyBytes = 8U;
    if ((args.size() != kSurveyBytes) || (args[1U] != static_cast<uint8_t>(BaseType::kSurvey))) {
        return false;
    }
    const uint16_t svyS = readU16Le(args.subspan(2U, 2U));
    const uint16_t accCm = readU16Le(args.subspan(4U, 2U));
    const uint16_t toS = readU16Le(args.subspan(6U, 2U));
    if ((svyS < GnssBaseConfig::kMinSvyS) || (svyS > GnssBaseConfig::kMaxSvyS) ||
        (accCm < GnssBaseConfig::kMinSvyAccCm) || (accCm > GnssBaseConfig::kMaxSvyAccCm) ||
        (toS < (svyS + GnssBaseConfig::kMinSvyToMarginS)) || (toS > GnssBaseConfig::kMaxSvyToS)) {
        return false;
    }
    cfg.baseType = BaseType::kSurvey;
    cfg.svyS = svyS;
    cfg.svyAccCm = accCm;
    cfg.svyToS = toS;
    return true;
}

/**
 * @brief 解析并校验 kFixed 子包。
 * @param args: 子包载荷。
 * @param[out] cfg: 待更新配置。
 * @retval true: 参数合法；false: 非法。
 */
bool decodeBaseFixedPatch(std::span<const uint8_t> args, GnssBaseConfig &cfg) noexcept
{
    constexpr size_t kFixedBytes = 22U;
    if ((args.size() != kFixedBytes) || (args[1U] != static_cast<uint8_t>(BaseType::kFixed))) {
        return false;
    }
    const int64_t lat = decodeI64Le(args.subspan(2U, GnssBaseConfig::kFixCoordBlobBytes));
    const int64_t lon = decodeI64Le(args.subspan(10U, GnssBaseConfig::kFixCoordBlobBytes));
    const int32_t hMm = decodeI32Le(args.subspan(18U, GnssBaseConfig::kFixHeightBlobBytes));
    if ((lat < -GnssBaseConfig::kMaxLatE9) || (lat > GnssBaseConfig::kMaxLatE9) ||
        (lon < -GnssBaseConfig::kMaxLonE9) || (lon > GnssBaseConfig::kMaxLonE9) ||
        (hMm < GnssBaseConfig::kMinHeightMm) || (hMm > GnssBaseConfig::kMaxHeightMm)) {
        return false;
    }
    cfg.baseType = BaseType::kFixed;
    cfg.fixLatE9 = lat;
    cfg.fixLonE9 = lon;
    cfg.fixHeightMm = hMm;
    return true;
}

/**
 * @brief 解析并校验 kRtcm 子包。
 * @param args: 子包载荷。
 * @param[out] cfg: 待更新配置。
 * @retval true: 参数合法；false: 非法。
 */
bool decodeBaseRtcmPatch(std::span<const uint8_t> args, GnssBaseConfig &cfg) noexcept
{
    constexpr size_t kRtcmBytes = 2U + ::gnss::kBaseRtcmCount;
    if ((args.size() != kRtcmBytes) ||
        ((args[1U] != GnssBaseConfig::kMsm4) && (args[1U] != GnssBaseConfig::kMsm7))) {
        return false;
    }
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        if (!GnssBaseConfig::isValidRateCode(static_cast<int8_t>(args[2U + i]))) {
            return false;
        }
    }
    cfg.msm = args[1U];
    for (size_t i = 0U; i < ::gnss::kBaseRtcmCount; ++i) {
        cfg.rtcmOut[i] = static_cast<int8_t>(args[2U + i]);
    }
    return true;
}

/**
 * @brief 解析并校验 kSetBase 子包参数到 cfg。
 * @param cmd: 待解码命令（verb == kSetBase）。
 * @param[in,out] cfg: 待更新的配置副本。
 * @retval true: 参数长度与范围合法；false: 非法参数。
 */
bool decodeBasePatch(const fw::Command &cmd, GnssBaseConfig &cfg) noexcept
{
    if (cmd.argLen == 0U) {
        return false;
    }
    const auto args = std::span{cmd.args}.first(cmd.argLen);
    switch (static_cast<BasePatchKind>(args[0U])) {
    case BasePatchKind::kMode:
        if ((args.size() != 2U) || (args[1U] > static_cast<uint8_t>(WorkModeKind::kBase))) {
            return false;
        }
        cfg.mode = static_cast<WorkModeKind>(args[1U]);
        return true;
    case BasePatchKind::kSurvey:
        return decodeBaseSurveyPatch(args, cfg);
    case BasePatchKind::kFixed:
        return decodeBaseFixedPatch(args, cfg);
    case BasePatchKind::kRtcm:
        return decodeBaseRtcmPatch(args, cfg);
    default:
        return false;
    }
}

/**
 * @brief 按子包种类将对应的 gnss.base 键切片写入 NVS。
 * @param kind: 子包种类。
 * @param storage: 存储模块引用。
 * @retval kOk: 持久化成功；其他: 底层 NVS 写入失败。
 */
fw::Result persistBasePatchSlices(BasePatchKind kind, StorageModule &storage) noexcept
{
    constexpr size_t kModeKeyCount = 1U;
    constexpr size_t kSurveyKeyCount = 4U;
    constexpr size_t kFixedCoordKeyCount = 3U;
    constexpr size_t kRtcmKeyCount = 2U;
    const auto allKeys = std::span{gnss_store::kBaseKeys};
    switch (kind) {
    case BasePatchKind::kMode:
        return storage.saveKeys(allKeys.subspan(gnss_store::kBaseModeKeyIdx, kModeKeyCount));
    case BasePatchKind::kSurvey:
        return storage.saveKeys(allKeys.subspan(gnss_store::kBaseTypeKeyIdx, kSurveyKeyCount));
    case BasePatchKind::kFixed: {
        const fw::Result first =
            storage.saveKeys(allKeys.subspan(gnss_store::kBaseTypeKeyIdx, kModeKeyCount));
        if (!fw::isOk(first)) {
            return first;
        }
        return storage.saveKeys(
            allKeys.subspan(gnss_store::kBaseFixLatKeyIdx, kFixedCoordKeyCount));
    }
    case BasePatchKind::kRtcm:
        return storage.saveKeys(allKeys.subspan(gnss_store::kBaseRtcmOutKeyIdx, kRtcmKeyCount));
    default:
        return fw::Result::kInvalidArg;
    }
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
 * @brief 从 NVS 恢复 gnss.base 组指定槽位的键值。
 * @param slot: gnss_store::kBaseKeys 下标（0..9）。
 * @param in: NVS 输入字节。
 * @retval kOk: 加载成功；kInvalidArg: 槽位或取值非法。
 */
fw::Result GnssModule::loadBaseAt(size_t slot, std::span<const uint8_t> in) noexcept
{
    if (slot >= kBaseFieldDescs.size()) {
        return fw::Result::kInvalidArg;
    }
    const BaseFieldDesc &desc = kBaseFieldDescs[slot];
    GnssBaseConfig parsed{};
    const fw::Result status = decodeBaseField(desc, in, parsed);
    if (!fw::isOk(status)) {
        return status;
    }
    auto &gnss = appContext().gnss;
    const OptionalMuxGuard guard(gnss.ctx().data.mux, gnss.initialized_);
    assignBaseField(desc, parsed, gnss.ensureBaseCfg());
    gnss.base_loaded_mask_ |= static_cast<uint16_t>(1U << slot);
    return fw::Result::kOk;
}

/**
 * @brief 编码 gnss.base 组指定槽位供 NVS 持久化。
 * @param slot: gnss_store::kBaseKeys 下标（0..9）。
 * @param[out] out: 目标字节切片。
 * @param[out] len: 写入字节数。
 * @retval kOk: 写入成功；kNoSpace: 空间不足；kInvalidArg: 槽位非法。
 */
fw::Result GnssModule::saveBaseAt(size_t slot, std::span<uint8_t> out, size_t &len) noexcept
{
    if (slot >= kBaseFieldDescs.size()) {
        return fw::Result::kInvalidArg;
    }
    const GnssBaseConfig cfg = appContext().gnss.readBaseConfig();
    return encodeBaseField(kBaseFieldDescs[slot], cfg, out, len);
}

/**
 * @brief 在 kIo 任务内原子更新 gnss.base 字段并写入 NVS（失败回滚内存）。
 * @param cmd: kSetBase 子包指令。
 * @param[out] result: 返回 kOk / kInvalidArg / 底层存储错误。
 */
void GnssModule::cmdSetBase(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    GnssBaseConfig next = readBaseConfig();
    if (!decodeBasePatch(cmd, next)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const GnssBaseConfig prev = readBaseConfig();
    writeBaseConfig(next);
    const auto kind = static_cast<BasePatchKind>(cmd.args[0U]);
    const fw::Result saved = persistBasePatchSlices(kind, ctx().storage);
    if (!fw::isOk(saved)) {
        writeBaseConfig(prev);
    }
    result.status = saved;
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
