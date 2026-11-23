// P1 Rev B 主机回归：真实 BtLinkModule（CONFIG_RTK_GNSS_ENABLE=1）连接即推 NMEA、Nav 首帧关闭、
// Survey interval=0 query 不关闭、ALARM 不推、断连清零重连恢复与 supportedMask 交集。
// 线程：仅主机单线程测试，替换 SPP/NVS/RTOS 边界，不依赖硬件。

#include "AppContext.hpp"
#include "SppServer.h"
#include "SppServerPairing.h"
#include "NvsStore.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "proto/Frame.hpp"
#include "rtos/EspStatus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

namespace app {
namespace {
constinit AppContext s_stream_app{};
} // namespace

AppContext &appContext() noexcept
{
    return s_stream_app;
}

bool AppContext::awaitBoot(BootTask, bool) noexcept
{
    return true;
}

fw::Result GnssModule::loadBaseAt(size_t, std::span<const uint8_t>) noexcept
{
    return fw::Result::kOk;
}

fw::Result GnssModule::saveBaseAt(size_t, std::span<uint8_t>, size_t &len) noexcept
{
    len = 0U;
    return fw::Result::kOk;
}
} // namespace app

namespace {

uint32_t s_clock = 1000U;
int s_failures = 0;

struct FakeTxRecord {
    uint32_t handle = 0U;
    bool isStream = false;
    std::array<uint8_t, 512U> bytes{};
    size_t len = 0U;
};

std::array<FakeTxRecord, 64U> s_tx_log{};
size_t s_tx_count = 0U;
uint32_t s_tx_serial = 1U;
std::array<bool, 2U> s_stream_bound{};
std::array<uint32_t, 2U> s_bound_handles{};
std::array<std::array<uint8_t, 1024U>, 2U> s_rx_buf{};
std::array<size_t, 2U> s_rx_size{};
std::array<size_t, 2U> s_rx_ofs{};
std::array<spp_server_evt_t, 32U> s_events{};
size_t s_evt_size = 0U;
size_t s_evt_ofs = 0U;
uint32_t s_rx_wake_calls = 0U;

std::array<uint8_t, app::kGnssAppRingBytes> s_app_ring_buf{};
std::array<uint8_t, app::data::kGnssLatestTotalBytes> s_latest_buf{};

constexpr core::BtAddr kPhoneAddr{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
constexpr core::BtAddr kAlarmAddr{{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
constexpr uint32_t kDefaultNmeaMask = 0x79U; // GGA(0)|GST(3)|RMC(4)|GSA(5)|GSV(6)

/** @brief 断言辅助，记录失败行号与表达式。
 *  @param cond: 期望为真的条件。
 *  @param expr: 表达式文本。
 *  @param line: 行号。 */
void checkImpl(bool cond, const char *expr, int line) noexcept
{
    if (!cond) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++s_failures;
    }
}

#define CHECK(expr) checkImpl((expr), #expr, __LINE__)

/** @brief 按句柄映射到 2 槽测试下标。
 *  @param handle: SPP 句柄。
 *  @retval 0 或 1；非法句柄返回 0。 */
size_t slotOf(uint32_t handle) noexcept
{
    return (handle == 2U) ? 1U : 0U;
}

/** @brief 重置所有伪 SPP 事件与收发记录。 */
void resetFakeSpp() noexcept
{
    s_tx_count = 0U;
    s_tx_serial = 1U;
    s_stream_bound.fill(false);
    s_bound_handles.fill(0U);
    s_rx_size.fill(0U);
    s_rx_ofs.fill(0U);
    s_evt_size = 0U;
    s_evt_ofs = 0U;
    s_rx_wake_calls = 0U;
}

/** @brief 投递一个 SPP 事件到测试队列。
 *  @param evt: 待投递事件。 */
void pushEvent(const spp_server_evt_t &evt) noexcept
{
    if (s_evt_size < s_events.size()) {
        s_events[s_evt_size++] = evt;
    }
}

/** @brief 投递 OPEN 事件。
 *  @param handle: 会话句柄。
 *  @param addr: 对端蓝牙地址。 */
void pushOpen(uint32_t handle, const core::BtAddr &addr) noexcept
{
    spp_server_evt_t evt{};
    evt.type = SPP_SERVER_EVT_OPEN;
    evt.handle = handle;
    std::memcpy(evt.addr, addr.bytes.data(), core::kAddrLen);
    pushEvent(evt);
}

/** @brief 投递 CLOSE 事件。
 *  @param handle: 会话句柄。 */
void pushClose(uint32_t handle) noexcept
{
    spp_server_evt_t evt{};
    evt.type = SPP_SERVER_EVT_CLOSE;
    evt.handle = handle;
    pushEvent(evt);
}

/** @brief 组装一帧协议请求并喂入指定会话的 RX 缓冲。
 *  @param handle: 会话句柄。
 *  @param msgId: 消息 ID。
 *  @param sender: sender 字段（0x00=Nav，0x02=Survey，0x03=Alarm）。
 *  @param type: 帧类型（QUERY/SET/PUSH）。
 *  @param interval: 头部第 19 字节（订阅间隔）。
 *  @param payload: 负载字节视图。 */
void pushFrame(uint32_t handle, uint16_t msgId, uint8_t sender, uint8_t type, int8_t interval,
               std::span<const uint8_t> payload = {}) noexcept
{
    std::array<uint8_t, proto::kMaxFrame> wire{};
    size_t len = 0U;
    CHECK(fw::isOk(proto::buildFrame(wire, msgId, sender, type, payload, len)));
    wire[proto::kOfsInterval] = static_cast<uint8_t>(interval);
    // 修改 interval 后重算帧尾 CRC-32。
    const size_t crcOfs = len - proto::kCrcLen;
    const uint32_t crc = proto::crc32(std::span<const uint8_t>{wire.data(), crcOfs});
    for (size_t i = 0U; i < proto::kCrcLen; ++i) {
        wire[crcOfs + i] = static_cast<uint8_t>((crc >> (i * 8U)) & 0xFFU);
    }
    const size_t idx = slotOf(handle);
    if (s_rx_ofs[idx] == s_rx_size[idx]) {
        s_rx_ofs[idx] = 0U;
        s_rx_size[idx] = 0U;
    }
    CHECK((s_rx_size[idx] + len) <= s_rx_buf[idx].size());
    std::memcpy(s_rx_buf[idx].data() + s_rx_size[idx], wire.data(), len);
    s_rx_size[idx] += len;
    spp_server_evt_t evt{};
    evt.type = SPP_SERVER_EVT_DATA;
    evt.handle = handle;
    pushEvent(evt);
}

/** @brief 推进虚拟时钟并执行一轮 wait(0) + poll()。
 *  @param stepMs: 推进毫秒数。 */
void stepLink(uint32_t stepMs = 20U) noexcept
{
    s_clock += stepMs;
    auto &bt = app::appContext().bt_link;
    bt.wait(0U);
    static_cast<void>(bt.poll(s_clock));
}

/** @brief 初始化干净的测试上下文与 BtLinkModule。
 *  @param whitelistAlarm: 是否将 kAlarmAddr 预置到白名单。 */
void initTestApp(bool whitelistAlarm) noexcept
{
    resetFakeSpp();
    auto &ctx = app::appContext();
    ctx.~AppContext();
    ::new (static_cast<void *>(&ctx)) app::AppContext{};
    ctx.bindContexts();
    ctx.data.init();
    CHECK(fw::isOk(ctx.data.initGnssBuffers(s_app_ring_buf, s_latest_buf)));

    app::SystemStatus sys{};
    sys.btReady = true;
    std::memcpy(sys.sn.data(), "1234", 5U);
    ctx.data.systemStatus.write(sys);

    app::LinkStatus link{};
    link.policy = core::Policy::kConcurrent;
    if (whitelistAlarm) {
        link.whitelistCount = 1U;
        link.whitelist[0] = kAlarmAddr;
    }
    ctx.data.linkStatus.write(link);

    CHECK(ctx.bt_link.init() == fw::Result::kOk);
}

/** @brief 统计自起点起发往指定会话的 stream 帧数量。
 *  @param fromIdx: 起始下标。
 *  @param handle: 目标句柄。
 *  @retval 匹配的 stream 发送条数。 */
size_t countStreamTx(size_t fromIdx, uint32_t handle) noexcept
{
    size_t count = 0U;
    for (size_t i = fromIdx; i < s_tx_count; ++i) {
        if (s_tx_log[i].isStream && (s_tx_log[i].handle == handle)) {
            ++count;
        }
    }
    return count;
}

/** @brief 场景 1：PHONE 连接即推 GGA/GST/RMC/GSA/GSV（零请求帧），断连清零，重连恢复。 */
void testConnectImmediatePushAndReconnect() noexcept
{
    initTestApp(false);
    auto &data = app::appContext().data;

    // 未连接前掩码为 0。
    CHECK(data.gnssAppSub.snapshot().activeMask == 0U);

    // PHONE 会话打开，不发送任何请求帧。
    pushOpen(1U, kPhoneAddr);
    stepLink();
    const auto sub1 = data.gnssAppSub.snapshot();
    CHECK(sub1.activeMask == kDefaultNmeaMask);
    for (const uint32_t period : sub1.periodsMs) {
        CHECK(period == 0U);
    }

    // 直接写入 GGA 与 GSV 到 App 环，无需请求帧即可经 SPP stream 推出。
    constexpr std::string_view kGga =
        "$GNGGA,123519.00,4807.038,N,01131.000,E,4,12,0.7,545.4,M,46.9,M,1.0,0000*62\r\n";
    constexpr std::string_view kGsv = "$GPGSV,1,1,04,01,40,083,46,02,17,308,41*74\r\n";
    const size_t txStart = s_tx_count;
    CHECK(data.gnssAppRing.write(
              static_cast<uint8_t>(::gnss::OutputMsg::kGga),
              std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(kGga.data()),
                                       kGga.size()}) == fw::Result::kOk);
    CHECK(data.gnssAppRing.write(
              static_cast<uint8_t>(::gnss::OutputMsg::kGsv),
              std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(kGsv.data()),
                                       kGsv.size()}) == fw::Result::kOk);
    stepLink();
    CHECK(countStreamTx(txStart, 1U) == 2U);
    CHECK(s_tx_log[txStart].len == kGga.size());
    CHECK(std::memcmp(s_tx_log[txStart].bytes.data(), kGga.data(), kGga.size()) == 0);
    CHECK(s_tx_log[txStart + 1U].len == kGsv.size());
    CHECK(std::memcmp(s_tx_log[txStart + 1U].bytes.data(), kGsv.data(), kGsv.size()) == 0);

    // 断开连接：掩码清零、残余清空。
    pushClose(1U);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == 0U);

