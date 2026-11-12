// GNSS 公共数据类型：型号/家族枚举、能力位、定位解与卫星表（纯数据，无 IDF 依赖）。
// 坐标用整数定点避免 ESP32 双精度软浮点：纬经度单位 1e-9 度（int64，≈0.1 mm），高度 mm。
// 线程安全：纯数据，拷贝由调用方保护（main 侧经 fw::Slot 跨任务发布）。
// 冻结接口（ADR-0010 §3）。实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gnss {

/** 命令方言家族；同一家族共用命令语法与应答格式（ADR-0010 §2.2）。顺序即 NVS 存储值，只能追加。 */
enum class Family : uint8_t {
    kUnknown = 0,
    kUnicore, // UM980/UM981/UM982 等 N4 平台：ASCII 命令 + "$command,...,response: OK" 应答
    kUblox,   // ZED-F9P 等 M9/F9 代：UBX CFG-VALSET/ACK
    kComnav,  // 司南 K803 等：NovAtel 风格 ASCII（实验性，手册未核实）
};

/** 具体型号；顺序即 NVS 存储值，只能追加。 */
enum class Model : uint8_t {
    kUnknown = 0,
    kUm980,
    kUm981,
    kUm982,
    kZedF9p,
    kK803,
};

/** 型号能力位（ADR-0010 §2.3）。由方言的 caps() 按型号给出。 */
namespace cap {
inline constexpr uint32_t kRtcmIn = 1U << 0;   // 接受 RTCM3 差分输入（流动站 RTK）
inline constexpr uint32_t kBase = 1U << 1;     // 支持基准站模式（第二阶段）
inline constexpr uint32_t kHeading = 1U << 2;  // 双天线定向
inline constexpr uint32_t kIns = 1U << 3;      // 组合导航（UM981）
inline constexpr uint32_t kRate20Hz = 1U << 4; // 定位输出可达 20 Hz
} // namespace cap

/** GGA 质量指示（NMEA 0183 GGA 第 6 字段），值即协议值。 */
enum class FixQuality : uint8_t {
    kInvalid = 0,
    kSingle = 1,
    kDgps = 2,
    kPps = 3,
    kRtkFixed = 4,
    kRtkFloat = 5,
    kDeadReckoning = 6,
    kManual = 7,
    kSimulator = 8,
    kUnknown = 0xFF, // 尚未收到 GGA
};

/** UTC 时间与日期；字段为 0 表示未知（日期来自 RMC，时间来自 GGA/RMC）。 */
struct UtcTime {
    uint16_t year = 0; // 四位年
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint16_t ms = 0; // 秒内毫秒 = second*1000 + 小数部分（0..59999）
};

inline constexpr uint16_t kU16Unknown = 0xFFFFU; // 16 位字段“未知/不可用”的统一取值

/** 一次定位解（GGA + RMC + GSA 合并）。valid=false 时其余字段无意义。 */
struct Fix {
    bool valid = false; // GGA 质量非 0
    FixQuality quality = FixQuality::kUnknown;
    UtcTime utc{};
    int64_t lat_e9 = 0;                 // 纬度，1e-9 度，北正
    int64_t lon_e9 = 0;                 // 经度，1e-9 度，东正
    int32_t alt_mm = 0;                 // 海拔（MSL），mm
    int32_t geoid_mm = 0;               // 大地水准面差距，mm
    uint8_t sats_used = 0;              // GGA 参与解算卫星数
    uint16_t hdop_x100 = kU16Unknown;   // HDOP×100，0xFFFF 未知
    uint16_t pdop_x100 = kU16Unknown;   // GSA PDOP×100
    uint16_t vdop_x100 = kU16Unknown;   // GSA VDOP×100
    uint16_t diff_age_ds = kU16Unknown; // 差分龄期，0.1 s，0xFFFF 无差分
    uint16_t diff_station = kU16Unknown;
    uint32_t speed_mmps = 0;  // RMC 对地速度，mm/s
    uint16_t course_cdeg = 0; // RMC 航向，0.01 度
    uint32_t update_ms = 0;   // 最近一次 GGA 到达时的本地 now_ms（由调用方传入）
};

/** 卫星系统；值与 NMEA 4.11 GSV 的 System ID 无关，仅本工程内部使用。 */
enum class Constellation : uint8_t { kGps, kGlonass, kGalileo, kBeidou, kQzss, kNavic, kSbas, kOther, kCount };

inline constexpr size_t kMaxSats = 64; // 卫星表容量（UM980 全系统可见约 40–60）

/** 一颗可见卫星（GSV），used 来自 GSA。 */
struct SatInfo {
    Constellation system = Constellation::kOther;
    uint8_t prn = 0;
    int8_t elev_deg = 0;   // -90..90，未知 0
    uint16_t azim_deg = 0; // 0..359
    uint8_t snr_dbhz = 0;  // 0 = 未跟踪
    bool used = false;
};

/** 卫星表快照：一轮 GSV 收齐后整体替换（避免半轮数据）。 */
struct SatTable {
    std::array<SatInfo, kMaxSats> sats{};
    uint8_t count = 0;
    uint32_t update_ms = 0;
};

} // namespace gnss
