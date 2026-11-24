// GNSS 模块（kIo 执行器）：串口接收分流、NMEA 解析、芯片探测/配置状态机、RTCM 管道写回与 gnss 命令组（ADR-0010 §2.7）。
// 线程模型：init() 启动前单线程；start()/wait()/poll()/onCommand 仅 kIo 执行器任务调用；keys/load/save 由 storage
// 在启动期与 kIo 写回时调用。 均非 ISR；不分配堆（demux 缓冲、控制器、NMEA 解析器与命令表均为静态存储）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssMaintenance.hpp"
#include "modules/gnss/GnssWorkMode.hpp"
#include "modules/gnss/RtcmTxFramer.hpp"
#include "cli/Console.hpp"
#include "data/GnssOutput.hpp"
#include "def/GnssDef.hpp"
#include "demux/Demux.hpp"
#include "fw/Command.hpp"
#include "fw/Result.hpp"
#include "gnss/Controller.hpp"
#include "gnss/NmeaParser.hpp"
#include "GgaQualityGate.hpp"
#include "PhoneCompatService.hpp"
#include "PppSequence.hpp"
#include "UbloxExt.hpp"

namespace settings {
struct PeriodDomain;
} // namespace settings

namespace app {
struct AppContext;

/**
 * GNSS 模块：拥有 UART 收发、StreamDemux、NmeaParser、Controller 与 NVS 提示/配置状态。
 * 串口读阻塞放在 wait(maxMs) 中作为 kIo 执行器的 idle 等待；poll(nowMs) 非阻塞驱动状态机与 RTCM 下发。
 */
class GnssModule final {
public:
    static constexpr size_t kDemuxBufBytes = 4096U;
    static constexpr size_t kUartReadChunkBytes = 512U;
    static constexpr size_t kUartRxBufBytes = 4096U;
    static constexpr size_t kUartTxBufBytes = 1024U;
    static constexpr size_t kRtcmTxChunkBytes = 512U;
    static constexpr size_t kMaxBaudCandidates = 9U;
    static constexpr uint32_t kStatusPeriodMs = 500U;

    /** @brief 安排工作模式切换后的接收机持久化。 @note 仅 kIo，多个请求合并成一次待发。 */
    void requestReceiverSave() noexcept
    {
        save_queue_.request();
    }

    constexpr GnssModule() noexcept = default;
    /**
     * @brief 记录所属上下文，不初始化硬件或 RTOS
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅对象构造时调用，不分配、不阻塞；非 ISR。
     */
    constexpr explicit GnssModule(AppContext &context) noexcept : context_(&context) {}
    /**
     * @brief 绑定模块所属上下文
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅消费者启动前单线程调用；不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~GnssModule() = default;
    GnssModule(const GnssModule &) = delete;
    GnssModule &operator=(const GnssModule &) = delete;
    GnssModule(GnssModule &&) = delete;
    GnssModule &operator=(GnssModule &&) = delete;

    /**
     * @brief 仅启动绑定回滚、消费者尚未启动时调用；清除 demux 外供引用，非 ISR。
     * @note 仅启动绑定回滚、消费者尚未启动时调用；不阻塞，非 ISR。
     */
    void unbindStorage() noexcept
    {
        demux_buf_ = {};
    }

    /**
     * @brief 启动前单线程：绑定外供 4 KiB demux 环缓冲。
     * @param demuxBuf: 外供分流环形区，至少 kDemuxBufBytes 字节
     * @retval kOk 已绑定分流区
     * @retval kInvalidState 模块已初始化
     * @retval kInvalidArg 分流区为空或不足
     * @note 仅启动前单线程，非 ISR，不阻塞。
     */
    [[nodiscard]] fw::Status bindStorage(std::span<uint8_t> demuxBuf) noexcept
    {
        if (initialized_) {
            return fw::Status::kInvalidState;
        }
        if ((demuxBuf.data() == nullptr) || (demuxBuf.size() < kDemuxBufBytes)) {
            return fw::Status::kInvalidArg;
        }
        demux_buf_ = demuxBuf.first(kDemuxBufBytes);
        return fw::Status::kOk;
    }

    /**
     * @brief 初始化 UART、分流器与控制器
     * @retval kOk 已初始化或重复调用
     * @retval kInvalidState 上下文、外供存储或控制对象状态不合法
     * @retval kInvalidArg UART/GPIO 或管道参数不合法
     * @retval kNoSpace 驱动或管道创建资源不足
     * @retval kIoError IDF 操作失败；清理失败时优先返回清理错误
     * @note 仅启动前单线程调用；UART 安装可阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result init() noexcept;

    /**
     * @brief 启动失败且 IO 已确认停稳后调用；检查 UART 删除结果，封存存储保留至重启，非 ISR。
     * @retval kOk 未安装 UART 或已成功卸载
     * @retval rtos::fromEspErr 的错误码 UART 卸载失败，安装标记保持不变
     * @note 仅启动任务在消费者停稳后调用；可等待 IDF，非 ISR。
     */
    [[nodiscard]] fw::Status stopBoot() noexcept;

    /**
     * @brief kIo 任务首次 poll 前调用一次：启动 GNSS 上电探测流程并发布初始状态。
     * @retval kOk 已启动控制器并发布状态
     * @retval kInvalidState 尚未初始化
     * @note 仅 kIo 启动阶段；不等待状态机完成，非 ISR。
     */
    [[nodiscard]] fw::Result start() noexcept;

    /**
     * @brief  等 UART 数据或命令事件，最长 maxMs
     * @param  maxMs: 最长等待（ms）
     */
    void wait(uint32_t maxMs) noexcept;