    // 重连新会话（无任何请求帧）：自动恢复默认直传。
    pushOpen(2U, kPhoneAddr);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);
    const size_t txReconnect = s_tx_count;
    CHECK(data.gnssAppRing.write(
              static_cast<uint8_t>(::gnss::OutputMsg::kGga),
              std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(kGga.data()),
                                       kGga.size()}) == fw::Result::kOk);
    stepLink();
    CHECK(countStreamTx(txReconnect, 2U) == 1U);
}

/** @brief 场景 2：Nav 首帧（sender==0x00）关闭默认直传，后续显式订阅正常生效。 */
void testNavFirstFrameDisablesDefaultPush() noexcept
{
    initTestApp(false);
    auto &data = app::appContext().data;

    pushOpen(1U, kPhoneAddr);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);

    // Nav 发送 0x01 查询帧（sender == 0x00）：默认直传立即关闭。
    pushFrame(1U, 0x0001U, proto::sender::kApp, proto::msg_type::kQuery, 0);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == 0U);

    // Nav 之后显式订阅 0x04（1 Hz）：按原逻辑开启 GGA 订阅。
    pushFrame(1U, 0x0004U, proto::sender::kApp, proto::msg_type::kQuery, 1);
    stepLink();
    const auto sub = data.gnssAppSub.snapshot();
    CHECK(sub.activeMask == (1U << 0U));
    CHECK(sub.periodsMs[0] == 1000U);

    // Nav 发送 0x04 interval=0：关闭显式订阅，掩码回到 0（默认直传保持关闭）。
    pushFrame(1U, 0x0004U, proto::sender::kApp, proto::msg_type::kQuery, 0);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == 0U);

    // 另一种顺序：Nav 连接后的首帧即为 0x04 interval=1（显式订阅），关闭默认直传且只保留 GGA。
    pushClose(1U);
    stepLink();
    pushOpen(2U, kPhoneAddr);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);
    pushFrame(2U, 0x0004U, proto::sender::kApp, proto::msg_type::kQuery, 1);
    stepLink();
    const auto navFirstSub = data.gnssAppSub.snapshot();
    CHECK(navFirstSub.activeMask == (1U << 0U));
    CHECK(navFirstSub.periodsMs[0] == 1000U);
}

