// 存储模块（kLink 执行器）：SN 与报警器白名单的 NVS 读写、去抖重试、目录键表加载与 `config` 查看命令。
// 由 kLink 执行器任务调用；NVS 读写可阻塞，非 ISR 安全。
#pragma once
#include <span>

#include "app/BootConfig.hpp"
#include "data/AppData.hpp"
#include "fw/StoreKey.hpp"
#include "cli/Console.hpp"
#include "NvsStore.h"
#include "PhoneCompatService.hpp"

namespace app {
struct AppContext;
struct ModuleTestAccess;
struct StoreGroup;

/** 存储模块（kLink 执行器）：SN 与白名单的 NVS 持久化与重试。 */
class StorageModule final {
public:
    static constexpr uint8_t kEraseUser = 1U;
    static constexpr uint8_t kEraseFactory = 2U;
    constexpr StorageModule() noexcept = default;
    /**
     * @brief 记录所属上下文，不初始化硬件或 RTOS
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅对象构造时调用，不分配、不阻塞；非 ISR。
     */
    constexpr explicit StorageModule(AppContext &context) noexcept : context_(&context) {}
    /**
     * @brief 绑定模块所属上下文
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅消费者启动前单线程调用；不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~StorageModule() = default;
    StorageModule(const StorageModule &) = delete;
    StorageModule &operator=(const StorageModule &) = delete;
    StorageModule(StorageModule &&) = delete;
    StorageModule &operator=(StorageModule &&) = delete;
    /**
     * @brief  初始化 NVS 并加载 SN、白名单与各模块键表
     * @retval fw::Result::kOk 总是（加载失败使用缺省值）
     * @note   启动前单线程，非 ISR；同步读 NVS，幂等。
     */
    [[nodiscard]] fw::Result init() noexcept;
    /**
     * @brief  处理挂起的白名单保存（去抖与失败重试）
     * @param  nowMs: 当前时间（ms）
     * @retval false 总是（重试由时钟驱动，不需要立即再轮询）
     * @note   仅 kLink，非 ISR；可同步写 NVS；失败记 W/E 级日志。
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;
    /**
     * @brief 仅 kLink，非 ISR；同步保存 SN，结果写入 result。
     * @param cmd: 待处理指令
     * @param[out] result: 执行结果输出
     * @note 仅所属模块执行器任务调用，非 ISR；涉及存储时可同步等待 NVS。
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 仅 kLink，非 ISR、非阻塞；登记白名单变化并重启去抖。
     * @param list: 新白名单，超容量部分忽略
     * @param nowMs: 当前单调时间（毫秒）
     * @note 仅 kLink，不阻塞，非 ISR；重新开始去抖且保留失败退避。
     */
    void markWhitelistDirty(std::span<const core::BtAddr> list, uint32_t nowMs) noexcept;
    /**
     * @brief 仅 kLink，非 ISR；跳过去抖同步写 NVS，返回后端错误。
     * @param nowMs: 当前单调时间（毫秒）
     * @retval 最近一次白名单写入的底层 esp_err_t；无待写时保持旧结果
     * @note 仅 kLink；跳过去抖同步调用 poll，可等待 NVS，非 ISR。
     */
    [[nodiscard]] esp_err_t flushWhitelist(uint32_t nowMs) noexcept;

    /**
     * @brief 逐个加载键表（blob 与标量分派给上面的单键函数）
     * @param keys: 键表
     * @retval 本组已恢复、缺省与加载失败的键数；失败键各记一条 W。
     * @note 仅 storage 任务启动期调用；非 ISR，不分配堆。
     */
    [[nodiscard]] StoreLoadCounts loadKeys(std::span<const fw::StoreKey> keys) noexcept;
    /**
     * @brief 逐个保存键表（blob 与标量分派给上面的单键函数）
     * @param keys: 键表
     * @retval kOk: 全部成功；其他: 最后一个失败键的类型（既有语义）
     * @note 仅 storage 任务调用；非 ISR，不分配堆。
     */
    [[nodiscard]] fw::Result saveKeys(std::span<const fw::StoreKey> keys) noexcept;

    /** @brief 读取当前天线高。 @retval 小端字节。 @note 仅 kLink。 */
    [[nodiscard]] core::AntennaHeight phoneHeight() const noexcept
    {
        return phone_height_;
    }

