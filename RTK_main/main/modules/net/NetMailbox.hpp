// 控制台 AT 透传邮箱（ADR-0021 §2.8 / N1 复审 #8/#9）：console 单写命令、kNet 单写响应，
// 状态原子量发布缓冲所有权；请求归属由 kNet 结算，旧请求终结前不复用缓冲。
// 纯逻辑（无 IDF 依赖），主机可测。堆档位：禁止区；缓冲为成员，不分配堆。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cellular/AtEngine.hpp"
#include "def/NetDef.hpp"
#include "fw/Status.hpp"

namespace app {

/** 控制台 AT 邮箱：console 任务 submit/release，kNet 任务 take/onLine/onDone。 */
class NetMailbox final {
public:
    /** 邮箱状态；kDone 时响应可读，读完须 release()。 */
    enum class State : uint8_t {
        kIdle = 0, ///< 空闲，可提交
        kFilling,  ///< 提交方正在写命令文本
        kPending,  ///< 已提交，等待 kNet 取走
        kRunning,  ///< 已交给 AT 引擎，等待终结
        kDone,     ///< 已完成，响应可读
    };

    constexpr NetMailbox() noexcept = default;
    ~NetMailbox() = default;
    NetMailbox(const NetMailbox &) = delete;
    NetMailbox &operator=(const NetMailbox &) = delete;
    NetMailbox(NetMailbox &&) = delete;
    NetMailbox &operator=(NetMailbox &&) = delete;

    /**
     * @brief 判断 AT 命令是否被禁止
     * @param text: AT 命令文本（已去前导空格）
     * @retval true 禁止：含 ';'/控制字符、不以 AT 开头、ATE*、IPR/QISWTMD/QICSGP（查询与设置）、
     *              CFUN 除 0/1/4 外、裸数据提示命令（QISEND 等）、QIOPEN access_mode≠0
     * @note 纯函数，大小写不敏感，线程/ISR 安全。
     */
    [[nodiscard]] static bool forbidden(std::string_view text) noexcept;

    /**
     * @brief 提交一条命令（console 任务）
     * @param text: 命令文本（不含 CR，长度 < kNetAtCmdBytes）
     * @retval fw::Status::kOk 已进入 kPending
     * @retval fw::Status::kInvalidArg 为空、过长或被禁止
     * @retval fw::Status::kBusy 上一条尚未 release（含旧请求仍在 kRunning）
     * @note 不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Status submit(std::string_view text) noexcept;

    /**
     * @brief 取走待处理命令（kNet 任务）：kPending → kRunning 并分配请求 token
     * @param[out] cmd: 命令文本视图（指向邮箱内部缓冲，kRunning 期间稳定）
     * @param[out] token: 本次请求 token，须随 AtCommand 提交并在 onDone 核对
     * @retval true 已取走
     * @retval false 无待处理命令
     */
    [[nodiscard]] bool take(std::string_view &cmd, uint32_t &token) noexcept;

    /**
     * @brief 引擎拒收时退回 kPending，下一轮重试（kNet 任务）
     */
    void untake() noexcept;

    /**
     * @brief 追加一条响应行（kNet 任务）；凭据行脱敏
     * @param line: 响应行
     */
    void onLine(std::string_view line) noexcept;

    /**
     * @brief 命令终结（kNet 任务）：token 不符的旧回调被丢弃；调用方已放弃则直接回 kIdle
     * @param rsp: 引擎终结信息
     */
    void onDone(const cellular::AtResponse &rsp) noexcept;

    /**
     * @brief 读取邮箱状态
     * @retval 当前状态
     * @note 任意任务，原子读。
     */
    [[nodiscard]] State state() const noexcept
    {
        return static_cast<State>(state_.load(std::memory_order_acquire));
    }

    /**
     * @brief 读取响应（仅 state()==kDone 时有效）
     * @param[out] ok: 命令是否以 OK 终结
     * @retval 响应文本视图（行以 '\n' 分隔，超长截断）
     */
    [[nodiscard]] std::string_view response(bool &ok) const noexcept
    {
        ok = ok_;
        return std::string_view(rsp_.data(), rspLen_);
    }

    /**
     * @brief 释放邮箱（console 任务）
     * @note kDone/kPending 立即回 kIdle；kRunning 只标记放弃，由 kNet 在命令终结时回 kIdle，
     *       因此旧请求终结前缓冲绝不被复用。
     */
    void release() noexcept;

private:
    std::atomic<uint8_t> state_{static_cast<uint8_t>(State::kIdle)};
    std::atomic<bool> abandon_{false};
    std::array<char, kNetAtCmdBytes> cmd_{};
    std::array<char, kNetAtRspBytes> rsp_{};
    size_t cmdLen_ = 0U;
    size_t rspLen_ = 0U;
    bool ok_ = false;
    uint32_t seq_ = 0U;
    uint32_t token_ = 0U;
};

} // namespace app