/** @brief 场景 3：Survey interval=0 查询不关闭默认直传，显式订阅仍修改周期且不丢已入环 GGA。 */
void testSurveyIntervalZeroKeepsDefaultPush() noexcept
{
    initTestApp(false);
    auto &data = app::appContext().data;

    pushOpen(1U, kPhoneAddr);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);

    // Survey 发送 0x03（电池查询，sender == 0x02）及全部 GNSS 指令 interval=0 query：默认直传保持开启。
    pushFrame(1U, 0x0003U, proto::sender::kPanel, proto::msg_type::kQuery, 0);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);

    for (const uint16_t id : {0x0004U, 0x0005U, 0x0021U, 0x0022U, 0x0023U}) {
        pushFrame(1U, id, proto::sender::kPanel, proto::msg_type::kQuery, 0);
        stepLink();
        CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);
    }

    // 环内先写一条 GGA，再发 Survey 0x04 interval=2（2 Hz）：掩码不变，不丢已入环 GGA，且周期更新为 500 ms。
    constexpr std::string_view kGga =
        "$GNGGA,123520.00,4807.038,N,01131.000,E,4,12,0.7,545.4,M,46.9,M,1.0,0000*68\r\n";
    CHECK(data.gnssAppRing.write(
              static_cast<uint8_t>(::gnss::OutputMsg::kGga),
              std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(kGga.data()),
                                       kGga.size()}) == fw::Result::kOk);
    const size_t txBefore = s_tx_count;
    pushFrame(1U, 0x0004U, proto::sender::kPanel, proto::msg_type::kQuery, 2);
    stepLink();
    const auto sub = data.gnssAppSub.snapshot();
    CHECK(sub.activeMask == kDefaultNmeaMask);
    CHECK(sub.periodsMs[0] == 500U);
    CHECK(countStreamTx(txBefore, 1U) == 1U);

    // 再发 Survey 0x04 interval=0：默认直传依然不关闭，GGA 周期回到 0（沿用模块输出）。
    pushFrame(1U, 0x0004U, proto::sender::kPanel, proto::msg_type::kQuery, 0);
    stepLink();
    const auto subAfter = data.gnssAppSub.snapshot();
    CHECK(subAfter.activeMask == kDefaultNmeaMask);
    CHECK(subAfter.periodsMs[0] == 0U);
}

