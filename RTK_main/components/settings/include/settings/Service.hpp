// 统一设置服务（ADR-0018 §2.3）：查键→权限→取值域→编码→异步 post；读快照与查询能力域。
// 线程安全取决于所绑定后端（DataCenter Slot 锁 + Executor::post/poll/cancel）；不阻塞、不分配堆、非 ISR。
// 严格遵守 SV1 约束：set() 只 post 并返回 Ticket，绝不调用 Executor::call()。
#pragma once

#include <string_view>

#include "fw/Command.hpp"
#include "settings/Registry.hpp"
#include "settings/Types.hpp"

namespace settings {

/** 统一设置服务：为网页、手机、控制台与未来 MQTT 提供一致的配置读写与取值域查询入口。 */
class Service final {
public:
    constexpr Service() noexcept = default;
    ~Service() = default;
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;
    Service(Service &&) = delete;
    Service &operator=(Service &&) = delete;

    /**
     * @brief  绑定运行时后端（DataCenter 快照读写与执行器 post/poll/cancel）
     * @param  backend: 后端钩子表，其 ctx 生命周期须覆盖服务使用期
     * @note   启动前单线程调用，不阻塞、不分配堆，非 ISR。
     */
    void bindBackend(const Backend &backend) noexcept
    {
        backend_ = backend;
    }

    /**
     * @brief  校验权限与当前取值域后编码并异步投递一条设置命令
     * @param  key:             点分键名
     * @param  value:           文本形式的新值
     * @param  caller:          调用方身份（用于 access 位校验与总线追踪）
     * @param[out] out:         成功时写入异步凭据；失败时重置为无效凭据
     * @param  factoryUnlocked: 当前会话是否已完成工厂解锁（供 kNeedsUnlock 键检查）
     * @retval Error::kOk          已异步投递，可随后 poll()/cancel()
     * @retval Error::kUnknownKey  键名未登记
     * @retval Error::kLocked      调用方无权或未工厂解锁
     * @retval Error::kUnsupported 当前硬件/配置不支持该键
     * @retval Error::kBadValue    格式非法或值超出当前取值域
     * @retval Error::kBusy        目标执行器队列或完成表满
     * @retval Error::kFailed      后端未绑定、执行器未运行或编码失败
     * @note   任意任务可调用（含执行器任务自身）；不阻塞、不分配堆、绝不调用 call()，非 ISR。
     */
    [[nodiscard]] Error set(std::string_view key, std::string_view value, Caller caller,
                            Ticket &out, bool factoryUnlocked = false) noexcept;

    /**
     * @brief  非阻塞查询异步设置凭据的完成状态
     * @param  t: set() 返回的凭据
     * @param[out] out: 仅返回 kDone 时写入模块命令结果
     * @retval PollState::kPending   仍在排队或执行中
     * @retval PollState::kDone      已完成并释放槽位
     * @retval PollState::kCancelled 已取消
     * @retval PollState::kInvalid   凭据无效或后端未绑定
     * @note   任意任务可调用，不阻塞，非 ISR。
     */
    [[nodiscard]] PollState poll(const Ticket &t, fw::CommandResult &out) const noexcept;

    /**
     * @brief  取消一条在途的异步设置请求
     * @param  t: set() 返回的凭据（无效凭据静默忽略）
     * @note   任意任务可调用，不阻塞，非 ISR。
     */
    void cancel(const Ticket &t) const noexcept;

    /**
     * @brief  按键名从快照读取当前设置值（不经命令总线）
     * @param  key: 点分键名
     * @param[out] out: 成功时写入当前值；失败不修改
     * @retval true  已读出当前值
     * @retval false 未知键、只写键、后端未绑定或当前无可读值
     * @note   任意任务可调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool get(std::string_view key, Value &out) const noexcept;

    /**
     * @brief  按 KeyId 从快照读取当前设置值（供注册表遍历生成配置帧）
     * @param  id: 键编号
     * @param[out] out: 成功时写入当前值；失败不修改
     * @retval true  已读出当前值
     * @retval false 越界、只写键、后端未绑定或当前无可读值
     * @note   任意任务可调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool get(KeyId id, Value &out) const noexcept;

    /**
     * @brief  按键名查询该键当前的取值域（结合 GNSS 能力快照）
     * @param  key: 点分键名
     * @param[out] out: 成功时写入当前取值域；未知键时清零
     * @retval true  已查到键定义（是否支持看 out.supported）
     * @retval false 未知键
     * @note   任意任务可调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool domain(std::string_view key, Domain &out) const noexcept;

    /**
     * @brief  按 KeyId 查询该键当前的取值域
     * @param  id: 键编号
     * @param[out] out: 成功时写入当前取值域；越界时清零
     * @retval true  已查到键定义（是否支持看 out.supported）
     * @retval false 越界
     * @note   任意任务可调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool domain(KeyId id, Domain &out) const noexcept;

    /**
     * @brief  按注册表顺序遍历全部设置键
     * @param  fn: 接受 `const KeySpec &` 的可调用对象
     * @note   零堆、纯静态表遍历，非 ISR。
     */
    template <typename Fn> void forEach(const Fn &fn) const noexcept
    {
        for (const KeySpec &spec : keyTable()) {
            fn(spec);
        }
    }

private:
    [[nodiscard]] GnssCaps readGnssCaps() const noexcept;
    [[nodiscard]] bool isGnssEnabled() const noexcept;

    Backend backend_{};
};

} // namespace settings