    /**
     * @brief 仅 kIo 调用，非阻塞：执行控制器动作、排空 RTCM 管道到 UART、定期发布状态快照与探测落盘。
     * @param nowMs: 当前单调时间（毫秒）
     * @retval true RTCM 管道仍有积压
     * @retval false 模块未初始化或无立即积压
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;

    /**
     * @brief 处理一条应用层指令（kProbe/kReset/kSetRate/kSend/kSetOut/kReadback）
     * @param cmd: 指令（verb 为 GnssVerb 或 GnssSubVerb）
     * @param[out] result: 输出，执行结果；不支持的子命令返回 kNotSupported
     * @note 在 IO 执行器任务里调用，不阻塞；读回子命令只发起请求，结果由 AppData 发布
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss probe：标记人工重探，清空最近定位并让控制器重新探测
     * @param  nowMs: 当前时间
     * @param[out] result: 写入命令结果（kOk；参数校验在 onCommand）
     * @note   仅 kIo 调用，非阻塞。
     */
    void cmdProbe(uint32_t nowMs, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss reset：清空最近定位后停再启动控制器
     * @param  nowMs: 当前时间
     * @param[out] result: 写入命令结果（kOk；参数校验在 onCommand）
     * @note   仅 kIo 调用，非阻塞。
     */
    void cmdReset(uint32_t nowMs, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss rate：两阶段提交更新输出频率（ADR-0012 §2.4）
     * @param  cmd: 指令，args[0] 必须是支持的频率
     * @param  nowMs: 当前时间
     * @param[out] result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
     * @note   仅 kIo 调用，非阻塞；NVS 写只在非 kRunning 的直落盘路径发生。
     */
    void cmdSetRate(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;
    /**
     * @brief  gnss setout：两阶段提交更新输出表
     * @param  cmd: 指令，args 为 GnssOutConfig 编码块
     * @param  nowMs: 当前时间
     * @param[out] result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
     * @note   仅 kIo 调用，非阻塞。
     */
    void cmdSetOut(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;

    /**
     * @brief 从 NVS 编码恢复接收机家族
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 1 字节小端值；多余字节忽略
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值不合法
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadFamily(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把接收机家族编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 成功为 1，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveFamily(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 编码恢复接收机型号
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 1 字节小端值；多余字节忽略
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值不合法
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadModel(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把接收机型号编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 成功为 1，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveModel(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 编码恢复探测波特率
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 4 字节小端值；多余字节忽略
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值不合法
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadBaud(void *obj, std::span<const uint8_t> in) noexcept;

    /** @brief 加载用户波特率。 @param in: 小端 u32。 @retval kOk 已加载；其他为无效值。 */
    [[nodiscard]] static fw::Result loadUserBaud(void *, std::span<const uint8_t> in) noexcept;
    /** @brief 编码用户波特率。 @param out: 输出缓冲。 @param len: 已编码长度。 @retval kOk 已编码；其他为缓冲不足。 */
    [[nodiscard]] static fw::Result saveUserBaud(const void *, std::span<uint8_t> out,
                                                 size_t &len) noexcept;
    /**
     * @brief 把探测波特率编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 成功为 4，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveBaud(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 编码恢复定位频率
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 1 字节小端值；多余字节忽略
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值不合法
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadRate(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把定位频率编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 成功为 1，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveRate(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 编码恢复GSV 开关
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 1 字节小端值；多余字节忽略
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值不合法
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadGsv(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把GSV 开关编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 成功为 1，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveGsv(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 恢复统一输出配置
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 输入字节视图
     * @retval kOk 已恢复
     * @retval kInvalidArg 编码非法；原配置非法时回填默认值
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadOut(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把有效输出配置或默认配置编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 有效字节数输出
     * @retval kOk 已写入 8 字节并设置 len
     * @retval kNoSpace 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveOut(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 编码恢复 GGA 质量门控开关
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 1 字节值（0/1）
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值越界
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadGgaGate(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把 GGA 质量门控开关编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲
     * @param[out] len: 成功为 1，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveGgaGate(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从 NVS 编码恢复 GGA 质量门控最低卫星数
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param in: 1 字节值（0..40）
     * @retval kOk 已应用
     * @retval kInvalidArg 输入不足或数值越界
     * @note 仅启动期加载或 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result loadGgaMinSats(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把 GGA 质量门控最低卫星数编码供 NVS 保存
     * @param obj: 未使用；通过 appContext().gnss 定位单例
     * @param[out] out: 输出缓冲
     * @param[out] len: 成功为 1，失败不变
     * @retval kOk 已写入并更新 len
     * @retval kInvalidArg 输出不足，len 不变
     * @note 仅 kIo 串行路径，不阻塞，非 ISR。
     */
    static fw::Result saveGgaMinSats(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 查询当前 GGA 质量门控是否允许输出 GGA（蓝牙直传/查询与将来 4G NTRIP 共用）
     * @retval true 允许输出 GGA
     * @retval false 当前处于阻断态，应丢弃 GGA
     * @note 无锁原子读取，任意任务安全，不阻塞。
     */
    [[nodiscard]] bool ggaAllowed() const noexcept;

    /**
     * @brief 从 NVS 加载 gnss.base 组指定槽位的键值。
     * @param slot: gnss_store::kBaseKeys 下标（0..9）。
     * @param in: 小端标量或 blob 输入视图。
     * @retval kOk: 加载成功；kInvalidArg: 槽位、长度或取值非法。
     * @note 仅启动期或持锁上下文，不阻塞，非 ISR。
     */
    [[nodiscard]] static fw::Result loadBaseAt(size_t slot, std::span<const uint8_t> in) noexcept;

    /**
     * @brief 编码 gnss.base 组指定槽位供 NVS 持久化。
     * @param slot: gnss_store::kBaseKeys 下标（0..9）。
     * @param[out] out: 输出缓冲。
     * @param[out] len: 写入字节数。
     * @retval kOk: 写入成功；kNoSpace: 空间不足；kInvalidArg: 槽位非法。
     * @note 仅 kIo 或持锁上下文，不阻塞，非 ISR。
     */
    [[nodiscard]] static fw::Result saveBaseAt(size_t slot, std::span<uint8_t> out,
                                               size_t &len) noexcept;

    /**
     * @brief StoreKey NVS 加载静态包装回调（转发至 loadBaseAt）。
     * @param obj: 未使用。
     * @param in: 输入字节视图。
     * @retval 同 loadBaseAt。
     */
    template <size_t Slot>
    [[nodiscard]] static fw::Result loadBaseSlot(void *obj, std::span<const uint8_t> in) noexcept
    {
        static_cast<void>(obj);
        return loadBaseAt(Slot, in);
    }

    /**
     * @brief StoreKey NVS 保存静态包装回调（转发至 saveBaseAt）。
     * @param obj: 未使用。
     * @param[out] out: 输出缓冲。
     * @param[out] len: 输出字节数。
     * @retval 同 saveBaseAt。
     */
    template <size_t Slot>
    [[nodiscard]] static fw::Result saveBaseSlot(const void *obj, std::span<uint8_t> out,
                                                 size_t &len) noexcept
    {
        static_cast<void>(obj);
        return saveBaseAt(Slot, out, len);
    }

    /**
     * @brief 跨任务安全读取 gnss.base 完整配置副本
     * @retval 当前生效的 gnss.base 配置副本
     */
    [[nodiscard]] GnssBaseConfig readBaseConfig() noexcept;
    /** @brief 跨任务安全更新 gnss.base 配置（先拷贝或修改再提交）。 */
    void writeBaseConfig(const GnssBaseConfig &cfg) noexcept;

private:
    class DemuxSink final : public demux::Sink {
    public:
        constexpr DemuxSink() noexcept = default;
        /**
         * @brief 记录分流回调所属模块
         * @param owner: 所属 GNSS 模块，回调前须有效
         * @note 仅构造时使用，不分配、不访问硬件或 RTOS。
         */
        constexpr explicit DemuxSink(GnssModule *owner) noexcept : owner_(owner) {}
        /**
         * @brief 把本 sink 绑定到模块实例
         * @param owner: owner 见调用点
         * @param handle: 会话句柄
         * @retval 无返回值（嵌套类方法的返回类型解析限制，此处显式标注 void）
         * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
         */
        void bind(GnssModule *owner) noexcept
        {
            owner_ = owner;
        }
        void onFrame(const demux::Decoder &decoder, uint8_t index,
                     std::span<const uint8_t> frame) noexcept override;
        void onReject(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> prefix,
                      demux::RejectCause cause, bool last) noexcept override;

    private:
        GnssModule *owner_ = nullptr;
    };
    static_assert(std::is_trivially_destructible_v<DemuxSink>,
                  "DemuxSink must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<demux::Demux>,
                  "Demux must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<::gnss::NmeaParser>,
                  "NmeaParser must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<::gnss::Controller>,
                  "Controller must be trivially destructible");

    static_assert(std::is_trivially_destructible_v<::gnss::RoverConfig>,
                  "RoverConfig must be trivially destructible");
    /**
     * @brief 按需原地构造流动站配置
     * @retval 已构造的流动站配置引用
     * @note 仅启动期或 kIo 串行调用；不分配堆、不阻塞，非 ISR。
     */
    ::gnss::RoverConfig &ensureRoverCfg() noexcept
    {
        if (!rover_cfg_.has_value()) {
            rover_cfg_.emplace();
        }
        return *rover_cfg_;
    }

    /**
     * @brief 取得已绑定的应用上下文
     * @retval 模块所属上下文引用
     * @note 仅模块所属任务调用；须已 bindContext，非 ISR，不阻塞。
     */
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    /**
     * @brief  处理分流器交付的一帧：喂控制器、应用输出，并记 D 级帧日志
     * @param  decoder: 识别出该帧的解码器
     * @param  frame: 完整帧字节
     */
    void handleFrame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept;
    /**
     * @brief  分流器拒收字节：记 D 级日志与可选 hex 转储
     * @param  decoder: 拒收的解码器
     * @param  prefix: 被拒收的字节
     * @param  cause: 拒收原因
     * @param  last: 是否为本次重同步的最后一个拒收
     */
    void handleReject(const demux::Decoder &decoder, std::span<const uint8_t> prefix,
                      demux::RejectCause cause, bool last) noexcept;
    /**
     * @brief 跑一轮控制器状态机并把动作落到 UART，收尾时检测读回窗口结束
     * @param nowMs: 单调时间（毫秒）
     * @note 在模块任务里调用，最多 kMaxActionsPerPoll 次动作，不阻塞
     */
    void pollController(uint32_t nowMs) noexcept;
    /**
     * @brief  把控制器下发的一条 UART 写动作落到串口，并记录探测/配置进度
     * @param  act: 控制器动作（kind 必为 kWrite）
     * @param  nowMs: 本轮单调时钟（毫秒）
     * @note   仅 kIo 调用；调用前由 pollController 保证 RTCM 帧边界。
     */
    void applyWriteAction(const ::gnss::Action &act, uint32_t nowMs) noexcept;
    /**
     * @brief  状态迁移的副作用：探测结果、步进计数与输出计划重算
     * @param  prev: 迁移前状态
     * @param  cur: 迁移后状态
     * @param  nowMs: 本轮单调时钟（毫秒）
     * @note   仅 kIo 调用；只在 prev != cur 时调用一次。
     */
    void onControllerStateChanged(::gnss::State prev, ::gnss::State cur, uint32_t nowMs) noexcept;
    /**
     * @brief  记录最近发出的 UBX-CFG-VALSET 在途事务，供后续 ACK/NAK/超时跟踪与键 ID 告警
     * @param  bytes: 刚写入串口的命令帧字节
     * @param  nowMs: 发送时刻单调时钟（ms）
     * @param  timeoutMs: 应答超时（ms）
     * @param  expectReply: 是否等待应答（切波特率为 false）
     * @note   仅 kIo 调用；非 VALSET 帧忽略，不分配堆、不阻塞。
     */
    void recordValsetTx(std::span<const uint8_t> bytes, uint32_t nowMs, uint32_t timeoutMs,
                        bool expectReply) noexcept;
    /**
     * @brief  处理 UBX 诊断与序列号帧（VALSET ACK/NAK 键日志与 SEC-UNIQID 板卡序列号）
     * @param  frame: 已校验的 UBX 完整帧字节
     * @param  nowMs: 当前单调时钟（ms）
     * @note   仅 kIo 调用；不分配堆、不阻塞。
     */
    void handleUbxFrame(std::span<const uint8_t> frame, uint32_t nowMs) noexcept;
    /**
     * @brief  低频冷路径告警日志：格式化在途 VALSET 键列表（独立栈帧避免污染高频 RX 路径）
     * @param  kind: 事件标签（"NAK" 或 "timeout"）
     * @param  keys: 对应的配置键列表
     * @note   仅 kIo 调用；不分配堆。
     */
    [[gnu::noinline]] static void logValsetIssue(const char *kind,
                                                 std::span<const uint32_t> keys) noexcept;
    /**
     * @brief  ZED-F9P 进入运行态后非阻塞轮询一次 UBX-SEC-UNIQID（读不到留空不报错）
     * @param  nowMs: 本轮单调时钟（毫秒）
     * @note   仅 kIo 调用；不分配堆，发送失败静默忽略。
     */
    void pollUbloxBoardSn(uint32_t nowMs) noexcept;
    /**
     * @brief  进入上电或探测态时复位重探相关临时状态（序列号、VALSET 键表与工作模式）
     * @note   仅 kIo 调用；不分配堆、不阻塞。
     */
    void resetOnReprobe() noexcept;
    /**
     * @brief 登记基础解码器与各驱动私有解码器（顺序即探测优先级）
     * @param receivers: 编译期启用的接收机实例表（可为空表）
     * @note 仅 init() 调用；重复登记同一解码器会被跳过。
     */
    void registerDecoders(std::span<const ::gnss::Receiver *const> receivers) noexcept;
    /**
     * @brief 生成探测用的候选波特率表：默认值优先，其后按从快到慢回退
     * @note 仅 init() 调用；重复值只保留一次，顺序即尝试顺序。
     */
    void initBaudCandidates() noexcept;
    /**
     * @brief  读回期间把一帧转成一行文本追加到 gnssReadbackText（ADR-0012 §2.7）
     * @param  decoder: 该帧的解码器
     * @param  frame: 整帧字节
     * @note   仅 kIo 调用；命中即计数，容量不足时丢弃该行并置 truncated。
     */
    void appendReadbackLine(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept;
    /**
     * @brief  发起一次配置读回，并把被拒原因就地发布
     * @param  nowMs: 当前时间
     * @param[out] result: 写入命令结果（kOk / kBusy / kNotSupported）
     * @note   仅 kIo 调用；成功时清空上一轮文本与计数。
     */
    void startReadback(uint32_t nowMs, fw::CommandResult &result) noexcept;
    /**
     * @brief  发布一次读回结论（seq 严格递增，保证控制台一定能看到变化）
     * @param  lines: 命中行数
     * @param  len: 文本有效字节
     * @param  supported: 驱动是否支持读回
     * @param  truncated: 是否有行因容量不足被丢弃
     * @param  busy: 是否因状态不对被拒
     * @note   仅 kIo 调用；文本本体已在 gnssReadbackText 中。
     */
    void publishReadback(uint16_t lines, uint16_t len, bool supported, bool truncated,
                         bool busy) noexcept;
    /**
     * @brief  把 RTCM 管道中的待发字节写到 GNSS UART
     * @param  stopAtFrameBoundary: true 时仅补完当前未完 RTCM3 帧即停止，便于插入配置命令
     * @retval true 管道仍有进展
     * @retval false 已排空、已对齐边界或写失败（失败时丢弃积压并记 W 级日志）
     * @note   仅 kIo 调用，非阻塞。
     */
    [[nodiscard]] bool pumpRtcmPipe(bool stopAtFrameBoundary = false) noexcept;
    /** @brief 丢弃未发 RTCM 块并清空管道与帧边界跟踪器。 */
    void discardRtcmPipe() noexcept;
    /** @brief 处理 RTCM 串口写错误：丢弃残余字节、复位边界跟踪并记录诊断。 @param written: 错误码。 @param st: 控制器状态。 */
    void handleRtcmWriteError(int written, ::gnss::State st) noexcept;
    /**
     * @brief  在发送配置帧前确保已补完当前在途 RTCM3 帧（管道空超时 200 ms 后保护性复位边界）
     * @param  nowMs: 当前单调时钟（ms）
     * @retval true 已位于完整 RTCM3 帧边界，可写配置帧
     * @retval false 当前 RTCM3 帧尚未发完，留待下轮继续
     * @note   仅 kIo 调用，非阻塞。
     */
    [[nodiscard]] bool ensureRtcmFrameBoundary(uint32_t nowMs) noexcept;
    /**
     * @brief 发布 GNSS 控制器、UART、分流器与 RTCM 管道快照
     * @param nowMs: 当前单调时间（毫秒）
     * @note 仅 kIo，非 ISR；主题与统计读取内部加锁。
     */
    void publishStatus(uint32_t nowMs) noexcept;
    /**
     * @brief 把探测到的驱动信息落库
     * @retval kOk 已落库或提示未变；其他为存储错误。
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    fw::Result persistDetectedHint() noexcept;
    /**
     * @brief 在接收机配置完成后按启动策略保存探测提示。
     * @note 仅 kIo，可能写 NVS，非 ISR。
     */
    void persistProbeHint() noexcept;
    /**
     * @brief 按动作给 GNSS 上电或断电
     * @param on: true 表示上电
     * @retval kOk 表示成功；其他为失败原因
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    [[nodiscard]] fw::Result applyPower(bool on) noexcept;
    /**
     * @brief 切换 GNSS UART 波特率
     * @param baud: 波特率
     * @retval kOk 表示成功；其他为失败原因
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    [[nodiscard]] fw::Result applyBaud(uint32_t baud) noexcept;

    /**
     * @brief 返回 GNSS 控制器引用
     * @retval 控制器引用（生命周期由模块持有）
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     * @note 仅 init 已原地构造后使用；运行期不 reset，缺失表示内部生命周期约束被破坏。
     */
    [[nodiscard]] ::gnss::Controller &controller() noexcept;
    /**
     * @brief 返回 NMEA 解析器引用
     * @retval 解析器引用（生命周期由模块持有）
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    [[nodiscard]] ::gnss::NmeaParser &parser() noexcept;
    /**
     * @brief 返回 GNSS 字节流解复用器引用
     * @retval 解复用器引用（生命周期由模块持有）
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    [[nodiscard]] demux::Demux &streamDemux() noexcept;
    /**
     * @brief 构造子对象并回填未被 NVS 覆盖的默认配置
     * @retval kOk: 子对象已就绪；其他: 保留给上位调用方
     * @note 仅 IO 任务启动期调用；emplace 不分配堆（成员已在 .bss）。
     */
    [[nodiscard]] fw::Result initObjects() noexcept;
    /**
     * @brief 起 UART、RTCM 管道、解码器表与分流器，并算出候选波特率
     * @retval kOk: 通路就绪；其他: 已回滚并记录失败原因
     * @note 仅启动期单线程调用；UART 安装可阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result initPipelines() noexcept;
    /**
     * @brief 用默认输出计划启动控制器并发布初始状态
     * @retval kOk: 控制器已启动；其他: 已回滚并记录失败原因
     * @note 仅启动期单线程调用；非阻塞、不分配堆。
     */
    [[nodiscard]] fw::Result startController() noexcept;
    /**
     * @brief 安装并配置 GNSS UART 与电源 GPIO
     * @retval kOk 驱动与引脚已配置
     * @retval kInvalidArg IDF 参数不合法
     * @retval kInvalidState IDF 驱动状态不合法
     * @retval kNoSpace IDF 内存不足
     * @retval kIoError 其他 IDF 错误；若清理失败则返回清理错误
     * @note 仅启动期单线程；可等待 IDF 驱动，非 ISR。
     */
    [[nodiscard]] fw::Result initUart() noexcept;
    /**
     * @brief 把原始字节直接写入 GNSS UART
     * @param cmd: cmd 见调用点
     * @param[out] result: result 见调用点
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    void sendRaw(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 按输出配置生成报文输出计划
     * @retval 生成的输出计划
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    [[nodiscard]] ::gnss::OutputPlan makePlan() noexcept;
    /**
     * @brief 推进待应用配置的超时计时
     * @param nowMs: 单调毫秒时间戳
     * @note 仅启动期或 kIo 串行路径，非 ISR；不分配堆。
     */
    void tickPendingApply(uint32_t nowMs) noexcept;

    /** @brief 受理一次性命令。 @param cmd: 命令。 @param nowMs: 当前时间。 @param result: 受理状态和完成序号。 */
    void cmdMaintenance(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;
    /**
     * @brief 尝试启动暂缓的切速或自动保存。
     * @param nowMs: 当前单调时间（ms）。
     * @note 仅 kIo 调用，非 ISR。
     */
    void tickMaintenance(uint32_t nowMs) noexcept;
    /**
     * @brief 计算探测成功后的目标运行波特率（用户 NVS gnss/baud 优先于 RTK_GNSS_TARGET_BAUD）。
     * @retval 目标波特率（bps）。
     * @note 仅 kIo 或启动前单线程调用，不阻塞、非 ISR。
     */
    [[nodiscard]] uint32_t targetBaud() const noexcept;
    /**
     * @brief 探测成功后按目标运行波特率尝试同步一次（仅运行态，不发 SAVECONFIG）。
     * @param nowMs: 当前单调时间（ms）。
     * @note 仅 kIo 调用，非 ISR。
     */
    void alignBootBaud(uint32_t nowMs) noexcept;
    /**
     * @brief 按当前检测结果刷新内存与 NVS 探测提示，并同步控制器候选偏好。
     * @note 仅 kIo 调用；安全缺省或 NVS 未就绪时只刷新内存提示，不写 NVS。
     */
    void syncBootHint() noexcept;
    /** @brief 写入已确认的波特率及探测缓存，随后请求接收机保存。 @param baud: 已确认的速率。 @retval true 保存请求已受理。 */
    [[nodiscard]] bool persistBaudAndSave(uint32_t baud) noexcept;
    /** @brief 公布本次手动维护结果。 @param seq: 请求序号。 @param kind: 操作。 @param result: 操作结果。 */
    void setConsoleMaintenanceResult(uint32_t seq, ::gnss::Maintenance kind,
                                     ::gnss::MaintenanceResult result) noexcept;
    /** @brief 完成切速或切速后的保存。 @param stats: 本次结果。 @param previousSeq: 切速结果的序号。 */
    void completeBaudMaintenance(const ::gnss::ControllerStats &stats,
                                 uint32_t previousSeq) noexcept;
    /**
     * @brief 处理启动运行切速的完成结果（成功只更新缓存，失败记 trace 与 baud_fail）。
     * @param baud: 本次目标波特率。
     * @param result: 控制器返回的切速结果。
     * @note 仅 kIo 调用，非 ISR。
     */
    void completeBootBaudMaintenance(uint32_t baud, ::gnss::MaintenanceResult result) noexcept;
    /** @brief 处理本轮完成的维护操作并公布结果。 @param nowMs: 当前时间。 @note 仅 kIo。 */
    void completeMaintenance(uint32_t nowMs) noexcept;
    /** @brief 将逻辑缺省设置写入 NVS 并清理探测缓存。 @retval kOk 成功；其他为存储错误。 */
    [[nodiscard]] fw::Result resetStoredSettings() noexcept;
    /** @brief 推进待启动的配置变更。 @param nowMs: 当前时间。 */
    void beginPendingApply(uint32_t nowMs) noexcept;
    /** @brief 校验并开始用户波特率变更。 @param cmd: 请求。 @param nowMs: 当前时间。 @param result: 受理结果。 */
    void cmdBaud(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;
    /** @brief 处理 GnssSubVerb::kMode（工作模式切换）命令。 */
    void cmdMode(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;
    /** @brief 处理 GnssSubVerb::kSetBase（gnss.base 子包更新与持久化）命令。 */
    void cmdSetBase(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 处理来自 GNSS RX 侧的一条 RTCM3 帧：通知自测并在 kBaseTx 且手机就绪时推入转发环
     * @param frame: 完整 RTCM3 线路帧字节
     */
    void handleRxRtcmFrame(std::span<const uint8_t> frame) noexcept;
    /**
     * @brief 更新 6 KiB 最新句缓存并按订阅掩码向 App FanoutRing 推送可订阅语句
     * @param decoder: 命中解码器
     * @param frame: 完整帧字节
     * @param now: 当前单调时间（ms）
     */
    void publishAppFrame(const demux::Decoder &decoder, std::span<const uint8_t> frame,
                         uint32_t now) noexcept;
    /**
     * @brief 分派常规 GNSS 动词与扩展子命令
     * @param cmd: 待执行指令
     * @param nowMs: 当前单调时间（ms）
     * @param result: 执行结果输出
     */
    void dispatchStandardCmd(const fw::Command &cmd, uint32_t nowMs,
                             fw::CommandResult &result) noexcept;
    /** @brief 回填未从 NVS 加载的 gnss.base 默认配置项。 */
    void initBaseDefaults() noexcept;
    /** @brief 驱动工作模式状态机并发布 gnssWorkMode 快照及 SAVECONFIG 请求。 */
    void pollWorkMode(uint32_t nowMs) noexcept;
    /**
     * @brief 懒构造返回底层 base_cfg_ 引用（外部须持有自旋锁或单线程期）
     * @retval 已就位的 GnssBaseConfig 可写引用
     */
    GnssBaseConfig &ensureBaseCfg() noexcept
    {
        if (!base_cfg_.has_value()) {
            base_cfg_.emplace();
        }
        return *base_cfg_;
    }
    /**
     * @brief 把一个控制器动作落到 UART（上电/改波特率/下发报文）
     * @param act: 控制器给出的动作
     * @param nowMs: 本轮单调时钟（毫秒）
     * @note 在模块任务里调用，阻塞在 UART 写队列上，非 ISR
     */
    void applyAction(const ::gnss::Action &act, uint32_t nowMs) noexcept;
    /**
     * @brief 读回收尾（ADR-0012 §2.7）：控制器回 kRunning 并推进 readbackSeq 后发布一次结论
     * @note 在模块任务里调用，非阻塞
     */
    void finishReadbackIfDone() noexcept;
    /**
     * @brief  把控制器状态变化与探测结果记为 I 级日志并更新快照
     * @param  nowMs: 本轮单调时钟（毫秒）
     */
    void updateControllerStatus(uint32_t nowMs) noexcept;
    /**
     * @brief 卸载已安装 UART 后返回初始化错误
     * @param original: 最初的初始化错误
     * @retval original 清理成功或无需清理
     * @retval 清理错误码 UART 卸载失败，优先报告清理错误
     * @note 仅启动失败路径；可等待 IDF 驱动，非 ISR。
     */
    [[nodiscard]] fw::Result initFailed(fw::Result original) noexcept;
    AppContext *context_ = nullptr;
    std::optional<DemuxSink> sink_{};
    std::optional<demux::Demux> demux_{};
    std::optional<::gnss::NmeaParser> parser_{};
    std::optional<::gnss::Controller> controller_{};
    ::gnss::ControllerStats prev_ctrl_stats_{};
    /** 读回状态（ADR-0012 §2.7）：文本在 gnssReadbackText，这里只留长度/计数与 seq 账本。 */
    uint16_t readback_len_ = 0U;
    uint16_t readback_lines_ = 0U;
    bool readback_truncated_ = false;
    /** 当前这轮读回开始时的 stats 值：与 stats 不等即表示刚收尾。 */
    uint32_t readback_base_seq_ = 0U;
    /** 已发布的 seq：失败路径会“预支”到 stats+1，发布时取 max 以免与控制台记录相撞。 */
    uint32_t readback_pub_seq_ = 0U;
    /** 上一次已上报的未知字节计数，用于按增量输出 `GNSS N unknown bytes dropped`（§2.11）。 */
    uint32_t prev_unknown_bytes_ = 0U;
    ::gnss::Hint hint_{};
    /** 探测命中后置位，首次回到 kRunning（配置与输出阶段都完成）时保存提示并清零。 */
    bool detect_pending_ = false;
    std::optional<::gnss::Timing> timing_{};
    std::optional<::gnss::RoverConfig> rover_cfg_{};
    data::GnssOutConfig out_cfg_{};
    std::optional<GnssBaseConfig> base_cfg_{};
    uint16_t base_loaded_mask_ = 0U;
    std::optional<GnssWorkMode> work_mode_{};
    /** 待落地的变更：等控制器 outputSeq 前进后再决定写 NVS 还是回滚。 */
    enum class PendingApply : uint8_t {
        kNone = 0,
        kOut = 1,
        kRate = 2,
    };
    GnssSaveQueue save_queue_{};
    /** @brief 受理 PPP 开关设置命令（延迟结单）。 @param cmd: PPP 开关请求。 @param[out] result: 受理结果。 @note 仅 kIo。 */
    void cmdApplyPpp(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    /** @brief 受理截止角/星座配置设置命令（延迟结单）。 @param cmd: 6 B 配置请求。 @param[out] result: 受理结果。 @note 仅 kIo。 */
    void cmdApplySatTrack(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    /** @brief 推进 PPP/星座脚本或上电重放。 @param nowMs: 当前时钟。 @retval true 本轮独占 UART。 @note 仅 kIo。 */
    [[nodiscard]] bool pollPppAndSat(uint32_t nowMs) noexcept;

    /** @brief 结束 PPP 脚本并按结果落盘与结单。 @param status: 执行结果。 @note 仅 kIo，成功请求可写 NVS。 */
    void finishPpp(fw::Result status) noexcept;

    /** @brief 结束星座配置脚本并按结果落盘与结单。 @param status: 执行结果。 @note 仅 kIo，成功请求可写 NVS。 */
    void finishSatTrack(fw::Result status) noexcept;

    /** @brief NVS 落盘失败后尽最大努力向接收机回写旧卫星快照。 @retval true 已启动回滚序列。 */
    [[nodiscard]] bool startSatRollback() noexcept;

    /** @brief 按当前检测型号与固件版本计算设置层 constMask。 @param model: 接收机型号。 @retval 星座能力掩码。 */
    [[nodiscard]] uint8_t computeConstMask(::gnss::Model model) const noexcept;

    /** @brief 触发上电或重配置后的 PPP/星座重放。 @note 仅 kIo。 */
    void startPendingReplay() noexcept;

    /** @brief 执行设置服务与维护命令。 @param cmd: 请求。 @param nowMs: 时钟。 @param[out] result: 受理结果。 @note 仅 kIo。 */
    void cmdSetting(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;

    /** @brief 校验并应用单项周期补丁。 @param cmd: 请求。 @param nowMs: 时钟。 @param[out] result: 受理结果。 @note 仅 kIo。 */
    void cmdPatchOutput(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;

    /** @brief 用执行器完成表回传结果。 @param seq: 凭据序号。 @param status: 执行结果。 @note 仅 kIo。 */
    void completeSetting(uint32_t &seq, fw::Result status) noexcept;

    /** @brief 回滚未落盘的输出配置。 @note 仅 kIo，重配由调用方触发。 */
    void restorePendingApply() noexcept;

    /** @brief 重探或复位前恢复配置并结单。 @note 仅 kIo。 */
    void abortPendingSettings() noexcept;

    /** @brief 按驱动能力将 App 卫星输出槽位映射到实际语句。 @param msg: App 语句。 @retval 驱动语句。 @note 仅 kIo。 */
    [[nodiscard]] ::gnss::OutputMsg outputCapabilityMsg(::gnss::OutputMsg msg) noexcept;

    /** @brief 首次识别后恢复该型号可执行的旧 NVS 值。 @note 仅 kIo。 */
    void restoreSettingsForModel() noexcept;

    /** @brief 将按型号回退后的定位频率写回 NVS 并推进配置版本。 @note 仅 kIo，写 NVS 可阻塞。 */
    void persistRestoredRate() noexcept;

    /** @brief 恢复已保存的旧输出周期。 @note 仅 kIo。 */
    void restoreOutputPeriods() noexcept;
    /** @brief 判断周期在给定定位频率下是否满足 F9P 整倍导航历元规则（0 或 1..255 倍 Tnav）。 @param periodMs: 语句周期（ms）。 @param rateHz: 定位频率（Hz）。 @retval true 合法；false 非法。 @note 纯函数，线程/ISR 安全。 */
    [[nodiscard]] static bool isF9pPeriodValid(uint32_t periodMs, uint8_t rateHz) noexcept;
    /** @brief 在升序候选周期表中查找 rateHz 下不小于 periodMs 的最近 F9P 合法整倍历元周期（找不到取最大合法值）。 @param periods: 候选周期表（ms）。 @param periodMs: 期望周期（ms）。 @param rateHz: 定位频率（Hz）。 @retval 对齐后的合法周期（ms）。 @note 纯函数，线程/ISR 安全。 */
    [[nodiscard]] static uint16_t alignF9pPeriodMs(std::span<const uint16_t> periods,
                                                   uint32_t periodMs, uint8_t rateHz) noexcept;

    /** @brief 过滤并填充单条语句在当前型号/频率下的设置能力周期域。 @param model: 型号。 @param navRateHz: 生效定位频率。 @param minPeriod: 最小周期（ms）。 @param gsvDisabled: GSV 是否关闭。 @param periods: 静态周期表。 @param[out] domain: 输出周期域。 @note 纯函数，线程/ISR 安全。 */
    static void fillOutputPeriodDomain(::gnss::Model model, uint8_t navRateHz, uint32_t minPeriod,
                                       bool gsvDisabled, std::span<const uint16_t> periods,
                                       settings::PeriodDomain &domain) noexcept;
    /** @brief 将启动对齐后的 F9P 输出周期写回 NVS 并推进配置版本。 @note 仅 kIo，写 NVS 可阻塞。 */
    void persistAlignedF9pOutput() noexcept;
    /** @brief 恢复非 F9P 型号已保存的旧输出周期。 @note 仅 kIo。 */
    void restoreLegacyOutputPeriods() noexcept;
    /** @brief 将 F9P 显式输出周期对齐到 rateHz 下不小于原值的最近合法整倍历元周期（0 保持 0）。 @param rateHz: 目标定位频率（Hz）。 @retval true 至少有一项周期被修改；false 全部已合法。 @note 仅 kIo。 */
    [[nodiscard]] bool alignF9pOutputConfig(uint8_t rateHz) noexcept;

    /** @brief 发布当前型号和定位频率对应的取值域。 @note 仅 kIo。 */
    void publishCaps() noexcept;

    /** @brief 写一条配置脚本命令到 GNSS UART。 @param tx: 待发字节。 @param nowMs: 发送时钟。 @param timeoutMs: 应答超时。 @retval true 全部写入成功。 @note 仅 kIo。 */
    [[nodiscard]] bool writeSeqTx(std::span<const uint8_t> tx, uint32_t nowMs,
                                  uint32_t timeoutMs) noexcept;

    /** @brief 查询当前探测到的 F9P 固件版本是否支持 SBAS 键控制（HPG >= 1.13）。 @retval true 支持。 @note 仅 kIo。 */
    [[nodiscard]] bool f9pIncludeSbas() const noexcept;

    /** @brief 查询配置通道是否空闲。 @retval true PPP 或星座脚本可启动。 @note 仅 kIo。 */
    [[nodiscard]] bool canStartSeq() noexcept;

    /** @brief 解析 GGA 门控修改。 @param cmd: 门控命令。 @param[out] next: 新配置。 @retval 是否有效。 @note 仅 kIo。 */
    [[nodiscard]] bool decodeGgaGateChange(const fw::Command &cmd,
                                           ::gnss::GgaGateConfig &next) const noexcept;

    /** @brief 单键持久化后发布网页门控修改。 @param cmd: 门控命令。 @param[out] result: 执行结果。 @note 仅 kIo，写 NVS 可阻塞。 */
    void cmdApplyGgaGate(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    /** @brief 处理 GGA 质量门控配置命令并安排去抖落盘。 @param cmd: 门控配置命令。 @param nowMs: 当前时钟。 @param[out] result: 执行结果。 @note 仅 kIo。 */
    void cmdSetGgaGate(const fw::Command &cmd, uint32_t nowMs, fw::CommandResult &result) noexcept;
    /**
     * @brief 推进 GGA 质量门控配置的去抖 NVS 保存与失败重试
     * @param nowMs: 当前时钟（ms）
     * @param forceImmediate: true 时跳过 500 ms 去抖与退避立即尝试写盘（供网页退出前确认持久化）
     * @note 仅 kIo，写 NVS 可阻塞。
     */
    void tickGgaGateSave(uint32_t nowMs, bool forceImmediate) noexcept;

    /** @brief 发布实际 GNSS 待写状态，包含 GGA 写入失败后的退避。 @note 仅 kIo，非 ISR。 */
    void publishPendingNvs() noexcept;
    /** @brief 将 GGA 质量门控状态快照与放行原子标志发布到 DataCenter。 @note 仅 kIo 或启动初始化。 */
    void publishGgaGateStatus() noexcept;

    std::optional<::gnss::GgaQualityGate> gga_gate_{};
    ::gnss::GgaGateConfig gga_gate_cfg_{};
    bool gga_gate_enabled_loaded_ = false;
    bool gga_gate_min_sats_loaded_ = false;
    bool gga_gate_save_pending_ = false;
    uint32_t gga_gate_save_changed_ms_ = 0U;
    uint32_t gga_gate_save_attempt_ms_ = 0U;
    uint32_t gga_gate_save_retry_ms_ = 0U;
    bool restoredSettings_ = false;
    uint32_t applyTicketSeq_ = 0U;
    uint32_t maintenanceTicketSeq_ = 0U;
    uint32_t maintenanceTicketTarget_ = 0U;

    ::gnss::PppSequence ppp_seq_{};
    uint32_t ppp_ticket_seq_ = 0U;
    bool ppp_target_enabled_ = false;
    bool ppp_replay_ = false;
    ::gnss::SatMaskSequence sat_seq_{};
    uint32_t sat_ticket_seq_ = 0U;
    core::SatTrackConfig sat_target_cfg_{};
    core::SatTrackConfig sat_prev_cfg_{};
    bool sat_replay_ = false;
    bool sat_rollback_in_flight_ = false;
    bool sat_replay_forced_gps_ = false;
    ::gnss::ublox::ValsetTracker valset_tracker_{};
    RtcmTxFramer rtcm_framer_{};
    bool rtcm_frame_stall_armed_ = false;
    uint32_t rtcm_frame_stall_ms_ = 0U;
    bool uniqid_polled_ = false;

    ::gnss::Maintenance active_maintenance_ = ::gnss::Maintenance::kSave;
    uint32_t completed_maintenance_seq_ = 0U;
    uint32_t console_maintenance_seq_ = 0U;
    ::gnss::Maintenance console_maintenance_ = ::gnss::Maintenance::kSave;
    ::gnss::MaintenanceResult console_maintenance_result_ = ::gnss::MaintenanceResult::kOk;
    uint32_t preferred_baud_ = 0U;
    uint32_t baud_fail_count_ = 0U;
    bool boot_baud_pending_ = false;
    bool boot_baud_in_flight_ = false;
    bool boot_baud_save_in_flight_ = false;
    bool baud_save_in_flight_ = false;
    uint32_t requested_baud_ = 0U;
    bool auto_save_in_flight_ = false;
    bool apply_deferred_ = false;
    bool baud_deferred_ = false;
    bool skip_hint_save_ = false;
    PendingApply pending_apply_ = PendingApply::kNone;
    GnssApply last_apply_ = GnssApply::kNone;
    uint32_t apply_seq_ = 0U;
    data::GnssOutConfig pending_prev_out_{};
    uint8_t pending_prev_rate_ = 0U;
    bool pending_rate_out_aligned_ = false;
    uint32_t pending_seq_ = 0U;
    uint32_t pending_deadline_ms_ = 0U;
    std::array<data::GnssPushGate, data::kGnssAppMsgCount> push_gates_{};
    std::array<const demux::Decoder *, demux::kMaxDecoders> decoders_{};
    size_t decoder_count_ = 0U;
    std::span<uint8_t> demux_buf_{};
    std::array<uint8_t, kRtcmTxChunkBytes> rtcm_tx_buf_{};
    size_t rtcm_tx_len_ = 0U;
    size_t rtcm_tx_off_ = 0U;
    std::array<uint32_t, kMaxBaudCandidates> bauds_{};
    size_t baud_count_ = 0U;

    uint32_t host_baud_ = 0U;
    uint32_t uart_rx_bytes_ = 0U;
    uint32_t uart_tx_bytes_ = 0U;
    size_t last_rx_bytes_ = 0U;
    uint32_t last_status_ms_ = 0U;
    ::gnss::State last_state_ = ::gnss::State::kOff;
    ::gnss::State last_published_state_ = ::gnss::State::kOff;
    uint32_t step_idx_ = 0U;
    uint32_t step_retry_ = 0U;
    bool step_sent_once_ = false;
    bool manual_reprobe_ = false;
    bool rate_loaded_ = false;
    bool gsv_loaded_ = false;
    bool initialized_ = false;
    bool uart_installed_ = false;
};

/**
 * @brief 取得GNSS命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> gnssCmds() noexcept;
/**
 * @brief 取得GNSS子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> gnssVerbs() noexcept;

} // namespace app