/** @brief 场景 4：ALARM 会话不推 NMEA；supportedMask 变化时与默认直传掩码取交集。 */
void testAlarmNoPushAndSupportedMaskIntersection() noexcept
{
    // 1. 白名单 ALARM 会话打开：不开启默认直传。
    initTestApp(true);
    auto &data = app::appContext().data;
    pushOpen(1U, kAlarmAddr);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == 0U);

    // 2. 随后 PHONE 打开并发布受限 supportedMask（不含 GST bit 3）：交集生效。
    pushOpen(2U, kPhoneAddr);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);

    app::GnssStatus st{};
    st.enabled = true;
    st.state = ::gnss::State::kRunning;
    st.family = ::gnss::Family::kOptisens;
    st.model = ::gnss::Model::kCm680;
    // 假设接收机不支持 GST(bit 3)：仅支持 GGA(0)|RMC(4)|GSA(5)|GSV(6) = 0x71。
    st.supportedMask = 0x71U;
    data.gnssStatus.write(st);
    stepLink();
    CHECK(data.gnssAppSub.snapshot().activeMask == 0x71U);

    // 3. 配对窗口内未知设备先作为 PHONE 连接，收到 0x41 HELLO 转为 ALARM 后立即关闭默认直传。
    initTestApp(false);
    auto &data2 = app::appContext().data;
    fw::Command pairCmd{};
    pairCmd.target = app::id(app::ModuleKind::kBtLink);
    pairCmd.verb = app::verb(app::LinkVerb::kPair);
    fw::CommandResult pairRes{};
    app::appContext().bt_link.onCommand(pairCmd, pairRes);
    CHECK(pairRes.status == fw::Result::kOk);

    pushOpen(1U, kAlarmAddr);
    stepLink();
    CHECK(data2.gnssAppSub.snapshot().activeMask == kDefaultNmeaMask);
    const std::array<uint8_t, 8U> helloPayload{};
    pushFrame(1U, 0x0041U, proto::sender::kAlarm, proto::msg_type::kPush, 0, helloPayload);
    stepLink();
    CHECK(data2.gnssAppSub.snapshot().activeMask == 0U);
}

