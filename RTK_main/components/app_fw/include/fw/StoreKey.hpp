// 存储键描述：每个模块用 `inline constexpr StoreKey kKeys[]` 登记自己的持久化项。
// 同一张表以后驱动 NVS 持久化、控制台 cfg get/set、App 配置与导入导出（借鉴 SparkFun rtkSettingsEntries[]，
// 但静态、类型化、按模块分表）。本轮 storage 模块只用 ns/key/type/max_size/binding，其余元数据先登记、后使用。
// 纯数据定义，无状态；表本身应为 constexpr，校验在编译期完成。
// 冻结接口（ADR-0007 §3.3、§3.6-2）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fw/Result.hpp"

namespace fw {

inline constexpr size_t kStoreNameMax = 15; // NVS 命名空间与键名上限（不含结尾 0）

/** 持久化项的值类型；决定 max_size 的合法取值与 NVS 存取方式。 */
enum class StoreType : uint8_t {
    kU8 = 0,
    kU16,
    kU32,
    kI32,
    kStr,  // 文本，不含结尾 0 的长度 ≤ max_size
    kBlob, // 二进制，长度 ≤ max_size
};

namespace store_flag {
inline constexpr uint8_t kConsole = 0x01;        // 控制台 cfg 可见
inline constexpr uint8_t kExport = 0x02;         // 参与导入/导出
inline constexpr uint8_t kReadOnly = 0x04;       // 外部（控制台/App）只读
inline constexpr uint8_t kRebootRequired = 0x08; // 修改后需重启生效
} // namespace store_flag

inline constexpr uint32_t kAllProducts = 0xFFFFFFFFU;

/**
 * 绑定到所属对象的序列化函数（在所属模块的执行器任务中调用）。
 * load：storage 从后端读到 in 后调用，所属对象校验并接收；数据非法返回 kInvalidArg（storage
 * 只记录错误，不改所属对象）。 save：storage 写盘前调用，所属对象把当前值序列化到 out，len 填实际长度（≤ out.size()）。
 */
struct StoreBinding {
    void *obj = nullptr;
    Result (*load)(void *obj, std::span<const uint8_t> in) noexcept = nullptr;
    Result (*save)(const void *obj, std::span<uint8_t> out, size_t &len) noexcept = nullptr;
};

/** 一条持久化项的描述；binding 指向所属对象及其 load/save 函数（可为空表示暂未绑定）。 */
struct StoreKey {
    std::string_view ns;  // NVS 命名空间，按模块
    std::string_view key; // NVS 键名
    StoreType type = StoreType::kBlob;
    uint16_t max_size = 0; // kStr/kBlob 的最大字节数；数值类型填 sizeof
    uint8_t flags = 0;     // store_flag 位或
    int32_t min = 0;       // 数值类型的取值范围（含）；kStr/kBlob 忽略
    int32_t max = 0;
    uint32_t product_mask = kAllProducts;
    std::string_view help; // 控制台帮助（可空）
    StoreBinding binding{};
};

/** 编译期校验：命名长度、max_size 与类型一致、范围有序。用法：static_assert(fw::valid(kKeys[0]))。 */
[[nodiscard]] constexpr bool valid(const StoreKey &k) noexcept
{
    if (k.ns.empty() || (k.ns.size() > kStoreNameMax) || k.key.empty() ||
        (k.key.size() > kStoreNameMax)) {
        return false;
    }
    switch (k.type) {
    case StoreType::kU8:
        return (k.max_size == 1U) && (k.min <= k.max);
    case StoreType::kU16:
        return (k.max_size == 2U) && (k.min <= k.max);
    case StoreType::kU32:
    case StoreType::kI32:
        return (k.max_size == 4U) && (k.min <= k.max);
    case StoreType::kStr:
    case StoreType::kBlob:
        return k.max_size > 0U;
    default:
        return false;
    }
}

/** 同一张表内 (ns, key) 不重复。用法：static_assert(fw::unique_keys(kKeys))。 */
[[nodiscard]] constexpr bool unique_keys(std::span<const StoreKey> keys) noexcept
{
    for (size_t i = 0; i < keys.size(); ++i) {
        for (size_t j = i + 1U; j < keys.size(); ++j) {
            if ((keys[i].ns == keys[j].ns) && (keys[i].key == keys[j].key)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace fw