    /** @brief 登记已校验天线高。 @param height: 小端字节。 @param nowMs: 去抖起点。 @retval true 已登记。 @note 仅 kLink。 */
    [[nodiscard]] bool setPhoneHeight(const core::AntennaHeight &height, uint32_t nowMs) noexcept;

    /** @brief 读取当前 SN 与 PN（[0..15]SN [16..31]PN）。 @retval 32 B 组合字段。 @note 仅 kLink。 */
    [[nodiscard]] core::PhoneSnPn phoneSnPn() const noexcept;

    /** @brief 保存 SN（同步复用 kSaveSn 逻辑）并登记去抖写入 PN。 @param snPn: 32 B 组合字段。 @param nowMs: 去抖起点。 @retval true 保存成功。 @note 仅 kLink，写 SN 可阻塞。 */
    [[nodiscard]] bool setPhoneSnPn(const core::PhoneSnPn &snPn, uint32_t nowMs) noexcept;

    /** @brief 读取当前 NTRIP 配置（0x12）。 @retval 112 B 配置块。 @note 仅 kLink。 */
    [[nodiscard]] core::PhoneNtripConfig phoneNtrip() const noexcept
    {
        return phone_ntrip_;
    }

    /** @brief 登记 NTRIP 配置（RAM+NVS 去抖）。 @param cfg: 112 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 @note 仅 kLink。 */
    [[nodiscard]] bool setPhoneNtrip(const core::PhoneNtripConfig &cfg, uint32_t nowMs) noexcept;

    /** @brief 读取当前 JSON 服务器配置（0x32 Nav）。 @retval 42 B 配置块。 @note 仅 kLink。 */
    [[nodiscard]] core::PhoneJsonServerConfig phoneJsonServer() const noexcept
    {
        return phone_json_srv_;
    }

    /** @brief 登记 JSON 服务器配置（RAM+NVS 去抖）。 @param cfg: 42 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 @note 仅 kLink。 */
    [[nodiscard]] bool setPhoneJsonServer(const core::PhoneJsonServerConfig &cfg,
                                          uint32_t nowMs) noexcept;

    /** @brief 读取当前 Thingsboard 配置（0x33 Nav）。 @retval 86 B 配置块。 @note 仅 kLink。 */
    [[nodiscard]] core::PhoneThingsboardConfig phoneThingsboard() const noexcept
    {
        return phone_tb_;
    }

    /** @brief 登记 Thingsboard 配置（RAM+NVS 去抖）。 @param cfg: 86 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 @note 仅 kLink。 */
    [[nodiscard]] bool setPhoneThingsboard(const core::PhoneThingsboardConfig &cfg,
                                           uint32_t nowMs) noexcept;

    /** @brief 从 NVS 恢复天线高。 @param in: 小端字节。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhoneHeight(void *, std::span<const uint8_t> in) noexcept;

    /** @brief 从 NVS 恢复 PPP。 @param in: 开关字节。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhonePpp(void *, std::span<const uint8_t> in) noexcept;

    /** @brief 从 NVS 恢复 0x06 截止角/星座配置。 @param in: 6 B 编码。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhoneSatCfg(void *, std::span<const uint8_t> in) noexcept;

    /** @brief 从 NVS 恢复 0x08 PN。 @param in: 16 B 定长字节。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhonePn(void *, std::span<const uint8_t> in) noexcept;

    /** @brief 从 NVS 恢复 0x12 NTRIP 配置。 @param in: 112 B 配置块。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhoneNtrip(void *, std::span<const uint8_t> in) noexcept;

    /** @brief 从 NVS 恢复 0x32 JSON 服务器配置。 @param in: 42 B 配置块。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhoneJsonSrv(void *, std::span<const uint8_t> in) noexcept;

    /** @brief 从 NVS 恢复 0x33 Nav Thingsboard 配置。 @param in: 86 B 配置块。 @retval 加载结果。 @note 仅启动。 */
    [[nodiscard]] static fw::Result loadPhoneTb(void *, std::span<const uint8_t> in) noexcept;

private:
    struct DebouncedState {
        bool known = false;
        bool pending = false;
        uint32_t changedMs = 0U;
        uint32_t attemptMs = 0U;
        uint32_t retryMs = 0U;
    };

    /** @brief 更新去抖 Blob 的内存值并启动 500 ms 去抖计时。 @param dst: 模块内目标缓冲。 @param state: 去抖状态。 @param value: 新字节内容。 @param nowMs: 当前时钟。 @retval true 已登记。 */
    [[nodiscard]] bool setPhoneBlob(std::span<uint8_t> dst, DebouncedState &state,
                                    std::span<const uint8_t> value, uint32_t nowMs) noexcept;