/** @brief 场景 5（Q1）：GGA 质量门控阻断时 0x04 query 回 02 02 且直传丢弃 GGA、照常推送 GSV。 */
void testGgaQualityGateQueryAndStream() noexcept
{
    initTestApp(false);
    auto &data = app::appContext().data;

    pushOpen(1U, kPhoneAddr);
    stepLink();

    constexpr std::string_view kGga =
        "$GNGGA,123519.00,4807.038,N,01131.000,E,4,12,0.7,545.4,M,46.9,M,1.0,0000*62\r\n";
    constexpr std::string_view kGsv = "$GPGSV,1,1,04,01,40,083,46,02,17,308,41*74\r\n";
    const std::span<const uint8_t> ggaBytes{reinterpret_cast<const uint8_t *>(kGga.data()),
                                            kGga.size()};
    const std::span<const uint8_t> gsvBytes{reinterpret_cast<const uint8_t *>(kGsv.data()),
                                            kGsv.size()};
    CHECK(data.gnssLatest.update(static_cast<uint8_t>(::gnss::OutputMsg::kGga), ggaBytes));

    // 1. 门控阻断：0x04 query 回 02 02；直传环中的 GGA 被丢弃，GSV 照常推出。
    data.ggaAllowedFlag.store(false, std::memory_order_release);
    const size_t txBeforeBlockStream = s_tx_count;
    CHECK(data.gnssAppRing.write(static_cast<uint8_t>(::gnss::OutputMsg::kGga), ggaBytes) ==
          fw::Result::kOk);
    CHECK(data.gnssAppRing.write(static_cast<uint8_t>(::gnss::OutputMsg::kGsv), gsvBytes) ==
          fw::Result::kOk);
    stepLink();
    CHECK(countStreamTx(txBeforeBlockStream, 1U) == 1U);
    CHECK(s_tx_log[txBeforeBlockStream].len == kGsv.size());
    CHECK(std::memcmp(s_tx_log[txBeforeBlockStream].bytes.data(), kGsv.data(), kGsv.size()) == 0);

    const auto decodeReply = [](const FakeTxRecord &rec, proto::FrameParser &parser,
                                proto::FrameView &view) noexcept -> bool {
        parser.reset();
        for (size_t i = 0U; i < rec.len; ++i) {
            if (parser.feed(rec.bytes[i], view) == proto::ParseResult::kFrame) {
                return true;
            }
        }
        return false;
    };

    const size_t txBeforeBlockedQuery = s_tx_count;
    pushFrame(1U, 0x0004U, proto::sender::kApp, proto::msg_type::kQuery, 0);
    stepLink();
    CHECK(s_tx_count > txBeforeBlockedQuery);
    const auto &blockedReply = s_tx_log[txBeforeBlockedQuery];
    CHECK(!blockedReply.isStream);
    proto::FrameParser parser{};
    proto::FrameView blockedView{};
    CHECK(decodeReply(blockedReply, parser, blockedView));
    CHECK(blockedView.msgId == 0x0004U);
    CHECK(blockedView.payload.size() == 2U);
    CHECK((blockedView.payload[0] == 0x02U) && (blockedView.payload[1] == 0x02U));

    // 2. 门控放行：0x04 query 正常返回最新 GGA。
    data.ggaAllowedFlag.store(true, std::memory_order_release);
    const size_t txBeforeAllowedQuery = s_tx_count;
    pushFrame(1U, 0x0004U, proto::sender::kApp, proto::msg_type::kQuery, 0);
    stepLink();
    CHECK(s_tx_count > txBeforeAllowedQuery);
    const auto &allowedReply = s_tx_log[txBeforeAllowedQuery];
    CHECK(!allowedReply.isStream);
    proto::FrameView allowedView{};
    CHECK(decodeReply(allowedReply, parser, allowedView));
    CHECK(allowedView.msgId == 0x0004U);
    CHECK(allowedView.payload.size() == kGga.size());
    CHECK(std::memcmp(allowedView.payload.data(), kGga.data(), kGga.size()) == 0);
}

} // namespace

