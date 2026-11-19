// 启动步骤表与执行器（ADR-0009）：按表执行有序启动步骤，逐项记录结果、耗时与堆快照。
// 线程：仅启动任务在 app_main 阶段调用一次，非 ISR；执行器本身不分配、不阻塞、不打印。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace app {

/** 步骤结果；kSkipped/kAbsent 表示因配置关闭或对象不存在而没有执行，仍保留在表中与记录里。 */
enum class BootResult : uint8_t { kPending = 0, kOk, kSkipped, kAbsent, kFail };

/** 失败后的处理方式：kDegrade 停止后续步骤并进入 console-only；kContinue 只记录，继续下一步。 */
enum class OnFail : uint8_t { kDegrade = 0, kContinue };

/**
 * @brief 结果的可读名（sys boot 与启动摘要共用）；纯函数，线程/ISR 安全。
 * @param[out] result: 步骤结果
 * @retval 结果对应的静态短名；未知枚举值返回 unknown
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *bootResultName(BootResult result) noexcept;

/** 故障注入原因缓冲：容纳 "fault:" 加最长步骤名。 */
inline constexpr size_t kBootReasonBytes = 40U;
/** 步骤表容量上限；生产表 16 项，用于固定 BootRecord 存储边界。 */
inline constexpr size_t kMaxBootSteps = 16U;

/** 一次堆快照，单位字节。 */
struct BootHeap {
    uint32_t freeBytes = 0U;
    uint32_t minBytes = 0U;
    uint32_t largestBytes = 0U;
};

struct BootCtx;

/** 执行器的外部依赖，由组合根注入，host 测试可替换计时/堆/降级回调。 */
struct BootHooks {
    uint32_t (*nowMs)() noexcept = nullptr;
    BootHeap (*heap)() noexcept = nullptr;
    void (*onDegrade)(BootCtx &ctx, const char *reason) noexcept = nullptr;
};

/** 步骤间共享的启动上下文；launched/stopRequired 由后续步骤推进，降级回调据此停止已启动消费者。 */
struct BootCtx {
    const BootHooks *hooks = nullptr;
    void *user = nullptr; // 组合根放 BootBuffers；执行器不解释
    uint32_t launched = 0U;
    bool stopRequired = false;       // 已进入链路/IO/收尾阶段，失败必须停稳消费者
    const char *faultStep = nullptr; // 非空且与步骤名相同 → 不执行步骤体，直接按故障失败
    const char *reason = nullptr;    // 本步失败原因；步骤函数写入，执行器读取后清空
    std::array<char, kBootReasonBytes> faultReason{};
};

/** 一步启动动作；fn 返回 kFail 时应把原因写入 ctx.reason（允许为空）。 */
struct BootStep {
    const char *name = nullptr;
    BootResult (*fn)(BootCtx &ctx) noexcept = nullptr;
    OnFail onFail = OnFail::kDegrade;
};

/** 一步的执行记录；start/took 为毫秒，堆字段为步骤结束后的内部 8-bit 堆快照。 */
struct BootRecord {
    const char *name = nullptr;
    BootResult result = BootResult::kPending;
    uint32_t startMs = 0U;
    uint32_t tookMs = 0U;
    uint32_t heapFree = 0U;
    uint32_t heapMin = 0U;
    uint32_t heapLargest = 0U;
    const char *reason = nullptr; // 指向静态字符串或静态 BootCtx::faultReason（须活过启动任务）
};

/**
 * @brief 依序执行步骤并写记录；kDegrade 失败时调用 hooks->onDegrade 后停止，剩余记录保持 kPending。 返回已写入的记录数（= 实际执行到第几步）。不分配、不阻塞、不打印；非 ISR。
 * @param steps: 按依赖顺序排列的步骤表
 * @param ctx: 跨步骤上下文；失败原因与降级状态在此传递
 * @param records: 记录输出；仅已执行的前缀被覆盖
 * @retval 实际执行并写入的记录数，不超过两张表的较小长度
 * @note 仅启动任务；本体不分配、不打印，阻塞性由注入步骤与钩子决定，非 ISR。
 */
size_t runBootSteps(std::span<const BootStep> steps, BootCtx &ctx,
                    std::span<BootRecord> records) noexcept;

} // namespace app
