// OLED 纯定点动画引擎与按键手势消歧（OL2，无浮点、无堆、无 IDF/FreeRTOS 依赖）。
// 所有函数与对象由 OLED 任务或主机测试单线程串行调用；不阻塞、不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <cstdint>

#include "def/OledDef.hpp"

namespace app::oled {

inline constexpr uint16_t kEaseScale = 1024U;          // Q10 缓动进度满值（0..1024）
inline constexpr uint32_t kDoubleClickWindowMs = 300U; // ms，短按/双击消歧窗口上限（<= 300 ms）
inline constexpr uint32_t kPageTransitionMs = 420U;    // ms，页面切换/展开收回动画时长
inline constexpr uint32_t kContentStaggerMs = 40U;     // ms，框体先动，随后内容逐行错峰上移
inline constexpr int16_t kTransitionContentLiftPx = 6; // px，进入时内容的初始垂直偏移
inline constexpr uint32_t kNumberRollMs = 180U;        // ms，数字滚动切换时长
inline constexpr uint32_t kSquishDurationMs = 120U;    // ms，双击确认选择框挤压/回弹总时长
inline constexpr uint32_t kSquishPressMs = 45U;        // ms，挤压向内收缩时长
inline constexpr int16_t kSquishMaxPx = 2;             // px，选择框向内挤压像素
inline constexpr uint32_t kHoldCollapseMs = 150U;      // ms，长按未满松开时浮层收起时长
inline constexpr uint32_t kChargeAnimPeriodMs = 500U;  // ms，充电待机流动动画周期（<= 2 fps）
inline constexpr uint32_t kStandbyRingDecayMs = 240U;  // ms，待机松开后进度环满程回落时长
inline constexpr uint32_t kAnimFramePeriodMs = 33U;    // ms，动画期目标帧周期（~30 fps）
inline constexpr uint32_t kIdlePollPeriodMs = 200U;    // ms，静止期轮询周期（5 Hz，0 写 I2C）

/** 短按/双击消歧输出事件（长按由 PowerKeySm 独立处理，不经本消歧器）。 */
enum class ClickEvent : uint8_t {
    kNone = 0,
    kSingle,
    kDouble,
};

/**
 * 单键短按/双击消歧状态机（按键方案 A，按消抖短按生产时间戳消歧）。
 * 收到首次短按后记录生产时刻 firstPressMs_，后续短按按事件生产间隔 < kDoubleClickWindowMs 判为 kDouble，
 * >= kDoubleClickWindowMs 先确认前次 kSingle 并开启新窗口；空闲轮询按绝对截止 firstPressMs_ + 300 ms 确认 kSingle。
 * 支持 uint32_t 单调时钟自然回绕，不受 OLED 刷屏消费延迟漂移影响（R2OL2-03）。
 */
class ClickDetector final {
public:
    constexpr ClickDetector() noexcept = default;

    /**
     * @brief 重置内部等待状态（如进入关机或充电待机时丢弃悬而未决的单击）
     * @note 由持有者串行调用，不阻塞。
     */
    void reset() noexcept;

    /**
     * @brief 按生产顺序喂入一次消抖短按事件的生产时间戳
     * @param pressMs: 该消抖短按事件在 PowerKeySm 中产生的单调时刻（ms）
     * @retval ClickEvent::kNone 首次按压进入待判双击窗口
     * @retval ClickEvent::kDouble 与前次按压生产间隔 < 300 ms，确认双击
     * @retval ClickEvent::kSingle 与前次按压生产间隔 >= 300 ms，确认前次单击并以本次开启新窗口
     * @note 由持有者按事件生产顺序串行调用，不阻塞、不分配堆。
     */
    [[nodiscard]] ClickEvent onPress(uint32_t pressMs) noexcept;

    /**
     * @brief 按当前单调时间检查待判单击是否已到达 300 ms 绝对截止
     * @param nowMs: 当前单调时间（ms）
     * @retval ClickEvent::kSingle 绝对窗口已到期，确认单击
     * @retval ClickEvent::kNone 无待判按压或尚未到期
     * @note 应在同轮消费完所有队列内 onPress() 后调用，避免消费延迟误拆有效双击。
     */
    [[nodiscard]] ClickEvent pollTimeout(uint32_t nowMs) noexcept;

