// P2 SH1106 128x64 OLED 驱动（共享板级 I2C0 总线，400 kHz，列偏移 2，按 8 页增量写出）。
// init 仅启动期调用；flush/powerOff/wake 仅 OLED 低优先级任务调用；I2C 事务按页持锁，非 ISR。
// 堆档位：启动期一次性（驱动本身不分配堆；IDF i2c_master_bus_add_device 仅在启动期一次性内部分配）。
#pragma once

#include <array>
#include <cstdint>

#include "driver/i2c_master.h"

#include "WouoUI_graph.h"
#include "board/BoardProfile.hpp"
#include "fw/Status.hpp"
#include "modules/oled/OledUi.hpp"

namespace app::board {

/** SH1106 一次刷屏尝试的结论。 */
enum class Sh1106FlushResult : uint8_t {
    kDisabled = 0, // 面板不存在或已关机断电（全零默认初值，落 .bss）
    kWritten,      // 至少写出一个脏页且全部成功
    kSkipped,      // 8 页内容均未变化或处于错误退避期，零 I2C 事务
    kError,        // 写出失败（已中止本轮剩余页并累加错误计数，未更新失败页摘要）
};

/**
 * SH1106 128×64 I2C OLED 驱动（132 列 RAM，偏移 2 列）。
 * 仅在板型含 Feature::kOled 时初始化；每页单独获取/释放共享 I2C 互斥锁，避免阻塞充电检测。
 */
class Sh1106 final {
public:
    static constexpr uint32_t kI2cFreqHz = 400000U; // Hz，SH1106 400 kHz 快速模式
    static constexpr int kI2cXferTimeoutMs =
        8; // ms，单页 130 B @ 400 kHz 约 3 ms；双次 < 20 ms 互斥等待
    static constexpr uint32_t kPowerSettleMs = 100U;     // ms，SH1106 上电稳定延时
    static constexpr uint8_t kColumnOffset = 2U;         // SH1106 132 列 RAM 居中偏移
    static constexpr size_t kPageTxBytes = 1U + 128U;    // 控制字节 0x40 + 128 列像素
    static constexpr uint8_t kMaxErrorBackoffTicks = 4U; // 连续失败最大退避轮数（5 Hz 下约 1 s）

    constexpr Sh1106() noexcept = default;
    ~Sh1106() = default;
    Sh1106(const Sh1106 &) = delete;
    Sh1106 &operator=(const Sh1106 &) = delete;
    Sh1106(Sh1106 &&) = delete;
    Sh1106 &operator=(Sh1106 &&) = delete;

    /**
     * @brief 使能 OLED 电源脚、探测 0x3C 并初始化 SH1106（无应答仅报一行 W 并断电禁用）
     * @param pins: 板级 OLED 引脚、地址与旋转配置
     * @retval kOk 面板初始化完成并已亮屏
     * @retval kNotFound 本板无 OLED 配置或 0x3C 无应答
     * @retval kInvalidArg 引脚或地址配置非法
     * @retval 其他 总线或初始化序列失败错误码
     * @note 仅启动任务调用；含 100 ms 上电延时，非 ISR。
     */
    [[nodiscard]] fw::Status init(const OledPins &pins) noexcept;

    /**
     * @brief 按页对比摘要，仅将发生变化的页经 I2C 写出到 SH1106（每页独立加解锁）
     * @param fb: 8×128 单色帧缓冲（WouoUI ScreenBuff）
     * @param forceFull: true 时跳过摘要对比，强制写出全部 8 页
     * @retval kWritten 至少写出一页且无错误
     * @retval kSkipped 内容完全未变，未发起任何 I2C 写操作
     * @retval kError 至少一页写失败
     * @retval kDisabled 面板未就绪或已断电
     * @note 仅 OLED 任务调用，每页最多阻塞约 3 ms + 锁等待，非 ISR。
     */
    [[nodiscard]] Sh1106FlushResult flush(const ScreenBuff &fb, bool forceFull = false) noexcept;

    /**
     * @brief 关机前关闭 SH1106 显示与电荷泵并拉低电源脚（GPIO12）
     * @note 仅 OLED 任务调用；幂等，非 ISR。
     */
    void powerOff() noexcept;