// -----------------------------------------------------------------------------
// 最小主机桩：替代 SPP、NVS、RTOS 时钟/临界区与日志输出。
// -----------------------------------------------------------------------------
namespace rtos {
uint32_t nowMs() noexcept
{
    return s_clock;
}

fw::Status fromEspErr(esp_err_t error) noexcept
{
    switch (error) {
    case ESP_OK:
        return fw::Status::kOk;
    case ESP_ERR_INVALID_ARG:
        return fw::Status::kInvalidArg;
    case ESP_ERR_INVALID_STATE:
        return fw::Status::kInvalidState;
    case ESP_ERR_NOT_FOUND:
        return fw::Status::kNotFound;
    case ESP_ERR_TIMEOUT:
        return fw::Status::kTimeout;
    case ESP_ERR_NO_MEM:
        return fw::Status::kNoSpace;
    default:
        return fw::Status::kIoError;
    }
}
} // namespace rtos

extern "C" {

void esp_restart(void) {}
uint32_t esp_log_timestamp(void)
{
    return s_clock;
}
const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "OK" : "ERR";
}
esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return nullptr;
}
void spp_server_set_trace_hook(spp_server_trace_fn) {}

bool spp_server_poll_event(spp_server_evt_t *evt, uint32_t)
{
    if (s_evt_ofs >= s_evt_size) {
        return false;
    }
    *evt = s_events[s_evt_ofs++];
    return true;
}

bool spp_server_rx_ready(uint32_t handle)
{
    const size_t idx = slotOf(handle);
    return s_rx_ofs[idx] < s_rx_size[idx];
}

size_t spp_server_read(uint32_t handle, uint8_t *buf, size_t max_len)
{
    const size_t idx = slotOf(handle);
    const size_t avail = s_rx_size[idx] - s_rx_ofs[idx];
    const size_t take = std::min(avail, max_len);
    if (take > 0U) {
        std::memcpy(buf, s_rx_buf[idx].data() + s_rx_ofs[idx], take);
        s_rx_ofs[idx] += take;
    }
    return take;
}

void spp_server_set_rx_wake_hook(spp_server_rx_wake_fn, void *)
{
    ++s_rx_wake_calls;
}

void spp_server_set_pair_query(spp_server_pair_query_t) {}

/** @brief 模拟 GAP 投递结果。 @param paused: 目标扫描模式。 @retval SDK 投递结果。 */
extern "C" esp_err_t spp_server_scan_pause(bool paused)
{
    static_cast<void>(paused);
    return ESP_OK;
}

esp_err_t spp_server_disconnect(uint32_t)
{
    return ESP_OK;
}

esp_err_t spp_server_write(uint32_t handle, const uint8_t *data, size_t len, uint32_t)
{
    if ((s_tx_count < s_tx_log.size()) && (len <= s_tx_log[s_tx_count].bytes.size())) {
        auto &rec = s_tx_log[s_tx_count++];
        rec.handle = handle;
        rec.isStream = false;
        rec.len = len;
        std::memcpy(rec.bytes.data(), data, len);
    }
    return ESP_OK;
}