    /**
     * @brief 查询是否正在等待双击窗口到期
     * @retval true 存在待判定的首次短按
     * @retval false 空闲
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] bool pending() const noexcept
    {
        return pending_;
    }

    /**
     * @brief 读取当前待判双击窗口的绝对截止时刻（firstPressMs_ + kDoubleClickWindowMs）
     * @retval 绝对截止单调时刻（ms）；仅在 pending() 为 true 时有效
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] uint32_t deadlineMs() const noexcept
    {
        return firstPressMs_ + kDoubleClickWindowMs;
    }

    /**
     * @brief 计算距离双击窗口绝对截止的剩余毫秒数（用于精确唤醒）
     * @param nowMs: 当前单调时间（ms）
     * @retval 剩余等待时间（ms）；无待定短按或已到期时返回 0
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] uint32_t remainingMs(uint32_t nowMs) const noexcept;

private:
    bool pending_ = false;
    uint32_t firstPressMs_ = 0U;
};

/**
 * @brief 定点三次缓出函数 ease-out cubic：1 - (1 - t)^3（纯整数 Q10，严格单调非递减）
 * @param elapsedMs: 已流逝时间（ms）
 * @param durationMs: 总时长（ms），0 时直接返回 kEaseScale
 * @retval 0..kEaseScale（1024）范围内的进度值；elapsedMs==0 返回 0，>=durationMs 返回 1024
 * @note 纯函数，无浮点、不溢出，线程/ISR 安全。
 */
[[nodiscard]] uint16_t easeOutCubic(uint32_t elapsedMs, uint32_t durationMs) noexcept;

/**
 * @brief 按 Q10 进度在起止整数坐标之间做定点线性插值
 * @param from: 起始值
 * @param to: 目标值
 * @param progressQ10: Q10 进度（0..kEaseScale）
 * @retval 插值结果；progressQ10==0 精确返回 from，>=kEaseScale 精确返回 to
 * @note 纯函数，无浮点，线程/ISR 安全。
 */
[[nodiscard]] int16_t lerpFixed(int16_t from, int16_t to, uint16_t progressQ10) noexcept;

/**
 * Q8 定点阻尼弹簧（用于表盘选中框丝滑跟随，无浮点、无堆）。
 * 采用 <=16 ms 子步半隐式欧拉积分与总 dt 64 ms 钳制，稳定收敛并自动吸附至目标终值。
 */
class FixedSpring final {
public:
    static constexpr int32_t kScale = 256;            // Q8：1 px = 256
    static constexpr int32_t kDefaultStiffness = 220; // 刚度 k（表盘入口跟随）
    static constexpr int32_t kDefaultDamping = 26;    // 阻尼 c（阻尼比约 0.88）
    static constexpr int32_t kRowStiffness = 2800;    // 刚度 k（子页行间移动 165 ms <= 180 ms）
    static constexpr int32_t kRowDamping = 94;        // 阻尼 c（阻尼比约 0.89，快速无过冲）
    static constexpr uint32_t kMaxStepMs = 64U;       // ms，单次 step 最大积分跨度
    static constexpr uint32_t kSubStepMs = 8U;        // ms，稳定子步上限

    constexpr FixedSpring() noexcept = default;

    /**
     * @brief 立即吸附到指定整像素位置并清零速度
     * @param posPx: 目标整像素位置
     * @note 纯内存操作，不阻塞。
     */
    void snap(int16_t posPx) noexcept;

    /**
     * @brief 设定新目标位置，保留当前位置与速度以实现平滑连续重定向
     * @param targetPx: 新目标整像素位置
     * @note 纯内存操作，不阻塞。
     */
    void retarget(int16_t targetPx) noexcept;

