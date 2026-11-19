// GNSS App 数据通路（ADR-0011）扩展命令字与只追加追踪事件码；不修改冻结的 def/GnssDef.hpp。
// 线程安全：纯常量与数据结构定义。ISR 安全：是。
#pragma once

#include <cstdint>

namespace app {

/**
 * 投递给 GnssModule 的 扩展命令字（接续 def/GnssDef.hpp 中 GnssVerb 0..3，从 4 起编）。
 * args[0..7] 按 GnssOutConfig::kBlobBytes 布局编码。
 */
enum class GnssSubVerb : uint8_t {
    kSetOut = 4,
    kReadback = 5, // 控制台 gnss config：读回模块配置（ADR-0012 §2.7），无参数
    // 6..8 已被 GnssMaintenanceVerb（kSave/kFactoryReset/kBaud）占用，见 GnssModuleCmds.cpp 的 static_assert。
    kMode = 9,         // 工作模式切换：args[0] = WorkModeKind, args[1] = saveOnSuccess(0/1)
    kSetBase = 10,     // 更新并持久化 gnss.base 字段子包：args[0] = BasePatchKind
    kPhonePpp = 11,    // 手机 0x33 Survey：args[0] = 0/1。
    kPhoneSatCfg = 12, // 手机 0x06 Survey：args[0..5] = {cutoffDeg, gps, glo, bds, gal, sbas}。
};

/**
 * gnss.base 更新命令子包种类（fw::kCommandArgBytes=32，完整配置 41 B 无法单包容纳，按变更块拆分）。
 */
enum class BasePatchKind : uint8_t {
    kMode = 0,   // [0]=0, [1]=WorkModeKind (2 B)
    kSurvey = 1, // [0]=1, [1]=BaseType(kSurvey), [2..3]=svyS, [4..5]=svyAccCm, [6..7]=svyToS (8 B)
    kFixed =
        2, // [0]=2, [1]=BaseType(kFixed), [2..9]=fixLatE9, [10..17]=fixLonE9, [18..21]=fixHeightMm (22 B)
    kRtcm = 3, // [0]=3, [1]=msm, [2..13]=rtcmOut[12] (14 B)
};

/** GNSS 工作模式（NVS gnss/mode）。 */
enum class WorkModeKind : uint8_t {
    kRover = 0,
    kBase = 1,
    kSmartBase = 2,
};

/** 基站坐标来源（NVS gnss/base_type）。 */
enum class BaseType : uint8_t {
    kSurvey = 0,
    kFixed = 1,
};

/** 上层工作模式状态机状态（仅 main/modules/gnss/GnssWorkMode 维护）。 */
enum class WorkModeState : uint8_t {
    kRover = 0,
    kBaseConfig = 1,
    kSurveyWait = 2,
    kSurvey = 3,
    kBaseTx = 4,
    kFallback = 5,
};

/** 上层工作模式最近错误原因。 */
enum class WorkModeErr : uint8_t {
    kNone = 0,
    kUnsupported = 1,
    kNack = 2,
    kTimeout = 3,
    kSurveyTimeout = 4,
    kInvalidConfig = 5,
};

inline constexpr uint16_t kWorkModeHAccUnknown = 0xFFFFU;

/** 跨任务发布的工作模式状态快照（DataCenter::gnssWorkMode）。 */
struct GnssWorkModeStatus {
    WorkModeKind mode = WorkModeKind::kRover;
    BaseType baseType = BaseType::kSurvey;
    WorkModeState state = WorkModeState::kRover;
    WorkModeErr lastErr = WorkModeErr::kNone;
    uint16_t elapsedS = 0;
    uint16_t hAccCm = kWorkModeHAccUnknown;
    uint32_t seq = 0;
    bool active = false;
};

/**
 * TraceCat::kGnss 的 追加追踪事件码（接续 def/GnssDef.hpp 中 GnssTrace 0..9，从 10 起编，不重排旧码）。
 */
enum class GnssSubTrace : uint8_t {
    kAppMask = 10,  // val0 = 订阅掩码，val1 = 变化来源（0=App订阅/查询，1=会话断开归零）
    kAppDrop = 11,  // val0 = 丢弃源（0=App环超长，1=查询超256B，2=最新帧缓存超限），val1 = typeIdx
    kOutCfg = 12,   // val0 = 启用掩码，val1 = 状态（0=加载/生效，1=回退默认）
    kBaudFail = 13, // val0 = 目标波特率，val1 = gnss::MaintenanceResult
};

} // namespace app