    /** @brief 按去抖与退避窗口保存一个手机 Blob 键。 @param key: NVS 键定义。 @param data: 待写字节。 @param state: 去抖状态。 @param nowMs: 当前时钟。 @param label: 日志名称。 */
    void pollPhoneBlob(const fw::StoreKey &key, std::span<const uint8_t> data,
                       DebouncedState &state, uint32_t nowMs, const char *label) noexcept;

    /** @brief 推进全部手机 Blob 键的去抖写盘。 @param nowMs: 当前单调时钟。 @note 仅 kLink。 */
    void pollPhoneBlobs(uint32_t nowMs) noexcept;

    core::AntennaHeight phone_height_{};
    DebouncedState phone_height_state_{};
    core::PhonePn phone_pn_{};
    DebouncedState phone_pn_state_{};
    core::PhoneNtripConfig phone_ntrip_{};
    DebouncedState phone_ntrip_state_{};
    core::PhoneJsonServerConfig phone_json_srv_{};
    DebouncedState phone_json_srv_state_{};
    core::PhoneThingsboardConfig phone_tb_{};
    DebouncedState phone_tb_state_{};
    /** @brief 排他擦除已确认的分区。 @param cmd: 擦除范围。 @param result: 返回操作结果。 @note 仅 kLink，可阻塞。 */
    void eraseForReset(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    friend struct ModuleTestAccess;
    /**
     * @brief  从 NVS 读白名单；缺失或损坏时使用空表并记 W 级日志
     */
    void loadWhitelist() noexcept;
    /**
     * @brief  从 NVS 读 SN；缺失时由 BT MAC 派生设备名
     */
    void loadSn() noexcept;
    /**
     * @brief 按持久化目录加载各模块键表
     * @note 仅启动任务；可等待 NVS，单类失败不阻止后续加载，非 ISR。
     */
    void loadKeyTables() noexcept;
    /** @brief 加载本组的后期键并合并早期统计。 @param group: 目录组。 @retval 启动加载计数。 */
    [[nodiscard]] StoreLoadCounts loadBootGroup(const StoreGroup &group) noexcept;
    /**
     * @brief 从连续六字节地址行恢复模块白名单
     * @param in: 输入字节视图
     * @retval kOk 已更新白名单
     * @retval kInvalidArg 长度不是六字节整数倍或超容量，旧值不变
     * @note 仅启动期或 kLink 串行调用，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result whitelistLoad(std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把模块白名单编码为连续地址行
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 始终写入所需字节数
     * @retval kOk 已写入 len 字节
     * @retval kNoSpace 输出不足，len 为所需长度
     * @note 仅 kLink 或启动前调用，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result whitelistSave(std::span<uint8_t> out, size_t &len) const noexcept;
    /**
     * @brief 校验并恢复模块内的序列号
     * @param in: 输入字节视图
     * @retval kOk 已更新模块 SN
     * @retval kInvalidArg 空值、过长、含零字节或换行，旧值不变
     * @note 仅 kLink 或启动前串行调用，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result snLoad(std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把模块序列号编码为不带终止符的字节串
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 始终写入所需字节数
     * @retval kOk 已写入 len 字节
     * @retval kNoSpace 输出不足，len 为所需长度
     * @note 仅 kLink 或启动前调用，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result snSave(std::span<uint8_t> out, size_t &len) const noexcept;
    /**
     * @brief  执行 SN 写入命令：校验后写 NVS，失败保留旧值
     * @param  cmd: 命令（载荷为 SN 文本）
     * @param[out] result: 执行结果
     */
    void saveSn(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    /**
     * @brief 取得已绑定的应用上下文
     * @retval 模块所属上下文引用
     * @note 仅模块所属任务调用；须已 bindContext，非 ISR，不阻塞。
     */
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    static constexpr uint32_t kRetryInitialMs = 1000;
    AppContext *context_ = nullptr;
    bool initialized_ = false;
    bool pending_ = false;
    bool attempted_ = false;
    uint32_t changed_ms_ = 0;
    uint32_t attempt_ms_ = 0;
    uint32_t retry_ms_ = 0U;
    StoreStatus status_{};
    LinkStatus whitelist_{};
    std::array<char, NVS_STORE_SN_MAX + 1U> sn_{};
};
/**
 * @brief 取得配置命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> configCmds() noexcept;
} // namespace app