    /**
     * @brief 按时间步进推进弹簧物理状态并在误差与速度低于阈值时吸附静止
     * @param dtMs: 距上次步进经过的毫秒数（0 忽略，>64 ms 钳制到 64 ms）
     * @param stiffness: 弹簧刚度参数（默认 kDefaultStiffness）
     * @param damping: 阻尼系数（默认 kDefaultDamping）
     * @note 纯整数定点计算，无浮点、不阻塞。
     */
    void step(uint32_t dtMs, int32_t stiffness = kDefaultStiffness,
              int32_t damping = kDefaultDamping) noexcept;

    /**
     * @brief 读取当前四舍五入后的整像素位置
     * @retval 当前整像素坐标
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] int16_t position() const noexcept;

    /**
     * @brief 读取当前目标整像素位置
     * @retval 目标整像素坐标
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] int16_t target() const noexcept;

    /**
     * @brief 查询弹簧是否已完全静止并吸附到目标值
     * @retval true 已静止（无需继续 30 fps 刷新）
     * @retval false 仍在运动中
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] bool settled() const noexcept
    {
        return !moving_;
    }

private:
    int32_t posQ8_ = 0;
    int32_t velQ8_ = 0;
    int32_t targetQ8_ = 0;
    bool moving_ = false;
};

/**
 * 纯定点数字滚动动画状态（参考 GNSS_Panel numberFlow）：数值变化时在 180 ms 内竖向滚动切入。
 */
class NumberRoll final {
public:
    constexpr NumberRoll() noexcept = default;

    /**
     * @brief 重置数字滚动器为未初始化态（首次 feed 直接静止落位不播放滚动）
     * @note 纯内存操作，不阻塞。
     */
    void reset() noexcept;

    /**
     * @brief 喂入最新数值，发生有效变化时启动 180 ms 滚动动画
     * @param value: 新数值
     * @param nowMs: 当前单调时间（ms）
     * @note 首次喂入不播动画，避免刚切页时误滚。
     */
    void update(int32_t value, uint32_t nowMs) noexcept;

    /**
     * @brief 判断当前是否仍在播放滚动动画
     * @param nowMs: 当前单调时间（ms）
     * @retval true 动画进行中
     * @retval false 已静止
     * @note 纯查询，支持 uint32_t 回绕。
     */
    [[nodiscard]] bool active(uint32_t nowMs) const noexcept;

    /**
     * @brief 读取当前滚动缓动进度（Q10：0..1024）
     * @param nowMs: 当前单调时间（ms）
     * @retval 当前缓动进度，静止时返回 kEaseScale
     * @note 纯查询，无浮点。
     */
    [[nodiscard]] uint16_t progressQ10(uint32_t nowMs) const noexcept;

    /**
     * @brief 读取滚动前的旧值
     * @retval 旧值
     * @note 纯查询。
     */
    [[nodiscard]] int32_t previous() const noexcept
    {
        return prev_;
    }

    /**
     * @brief 读取当前目标值
     * @retval 当前值
     * @note 纯查询。
     */
    [[nodiscard]] int32_t current() const noexcept
    {
        return curr_;
    }

    /**
     * @brief 判断本次变化是否为数值增大（决定滚动方向）
     * @retval true 新值大于旧值（向上滚入）
     * @retval false 新值小于等于旧值（向下滚入）
     * @note 纯查询。
     */
    [[nodiscard]] bool increasing() const noexcept
    {
        return curr_ > prev_;
    }

private:
    bool initialized_ = false;
    bool rolling_ = false;
    int32_t prev_ = 0;
    int32_t curr_ = 0;
    uint32_t startMs_ = 0U;
};

/** 双击确认瞬间的选择框“挤压/回弹”反馈动画（<= 120 ms，0 浮点、0 堆）。 */
class SquishAnim final {
public:
    constexpr SquishAnim() noexcept = default;

    /**
     * @brief 触发一次挤压/回弹反馈
     * @param nowMs: 当前单调时间（ms）
     * @note 纯内存操作，不阻塞。
     */
    void trigger(uint32_t nowMs) noexcept;

    /**
     * @brief 立即清除挤压状态
     * @note 纯内存操作，不阻塞。
     */
    void clear() noexcept;

