// 冻结接口（ADR-0002；ADR-0007 J0 把 FixedStr/DeviceInfo/常量/DispatchResult/Reply 迁到 reply.hpp，语义不变）。实现方不得修改。
// J2b 验收后整个文件移入 reference/。
// 按角色分发手机/报警器命令并生成应答帧。协议见 docs/protocol/phone_protocol.md §3–§5。
// 线程安全：否（链路任务独占）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "rtk/core/alarm_source.hpp"
#include "rtk/core/link_manager.hpp"
#include "rtk/core/reply.hpp"
#include "rtk/core/types.hpp"
#include "rtk/proto/alarm_codec.hpp"
#include "rtk/proto/frame.hpp"

namespace rtk::core {

class CommandDispatcher {
public:
    /**
     * 构造只保存引用并填默认值：product="E1-Lite"，hw_ver="V1.1.0"，board="UM980"，
     * radio_type="5002D"，其余见 DeviceInfo 默认成员；配置块清零后 radio_cfg[17]=0x05。
     */
    CommandDispatcher(AlarmSource &alarm, LinkManager &link) noexcept;

    // 持有引用、独占配置块：不可拷贝、不可移动（C.21 rule of five）
    CommandDispatcher(const CommandDispatcher &) = delete;
    CommandDispatcher &operator=(const CommandDispatcher &) = delete;
    CommandDispatcher(CommandDispatcher &&) = delete;
    CommandDispatcher &operator=(CommandDispatcher &&) = delete;
    ~CommandDispatcher() = default;

    [[nodiscard]] DeviceInfo &info() noexcept
    {
        return info_;
    }

    /**
     * 处理一帧。reply 先 clear()。权限：
     * - kPhone：§4 全部命令及 0x42、0x43、0x44。
     * - kAlarm：0x41 PUSH（解析心跳 → reply.heartbeat，kNoReply）、0x40 QUERY（状态应答）。
     * - 其他组合：kDenied。
     * 未知 ID 与 5.4 保留 ID（0x1E、0x21–0x25、0x28–0x34）：kNoReply（先于角色检查）。
     * 0x40 查询应答 seq 填 0，uptime 填 now_ms，flags bit0 按 LinkManager 中是否存在 PHONE。
     */
    [[nodiscard]] DispatchResult handle(Role role, const proto::FrameView &frame, uint32_t now_ms,
                                        Reply &reply) noexcept;

    /** 组 0x40 主动上报帧（type=PUSH，sender=DEV）；fix_quality 固定 kValueUnknown。 */
    [[nodiscard]] bool build_status_push(uint16_t seq, uint32_t now_ms, Reply &reply) const noexcept;

private:
    AlarmSource &alarm_;
    LinkManager &link_;
    DeviceInfo info_{};
    std::array<uint8_t, kWorkModeLen> work_mode_{};
    std::array<uint8_t, kRadioLen> radio_cfg_{};
    std::array<uint8_t, kLogLen> log_cfg_{};
};

} // namespace rtk::core