    /**
     * @brief 关机超时回退后重新拉高电源脚并初始化面板
     * @retval kOk 已重新点亮
     * @retval kInvalidState 面板原本不存在
     * @retval 其他 重新初始化失败
     * @note 仅 OLED 任务调用；含 100 ms 上电延时，非 ISR。
     */
    [[nodiscard]] fw::Status wakeFromPowerOff() noexcept;

    /**
     * @brief 查询面板是否探测在线
     * @retval true 面板存在
     * @retval false 未初始化或探测离线
     * @note 纯查询，非 ISR。
     */
    [[nodiscard]] bool present() const noexcept
    {
        return present_;
    }

    /**
     * @brief 查询面板是否处于关机断电态
     * @retval true 已关闭面板与电源脚
     * @retval false 未断电
     * @note 纯查询，非 ISR。
     */
    [[nodiscard]] bool poweredOff() const noexcept
    {
        return poweredOff_;
    }

    /**
     * @brief 取得累计 I2C 错误次数
     * @retval 错误次数
     * @note 纯查询，非 ISR。
     */
    [[nodiscard]] uint32_t i2cErrors() const noexcept
    {
        return i2cErrors_;
    }

private:
    /**
     * @brief 配置并驱动 OLED 电源使能脚
     * @param high: true 上电，false 断电
     * @retval kOk 成功或未配置电源脚
     * @retval 其他 GPIO 配置/写电平错误
     * @note 仅 init/powerOff/wake 调用，非 ISR。
     */
    [[nodiscard]] fw::Status drivePowerPin(bool high) noexcept;

    /**
     * @brief 探测 SH1106 地址并向共享总线挂载设备句柄
     * @retval kOk 探测应答并成功挂载设备
     * @retval kNotFound 0x3C 无应答（已记一行 W）
     * @retval 其他 总线或挂载失败（已记一行 W）
     * @note 仅 init 调用，非 ISR。
     */
    [[nodiscard]] fw::Status probeAndAttach() noexcept;

    /**
     * @brief 发送 SH1106 初始化命令序列、清屏并开显示
     * @retval kOk 全部命令发送成功
     * @retval 其他 I2C 错误映射码
     * @note 仅 init/wakeFromPowerOff 调用，非 ISR。
     */
    [[nodiscard]] fw::Status configurePanel() noexcept;

    /**
     * @brief 写出单个 128 字节显示页（内部获取并释放一次 I2C 总线锁）
     * @param page: 页索引（0..7）
     * @param data: 128 字节列数据
     * @retval kOk 写出成功
     * @retval kBusy 获取总线锁超时
     * @retval 其他 I2C 错误映射码
     * @note 仅 flush/configurePanel 调用，非 ISR。
     */
    [[nodiscard]] fw::Status writePage(uint8_t page,
                                       std::span<const uint8_t, oled::kWidth> data) noexcept;

    /**
     * @brief 传输超时且释放旧锁后复位共享 I2C 总线，避免 SDA 卡死
     * @param status: 刚完成事务的状态码，仅 kTimeout 时触发复位
     * @note 调用方不得持有 i2c::BusLock，非 ISR。
     */
    static void recoverBusOnTimeout(fw::Status status) noexcept;

    // 全部成员全零默认初值，确保静态实例 s_panel 落入 .dram0.bss（ROL1-04）。
    int i2cPort_ = 0;
    int sdaGpio_ = 0;
    int sclGpio_ = 0;
    int powerGpio_ = 0;
    uint8_t addr_ = 0U;
    bool rotated180_ = false;
    bool hasPowerGpio_ = false;
    i2c_master_dev_handle_t dev_ = nullptr;
    oled::FrameTracker tracker_{};
    std::array<uint8_t, kPageTxBytes> pageTxBuf_{};
    bool powerPinConfigured_ = false;
    bool present_ = false;
    bool poweredOff_ = false;
    uint8_t consecutiveErrors_ = 0U;
    uint8_t backoffSkips_ = 0U;
    uint32_t i2cErrors_ = 0U;
};

} // namespace app::board