    /**
     * @brief 检查挤压动画是否仍在播放（超时后自动熄灭）
     * @param nowMs: 当前单调时间（ms）
     * @retval true 动画进行中
     * @retval false 已静止
     * @note 每帧调用。
     */
    [[nodiscard]] bool pollActive(uint32_t nowMs) noexcept;

    /**
     * @brief 读取当前时刻选择框向内收缩像素数（0..kSquishMaxPx）
     * @param nowMs: 当前单调时间（ms）
     * @retval 当前收缩像素（0、1 或 2）
     * @note 纯查询，无浮点。
     */
    [[nodiscard]] int16_t insetPx(uint32_t nowMs) const noexcept;

private:
    uint32_t startMs_ = 0U;
    bool active_ = false;
};

/** 屏幕矩形（用于子菜单从图标展开与收回动画）。 */
struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;

    /** @brief 比较屏幕矩形。 */
    [[nodiscard]] bool operator==(const Rect &) const noexcept = default;
};

/** 由同一个绝对动画时钟驱动的视口和内容偏移。 */
struct TransitionPose {
    Rect viewport{};
    int16_t contentOffsetY = 0;
};

/** 页面切换动画类型。 */
enum class TransitionKind : uint8_t {
    kNone = 0,
    kSlideLeft,      // 滑动视差（如自检页 -> 表盘）
    kExpandFromIcon, // 从表盘选中图标展开进入子菜单
    kCollapseToIcon, // 从子菜单收回至表盘选中图标
};

/** 页面切换过渡动画状态（定点 ease-in-out cubic）。 */
struct PageTransition {
    TransitionKind kind = TransitionKind::kNone;
    OledPage fromPage = OledPage::kSystemCheck;
    OledPage toPage = OledPage::kSystemCheck;
    Rect iconRect{};
    uint32_t startMs = 0U;
    uint32_t durationMs = 0U;

    /**
     * @brief 启动一次页面过渡动画
     * @param newKind: 过渡类型
     * @param from: 离开页
     * @param to: 进入页
     * @param anchor: 图标锚点矩形（仅展开/收回使用）
     * @param nowMs: 起始单调时间（ms）
     * @param durMs: 过渡时长（ms）
     * @note 纯内存操作，不阻塞。
     */
    void start(TransitionKind newKind, OledPage from, OledPage to, Rect anchor, uint32_t nowMs,
               uint32_t durMs = kPageTransitionMs) noexcept;

    /**
     * @brief 立即结束当前过渡动画
     * @note 纯内存操作，不阻塞。
     */
    void clear() noexcept;

    /**
     * @brief 检查过渡动画是否仍在进行，若已到期则自动清零
     * @param nowMs: 当前单调时间（ms）
     * @retval true 过渡进行中
     * @retval false 已完成或无过渡
     * @note 由持有者每帧调用。
     */
    [[nodiscard]] bool pollActive(uint32_t nowMs) noexcept;

    /**
     * @brief 计算当前过渡的 ease-in-out cubic Q10 进度（0..1024）
     * @param nowMs: 当前单调时间（ms）
     * @retval 当前 Q10 缓动进度
     * @note 纯查询，无浮点。
     */
    [[nodiscard]] uint16_t progressQ10(uint32_t nowMs) const noexcept;

    /**
     * @brief 计算新页视口与内容垂直偏移，收回与展开对应镜像时刻。
     * @param nowMs: 当前单调时间（ms）。
     * @param screen: 展开后的完整屏幕矩形。
     * @retval 本轮绘制使用的视口及偏移。
     */
    [[nodiscard]] TransitionPose pose(uint32_t nowMs, Rect screen) const noexcept;

    /**
     * @brief 计算子页中第 rowIdx 行的错峰垂直偏移（每行迟 40 ms，从 6 px 上移到 0）
     * @param nowMs: 当前单调时间（ms）
     * @param rowIdx: 行序号（0 起）
     * @retval 该行当前附加的垂直偏移像素（0..kTransitionContentLiftPx）
     * @note 纯查询，收回时自动按镜像时间回退。
     */
    [[nodiscard]] int16_t rowOffsetY(uint32_t nowMs, uint8_t rowIdx) const noexcept;
};

} // namespace app::oled