esp_err_t spp_server_reply_reserve(uint32_t handle, spp_server_tx_token_t *out)
{
    out->generation = handle;
    out->capacity = SPP_SERVER_REPLY_RESERVE;
    out->serial = s_tx_serial++;
    out->slot = static_cast<uint8_t>(slotOf(handle));
    out->channel = 0U;
    return ESP_OK;
}

bool spp_server_reply_ready(uint32_t)
{
    return true;
}

esp_err_t spp_server_stream_bind(uint32_t handle, bool enable)
{
    const size_t idx = slotOf(handle);
    s_stream_bound[idx] = enable;
    s_bound_handles[idx] = enable ? handle : 0U;
    return ESP_OK;
}

esp_err_t spp_server_stream_reserve(uint32_t handle, size_t len, spp_server_tx_token_t *out)
{
    const size_t idx = slotOf(handle);
    if (!s_stream_bound[idx] || (s_bound_handles[idx] != handle)) {
        return ESP_ERR_INVALID_STATE;
    }
    out->generation = handle;
    out->capacity = static_cast<uint16_t>(len);
    out->serial = s_tx_serial++;
    out->slot = static_cast<uint8_t>(idx);
    out->channel = 1U;
    return ESP_OK;
}

esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *token, const uint8_t *data, size_t len)
{
    if ((s_tx_count < s_tx_log.size()) && (len <= s_tx_log[s_tx_count].bytes.size())) {
        auto &rec = s_tx_log[s_tx_count++];
        rec.handle = token->generation;
        rec.isStream = (token->channel == 1U);
        rec.len = len;
        std::memcpy(rec.bytes.data(), data, len);
    }
    return ESP_OK;
}

esp_err_t spp_server_tx_commit2(const spp_server_tx_token_t *token, const uint8_t *a, size_t aLen,
                                const uint8_t *b, size_t bLen)
{
    if ((s_tx_count < s_tx_log.size()) && ((aLen + bLen) <= s_tx_log[s_tx_count].bytes.size())) {
        auto &rec = s_tx_log[s_tx_count++];
        rec.handle = token->generation;
        rec.isStream = (token->channel == 1U);
        rec.len = aLen + bLen;
        std::memcpy(rec.bytes.data(), a, aLen);
        std::memcpy(rec.bytes.data() + aLen, b, bLen);
    }
    return ESP_OK;
}

void spp_server_tx_cancel(const spp_server_tx_token_t *) {}

esp_err_t spp_server_push_latest(uint32_t, const uint8_t *, size_t)
{
    return ESP_OK;
}

void spp_server_tx_stats(spp_server_tx_stats_t *out)
{
    *out = {};
}

void spp_server_rx_stats(spp_server_rx_stats_t *out)
{
    *out = {};
}

esp_err_t nvs_store_init(void)
{
    return ESP_OK;
}
esp_err_t nvs_store_get_sn(char *, size_t)
{
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_store_set_sn(const char *)
{
    return ESP_OK;
}
esp_err_t nvs_store_get_whitelist(uint8_t (*)[6], size_t, size_t *outCount)
{
    *outCount = 0U;
    return ESP_OK;
}
esp_err_t nvs_store_set_whitelist(const uint8_t (*)[6], size_t)
{
    return ESP_OK;
}
esp_err_t nvs_store_get_alarms(uint8_t (*)[6], size_t, size_t *outCount)
{
    *outCount = 0U;
    return ESP_OK;
}
esp_err_t nvs_store_set_alarms(const uint8_t (*)[6], size_t)
{
    return ESP_OK;
}
esp_err_t nvs_store_erase_user(void)
{
    return ESP_OK;
}
esp_err_t nvs_store_erase_factory(void)
{
    return ESP_OK;
}

} // extern "C"

int main()
{
    testConnectImmediatePushAndReconnect();
    testNavFirstFrameDisablesDefaultPush();
    testSurveyIntervalZeroKeepsDefaultPush();
    testAlarmNoPushAndSupportedMaskIntersection();
    testGgaQualityGateQueryAndStream();
    if (s_failures != 0) {
        std::fprintf(stderr, "GnssStreamCheck: %d failure(s)\n", s_failures);
        return 1;
    }
    return 0;
}
