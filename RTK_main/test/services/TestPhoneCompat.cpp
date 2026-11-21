// 手机兼容路由（Nav/Survey 双模式）与通用延迟应答回归。仅主机单线程运行。
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "PhoneCompatService.hpp"
#include "core/LinkService.hpp"
#include "core/services/DeviceService.hpp"
#include "unity.h"

namespace {

constexpr uint16_t kSatTrack = 0x06U;
constexpr uint16_t kSnPn = 0x08U;
constexpr uint16_t kGsmInfo = 0x09U;
constexpr uint16_t kGsv = 0x0AU;
constexpr uint16_t kMountpoints = 0x11U;
constexpr uint16_t kNtrip = 0x12U;
constexpr uint16_t kReserved13 = 0x13U;
constexpr uint16_t kOta = 0x1DU;
constexpr uint16_t kReset = 0x1EU;
constexpr uint16_t kHeight = 0x24U;
constexpr uint16_t kIns = 0x25U;
constexpr uint16_t kAlign = 0x26U;
constexpr uint16_t kHandNet = 0x27U;
constexpr uint16_t kWifiCtl = 0x28U;
constexpr uint16_t kBaseInfo = 0x29U;
constexpr uint16_t kJsonServer = 0x32U;
constexpr uint16_t kPppOrTb = 0x33U;
constexpr uint16_t kFenceList = 0x60U;
constexpr uint16_t kFenceDetail = 0x61U;
constexpr uint16_t kFenceEdit = 0x62U;

constexpr uint8_t kNav = proto::sender::kApp;
constexpr uint8_t kSurvey = proto::sender::kPanel;
constexpr auto kQuery = proto::msg_type::kQuery;
constexpr auto kSet = proto::msg_type::kSet;
constexpr auto kQueryRes = proto::msg_type::kQueryRes;
constexpr auto kSetRes = proto::msg_type::kSetRes;

constexpr core::AntennaHeight kOneAndHalf{0, 0, 0, 0, 0, 0, 0xF8, 0x3F};
constexpr std::array<uint8_t, 2U> kOk{1U, 1U};
constexpr std::array<uint8_t, 2U> kFail{2U, 2U};
constexpr std::array<uint8_t, 2U> kZero2{};
constexpr size_t kSnPnReplyLen = 48U;
constexpr size_t kGsmReplyLen = 96U;
constexpr size_t kReserved13Len = 132U;
constexpr size_t kHeightReplyLen = 16U;
constexpr size_t kAlignReplyLen = 4U;
constexpr size_t kPppReplyLen = 4U;
constexpr size_t kMountpointLen = 4U;
constexpr size_t kResetLen = 4U;

struct Store {
    core::AntennaHeight height{};
    std::optional<bool> heightDone{};
    unsigned heightRequests = 0U;
    unsigned heightCancels = 0U;
    bool pppSupported = true;
    bool ppp = false;
    std::optional<bool> pppDone{};
    unsigned pppRequests = 0U;
    unsigned pppCancels = 0U;
    uint8_t satCapsMask = core::kSatCapAll;
    core::SatTrackConfig sat{};
    std::optional<bool> satDone{};
    unsigned satRequests = 0U;
    unsigned satCancels = 0U;
    core::PhoneSnPn snPn{};
    std::optional<bool> snPnDone{};
    unsigned snPnRequests = 0U;
    unsigned snPnCancels = 0U;
    std::string_view gsv{};
    core::PhoneNtripConfig ntrip{};
    core::PhoneJsonServerConfig jsonSrv{};
    core::PhoneThingsboardConfig tb{};
    unsigned resetCalls = 0U;
    bool lastFactory = false;
    bool lastReboot = false;
    uint32_t lastResetNow = 0U;
    bool wifiSupported = true;
    bool wifiAccept = true;
    unsigned wifiCalls = 0U;
    uint32_t lastWifiNow = 0U;
};

/** @brief 构造手机回调替身。 @param store: 测试状态。 @retval 回调表。 */
core::services::PhoneCompatOps makeOps(Store &store)
{
    return {
        .ctx = &store,
        .readHeight =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->height; },
        .startHeight =
            [](void *ctx, const core::AntennaHeight &height) noexcept {
                auto &state = *static_cast<Store *>(ctx);
                ++state.heightRequests;
                state.height = height;
                state.heightDone.reset();
                return true;
            },
        .heightResult =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->heightDone; },
        .cancelHeight = [](void *ctx) noexcept { ++static_cast<Store *>(ctx)->heightCancels; },
        .pppSupported =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->pppSupported; },
        .readPpp = [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->ppp; },
        .startPpp =
            [](void *ctx, bool enabled) noexcept {
                auto &state = *static_cast<Store *>(ctx);
                ++state.pppRequests;
                state.ppp = enabled;
                state.pppDone.reset();
                return true;
            },
        .pppResult =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->pppDone; },
        .cancelPpp = [](void *ctx) noexcept { ++static_cast<Store *>(ctx)->pppCancels; },
        .satCapsMask =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->satCapsMask; },
        .readSatConfig =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->sat; },
        .startSatConfig =
            [](void *ctx, const core::SatTrackConfig &cfg) noexcept {
                auto &state = *static_cast<Store *>(ctx);
                ++state.satRequests;
                state.sat = cfg;
                state.satDone.reset();
                return true;
            },
        .satConfigResult =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->satDone; },
        .cancelSatConfig = [](void *ctx) noexcept { ++static_cast<Store *>(ctx)->satCancels; },
        .readSnPn = [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->snPn; },
        .startSnPn =
            [](void *ctx, const core::PhoneSnPn &snPn) noexcept {
                auto &state = *static_cast<Store *>(ctx);
                ++state.snPnRequests;
                state.snPn = snPn;
                state.snPnDone.reset();
                return true;
            },
        .snPnResult =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->snPnDone; },
        .cancelSnPn = [](void *ctx) noexcept { ++static_cast<Store *>(ctx)->snPnCancels; },
        .readLatestGsv = [](const void *ctx, std::span<uint8_t> dst) noexcept -> size_t {
            const auto text = static_cast<const Store *>(ctx)->gsv;
            if (text.empty() || (text.size() > dst.size())) {
                return 0U;
            }
            std::memcpy(dst.data(), text.data(), text.size());
            return text.size();
        },
        .readNtrip =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->ntrip; },
        .setNtrip =
            [](void *ctx, const core::PhoneNtripConfig &cfg, uint32_t) noexcept {
                static_cast<Store *>(ctx)->ntrip = cfg;
                return true;
            },
        .readJsonServer =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->jsonSrv; },
        .setJsonServer =
            [](void *ctx, const core::PhoneJsonServerConfig &cfg, uint32_t) noexcept {
                static_cast<Store *>(ctx)->jsonSrv = cfg;
                return true;
            },
        .readThingsboard =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->tb; },
        .setThingsboard =
            [](void *ctx, const core::PhoneThingsboardConfig &cfg, uint32_t) noexcept {
                static_cast<Store *>(ctx)->tb = cfg;
                return true;
            },
        .scheduleReset =
            [](void *ctx, bool factory, bool reboot, uint32_t nowMs) noexcept {
                auto &state = *static_cast<Store *>(ctx);
                ++state.resetCalls;
                state.lastFactory = factory;
                state.lastReboot = reboot;
                state.lastResetNow = nowMs;
                return true;
            },
        .wifiConfigSupported =
            [](const void *ctx) noexcept { return static_cast<const Store *>(ctx)->wifiSupported; },
        .scheduleWifiConfig =
            [](void *ctx, uint32_t nowMs) noexcept {
                auto &state = *static_cast<Store *>(ctx);
                ++state.wifiCalls;
                state.lastWifiNow = nowMs;
                return state.wifiAccept;
            },
    };
}

/** @brief 解码并比较整帧字段。 @param bytes: 应答帧。 @param msg: 消息号。 @param type: 类型。 @param payload: 负载。 */
void checkReply(std::span<const uint8_t> bytes, uint16_t msg, uint8_t type,
                std::span<const uint8_t> payload)
{
    proto::FrameParser parser{};
    proto::FrameView frame{};
    proto::ParseResult result = proto::ParseResult::kNone;
    for (const auto byte : bytes) {
        result = parser.feed(byte, frame);
    }
    TEST_ASSERT_TRUE(result == proto::ParseResult::kFrame);
    TEST_ASSERT_EQUAL_UINT16(msg, frame.msgId);
    TEST_ASSERT_EQUAL_UINT8(type, frame.type);
    TEST_ASSERT_EQUAL_UINT8(proto::sender::kDevice, frame.sender);
    TEST_ASSERT_EQUAL_UINT(payload.size(), frame.payload.size());
    TEST_ASSERT_TRUE(std::ranges::equal(payload, frame.payload));
}

struct RouterFixture {
    Store store{};
    core::services::DeviceService device{};
    core::services::PhoneCompatService phone{};
    core::CommandRouter router{};
    core::Reply reply{};

    RouterFixture()
    {
        phone.ops = makeOps(store);
        device.setSatTrackSurveyHook(&core::services::PhoneCompatService::handleSurveySatTrack,
                                     &phone);
        const std::array tables{device.table(), phone.table()};
        TEST_ASSERT_TRUE(fw::isOk(router.init(tables)));
    }

    void expectReply(uint8_t sender, uint16_t msg, uint8_t req, std::span<const uint8_t> in,
                     uint8_t resType, std::span<const uint8_t> out, uint32_t nowMs = 0U)
    {
        const proto::FrameView frame{.msgId = msg, .sender = sender, .type = req, .payload = in};
        TEST_ASSERT_TRUE(router.handle(core::Role::kPhone, frame, nowMs, reply) ==
                         core::DispatchResult::kReply);
        checkReply(reply.bytes(), msg, resType, out);
    }

    void expectNoReply(uint8_t sender, uint16_t msg, uint8_t req, std::span<const uint8_t> in = {})
    {
        const proto::FrameView frame{.msgId = msg, .sender = sender, .type = req, .payload = in};
        TEST_ASSERT_TRUE(router.handle(core::Role::kPhone, frame, 0U, reply) ==
                         core::DispatchResult::kNoReply);
    }
};

/** @brief 覆盖两类 App 对通用消息（0x09/0x12/0x13/0x24/0x25/0x26/0x27/0x29）与保留 ID 的应答。 */
void testSharedMessagesBothApps()
{
    RouterFixture f;
    for (const uint8_t sender : {kNav, kSurvey}) {
        // 0x09 4G 信息占位
        f.expectReply(sender, kGsmInfo, kQuery, {}, kQueryRes, std::array<uint8_t, kGsmReplyLen>{});
        f.expectReply(sender, kGsmInfo, kSet, {}, kSetRes, kOk);

        // 0x12 NTRIP 配置（112 B 回显）
        core::PhoneNtripConfig ntrip{};
        ntrip[1U] = 1U;
        ntrip[8U] = 'a';
        ntrip[108U] = 2U;
        f.expectReply(sender, kNtrip, kSet, std::span{ntrip}.first(10U), kSetRes, kFail);
        f.expectReply(sender, kNtrip, kSet, ntrip, kSetRes, kOk);
        f.expectReply(sender, kNtrip, kQuery, {}, kQueryRes, ntrip);

        // 0x13 132 B 全 0
        f.expectReply(sender, kReserved13, kQuery, {}, kQueryRes,
                      std::array<uint8_t, kReserved13Len>{});

        // 0x24 天线高（Query 同步返回，合法 Set 走异步 kDeferred）
        std::array<uint8_t, kHeightReplyLen> heightOut{};
        f.store.height = {};
        f.expectReply(sender, kHeight, kQuery, {}, kQueryRes, heightOut);
        f.store.height = kOneAndHalf;
        std::copy(kOneAndHalf.begin(), kOneAndHalf.end(), heightOut.begin());
        f.expectReply(sender, kHeight, kQuery, {}, kQueryRes, heightOut);

        // 0x25 / 0x26 / 0x27 / 0x29
        f.expectReply(sender, kIns, kQuery, {}, kSetRes, kZero2);
        f.expectReply(sender, kAlign, kQuery, {}, kQueryRes, std::array<uint8_t, kAlignReplyLen>{});
        f.expectReply(sender, kAlign, kSet, std::array<uint8_t, 1U>{1U}, kSetRes, kOk);
        f.expectReply(sender, kHandNet, kSet, {}, kSetRes, kZero2);
        f.expectReply(sender, kBaseInfo, kSet, {}, kSetRes, kOk);

        // 0x30/0x34/0x36 仍不回复
        for (const uint16_t msg : std::array<uint16_t, 3U>{0x30U, 0x34U, 0x36U}) {
            f.expectNoReply(sender, msg, kQuery);
        }
    }

    // 0x24 非法天线高（越界/负值/Inf/NaN/短包）
    for (const auto &bad :
         std::array<core::AntennaHeight, 4U>{{{0, 0, 0, 0, 0, 0, 0x26, 0x40},     // 11 m
                                              {0, 0, 0, 0, 0, 0, 0xF0, 0xBF},     // -1 m
                                              {0, 0, 0, 0, 0, 0, 0xF0, 0x7F},     // +Inf
                                              {0, 0, 0, 0, 0, 0, 0xF8, 0x7F}}}) { // NaN
        f.expectReply(kSurvey, kHeight, kSet, bad, kSetRes, kFail);
    }
    f.expectReply(kSurvey, kHeight, kSet, {}, kSetRes, kFail);
    TEST_ASSERT_TRUE(f.router.handle(core::Role::kAlarm, {.msgId = kHeight}, 0U, f.reply) ==
                     core::DispatchResult::kDenied);
}

/** @brief 覆盖 Nav 专属消息（0x08/0x0A/0x11/0x1D/0x1E/0x32/0x60/0x61/0x62）在两类 App 下的行为。 */
void testNavOnlyMessagesBothApps()
{
    RouterFixture f;

    // 0x08 SN/PN（短包同步回 02 02，合法 Set 走异步 kDeferred，Query 回 48 B）
    core::PhoneSnPn snPn{};
    std::memcpy(snPn.data(), "SN123456", 8U);
    std::memcpy(snPn.data() + core::kPhoneSnBytes, "1000190000002000", 16U);
    f.store.snPn = snPn;
    std::array<uint8_t, kSnPnReplyLen> snPnOut{};
    std::copy(snPn.begin(), snPn.end(), snPnOut.begin());
    f.expectReply(kNav, kSnPn, kSet, std::span{snPn}.first(10U), kSetRes, kFail);
    f.expectReply(kNav, kSnPn, kQuery, {}, kQueryRes, snPnOut);
    f.expectNoReply(kSurvey, kSnPn, kQuery);
    f.expectNoReply(kSurvey, kSnPn, kSet, snPn);

    // 0x0A GSV
    f.expectReply(kNav, kGsv, kQuery, {}, kQueryRes, kFail);
    f.store.gsv = "$GPGSV,1,1,01,01,45,120,42*4D\r\n";
    f.expectReply(kNav, kGsv, kQuery, {}, kQueryRes,
                  std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(f.store.gsv.data()),
                                           f.store.gsv.size()});
    f.expectNoReply(kSurvey, kGsv, kQuery);

    // 0x11 挂载点列表
    f.expectReply(kNav, kMountpoints, kQuery, {}, kQueryRes,
                  std::array<uint8_t, kMountpointLen>{1U, 0U, 0U, 0U});
    f.expectNoReply(kSurvey, kMountpoints, kQuery);

    // 0x1D OTA
    f.expectReply(kNav, kOta, kSet, {}, kSetRes, kFail);
    f.expectNoReply(kSurvey, kOta, kSet);

    // 0x1E 恢复/重启
    f.expectReply(kNav, kReset, kSet, std::array<uint8_t, 2U>{1U, 0U}, kSetRes, kFail);
    f.expectReply(kNav, kReset, kSet, std::array<uint8_t, kResetLen>{1U, 0U, 0U, 0U}, kSetRes, kOk,
                  500U);
    TEST_ASSERT_EQUAL_UINT(1U, f.store.resetCalls);
    TEST_ASSERT_TRUE(f.store.lastFactory);
    TEST_ASSERT_FALSE(f.store.lastReboot);
    TEST_ASSERT_EQUAL_UINT32(500U, f.store.lastResetNow);
    f.expectReply(kNav, kReset, kSet, std::array<uint8_t, kResetLen>{0U, 0U, 0U, 1U}, kSetRes, kOk,
                  600U);
    TEST_ASSERT_EQUAL_UINT(2U, f.store.resetCalls);
    TEST_ASSERT_FALSE(f.store.lastFactory);
    TEST_ASSERT_TRUE(f.store.lastReboot);
    f.expectNoReply(kSurvey, kReset, kSet, std::array<uint8_t, kResetLen>{0U, 0U, 0U, 1U});

    // 0x32 JSON 服务器
    core::PhoneJsonServerConfig jsonSrv{};
    jsonSrv[0U] = 'h';
    jsonSrv[36U] = 1U;
    f.expectReply(kNav, kJsonServer, kSet, std::span{jsonSrv}.first(10U), kSetRes, kFail);
    f.expectReply(kNav, kJsonServer, kSet, jsonSrv, kSetRes, kOk);
    f.expectReply(kNav, kJsonServer, kQuery, {}, kQueryRes, jsonSrv);
    f.expectNoReply(kSurvey, kJsonServer, kQuery);

    // 0x60/0x61/0x62 电子围栏
    f.expectReply(kNav, kFenceList, kQuery, {}, kQueryRes, std::array<uint8_t, 1U>{0U});
    f.expectReply(kNav, kFenceDetail, kQuery, {}, kQueryRes, kFail);
    f.expectReply(kNav, kFenceDetail, kSet, {}, kSetRes, kFail);
    f.expectReply(kNav, kFenceEdit, kQuery, {}, kQueryRes, kFail);
    f.expectReply(kNav, kFenceEdit, kSet, {}, kSetRes, kFail);
    f.expectNoReply(kSurvey, kFenceList, kQuery);
    f.expectNoReply(kSurvey, kFenceDetail, kQuery);
    f.expectNoReply(kSurvey, kFenceEdit, kSet);
}

/** @brief 覆盖 0x06 与 0x33 在 Nav 与 Survey 下的双语义及 PPP 不支持分支（不发命令）。 */
void testDualAppMessagesAndPppGate()
{
    RouterFixture f;

    // 0x06：Nav 保持占位（query 12 B 全 0，set 回 01 01）；Survey 返回真实截止角/星座
    f.expectReply(kNav, kSatTrack, kQuery, {}, kQueryRes,
                  std::array<uint8_t, core::kSatTrackLen>{});
    f.expectReply(kNav, kSatTrack, kSet, {}, kSetRes, kOk);

    f.store.sat = {.cutoffDeg = 10U, .gps = 1U, .glo = 0U, .bds = 1U, .gal = 1U, .sbas = 1U};
    std::array<uint8_t, core::kSatTrackLen> expectedSat{};
    expectedSat[1U] = 10U;
    expectedSat[4U] = 1U;
    expectedSat[5U] = 0U;
    expectedSat[6U] = 1U;
    expectedSat[7U] = 1U;
    expectedSat[8U] = 1U;
    f.expectReply(kSurvey, kSatTrack, kQuery, {}, kQueryRes, expectedSat);

    // 0x06 Survey 按 gnssCaps 能力掩码应答：部分能力按位遮蔽；无能力（capsMask==0）时 query 保持旧行为返回存储/默认值，set 仍拒收。
    f.store.satCapsMask = core::kSatCapGps | core::kSatCapBds;
    auto maskedSat = expectedSat;
    maskedSat[7U] = 0U; // GAL 未在能力掩码内
    maskedSat[8U] = 0U; // SBAS 未在能力掩码内
    f.expectReply(kSurvey, kSatTrack, kQuery, {}, kQueryRes, maskedSat);
    f.store.satCapsMask = 0U;
    f.expectReply(kSurvey, kSatTrack, kQuery, {}, kQueryRes, expectedSat);
    f.expectReply(kSurvey, kSatTrack, kSet, expectedSat, kSetRes, kFail);
    TEST_ASSERT_EQUAL_UINT(0U, f.store.satRequests);
    f.store.satCapsMask = core::kSatCapAll;

    // 0x06 Survey 非法截止角与短包直接回 02 02 且不下发命令
    auto badSat = expectedSat;
    badSat[1U] = 91U;
    f.expectReply(kSurvey, kSatTrack, kSet, badSat, kSetRes, kFail);
    f.expectReply(kSurvey, kSatTrack, kSet, std::span{expectedSat}.first(5U), kSetRes, kFail);
    TEST_ASSERT_EQUAL_UINT(0U, f.store.satRequests);

    // 0x33 Nav = Thingsboard（86 B 回显）
    core::PhoneThingsboardConfig tb{};
    tb[0U] = 1U;
    tb[2U] = 'C';
    tb[82U] = 0xBBU;
    f.expectReply(kNav, kPppOrTb, kSet, std::span{tb}.first(10U), kSetRes, kFail);
    f.expectReply(kNav, kPppOrTb, kSet, tb, kSetRes, kOk);
    f.expectReply(kNav, kPppOrTb, kQuery, {}, kQueryRes, tb);

    // 0x33 Survey PPP 不支持（CM680）：query 恒回 0，set 直接回 02 02 且不发任何命令
    f.store.ppp = true;
    f.store.pppSupported = false;
    f.expectReply(kSurvey, kPppOrTb, kQuery, {}, kQueryRes, std::array<uint8_t, kPppReplyLen>{});
    f.expectReply(kSurvey, kPppOrTb, kSet, std::array<uint8_t, 1U>{1U}, kSetRes, kFail);
    TEST_ASSERT_EQUAL_UINT(0U, f.store.pppRequests);

    // 0x33 Survey PPP 支持（UM98x）：query 回 1，非法参数回 02 02
    f.store.pppSupported = true;
    f.expectReply(kSurvey, kPppOrTb, kQuery, {}, kQueryRes,
                  std::array<uint8_t, kPppReplyLen>{1U, 0U, 0U, 0U});
    f.expectReply(kSurvey, kPppOrTb, kSet, {}, kSetRes, kFail);
    f.expectReply(kSurvey, kPppOrTb, kSet, std::array<uint8_t, 1U>{2U}, kSetRes, kFail);
    TEST_ASSERT_EQUAL_UINT(0U, f.store.pppRequests);
}

/** 记录延迟应答及其预留状态的主机传输替身。 */
class ReplyTransport final : public core::Transport {
public:
    /** @brief 兼容接口。 @retval 当前替身只接受额度提交。 */
    fw::Status write(core::SessionHandle, std::span<const uint8_t>, uint32_t) noexcept override
    {
        return fw::Status::kNotSupported;
    }
    /** @brief 接受断开。 @retval kOk。 */
    fw::Status disconnect(core::SessionHandle) noexcept override
    {
        return fw::Status::kOk;
    }
    /** @brief 声明额度支持。 @retval true。 */
    bool supportsCredit() const noexcept override
    {
        return true;
    }
    /** @brief 标记额度。 @retval 是否成功预留。 */
    fw::Status reserveReply(core::SessionHandle, core::ReplyCredit &credit) noexcept override
    {
        if (reservedCount >= 2U) {
            return fw::Status::kBusy;
        }
        ++reservedCount;
        credit.serial = ++nextSerial;
        return fw::Status::kOk;
    }
    /** @brief 提交并记录报文。 @param bytes: 应答。 @retval kOk。 */
    fw::Status commitReply(const core::ReplyCredit &,
                           std::span<const uint8_t> bytes) noexcept override
    {
        TEST_ASSERT_TRUE(reservedCount > 0U);
        --reservedCount;
        len = bytes.size();
        std::copy(bytes.begin(), bytes.end(), frame.begin());
        return fw::Status::kOk;
    }
    /** @brief 释放预留额度。 */
    void cancelReply(const core::ReplyCredit &) noexcept override
    {
        TEST_ASSERT_TRUE(reservedCount > 0U);
        --reservedCount;
        ++cancelCount;
    }

    std::array<uint8_t, proto::kMaxFrame> frame{};
    size_t len = 0U;
    unsigned reservedCount = 0U;
    unsigned cancelCount = 0U;
    uint32_t nextSerial = 0U;
};

/** @brief 验证通用延迟应答（0x33 PPP 与 0x06 卫星配置）正常回包及断开释放 credit。 */
void testDeferredRepliesAndDisconnectCancel()
{
    Store store{};
    core::services::DeviceService device{};
    core::services::PhoneCompatService phone{.ops = makeOps(store)};
    device.setSatTrackSurveyHook(&core::services::PhoneCompatService::handleSurveySatTrack, &phone);
    const std::array tables{device.table(), phone.table()};
    core::CommandRouter router{};
    TEST_ASSERT_TRUE(fw::isOk(router.init(tables)));
    core::LinkManager manager{};
    manager.init(core::Policy::kConcurrent, {});
    core::AlarmSource source{};
    core::services::AlarmService alarm(source, manager, device.info());
    ReplyTransport transport{};
    core::LinkService link(transport, manager, router, alarm);
    constexpr core::SessionHandle kHandle = 1U;
    constexpr core::BtAddr kAddr{{1, 2, 3, 4, 5, 6}};
    link.onOpen(kHandle, kAddr, 0U);

    std::array<uint8_t, proto::kMaxFrame> wire{};
    const auto send = [&](uint16_t msgId, std::span<const uint8_t> payload, uint32_t now) {
        size_t len = 0U;
        TEST_ASSERT_TRUE(fw::isOk(proto::buildFrame(wire, msgId, kSurvey, kSet, payload, len)));
        TEST_ASSERT_EQUAL_UINT(len, link.onData(kHandle, std::span{wire}.first(len), now));
    };

    // 1. Survey 0x33 PPP 延迟应答：OK / FAIL / 断开取消 credit
    send(kPppOrTb, std::array<uint8_t, 1U>{1U}, 1U);
    TEST_ASSERT_EQUAL_UINT(1U, transport.reservedCount);
    link.pollDeferred(2U);
    TEST_ASSERT_EQUAL_UINT(0U, transport.len);
    TEST_ASSERT_EQUAL_UINT(1U, store.pppRequests);
    store.pppDone = true;
    link.pollDeferred(3U);
    TEST_ASSERT_EQUAL_UINT(0U, transport.reservedCount);
    checkReply(std::span{transport.frame}.first(transport.len), kPppOrTb, kSetRes, kOk);

    send(kPppOrTb, std::array<uint8_t, 1U>{0U}, 4U);
    store.pppDone = false;
    link.tick(5U);
    TEST_ASSERT_EQUAL_UINT(0U, transport.reservedCount);
    checkReply(std::span{transport.frame}.first(transport.len), kPppOrTb, kSetRes, kFail);

    // 2. Survey 0x06 截止角/星座延迟应答：OK 与断开取消 credit
    std::array<uint8_t, core::kSatTrackLen> satPayload{};
    satPayload[1U] = 12U;
    satPayload[4U] = 1U;
    satPayload[6U] = 1U;
    send(kSatTrack, satPayload, 6U);
    TEST_ASSERT_EQUAL_UINT(1U, transport.reservedCount);
    TEST_ASSERT_EQUAL_UINT(1U, store.satRequests);
    store.satDone = true;
    link.pollDeferred(7U);
    TEST_ASSERT_EQUAL_UINT(0U, transport.reservedCount);
    checkReply(std::span{transport.frame}.first(transport.len), kSatTrack, kSetRes, kOk);

    // 3. 0x24 天线高与 0x08 Nav SN/PN 经 settings::Service 异步延迟应答
    send(kHeight, kOneAndHalf, 7U);
    TEST_ASSERT_EQUAL_UINT(1U, transport.reservedCount);
    TEST_ASSERT_EQUAL_UINT(1U, store.heightRequests);
    store.heightDone = true;
    link.pollDeferred(7U);
    TEST_ASSERT_EQUAL_UINT(0U, transport.reservedCount);
    checkReply(std::span{transport.frame}.first(transport.len), kHeight, kSetRes, kOk);

    core::PhoneSnPn snPn{};
    std::memcpy(snPn.data(), "SN123456", 8U);
    std::memcpy(snPn.data() + core::kPhoneSnBytes, "1000190000002000", 16U);
    {
        size_t len = 0U;
        TEST_ASSERT_TRUE(fw::isOk(proto::buildFrame(wire, kSnPn, kNav, kSet, snPn, len)));
        TEST_ASSERT_EQUAL_UINT(len, link.onData(kHandle, std::span{wire}.first(len), 7U));
    }
    TEST_ASSERT_EQUAL_UINT(1U, transport.reservedCount);
    TEST_ASSERT_EQUAL_UINT(1U, store.snPnRequests);
    store.snPnDone = true;
    link.pollDeferred(7U);
    TEST_ASSERT_EQUAL_UINT(0U, transport.reservedCount);
    checkReply(std::span{transport.frame}.first(transport.len), kSnPn, kSetRes, kOk);

    // 4. 断开会话时释放所有挂起 credit、调用 cancel 钩子并清理服务等待标志，重连后可继续发起新请求
    send(kPppOrTb, std::array<uint8_t, 1U>{1U}, 8U);
    send(kSatTrack, satPayload, 9U);
    TEST_ASSERT_EQUAL_UINT(2U, transport.reservedCount);
    const unsigned cancelsBefore = transport.cancelCount;
    link.onClose(kHandle);
    TEST_ASSERT_EQUAL_UINT(0U, transport.reservedCount);
    TEST_ASSERT_EQUAL_UINT(cancelsBefore + 2U, transport.cancelCount);
    TEST_ASSERT_EQUAL_UINT(1U, store.pppCancels);
    TEST_ASSERT_EQUAL_UINT(1U, store.satCancels);
    TEST_ASSERT_FALSE(phone.pppWaiting);
    TEST_ASSERT_FALSE(phone.satWaiting);

    // 重连后仍可正常发起延迟应答
    link.onOpen(kHandle, kAddr, 10U);
    send(kSatTrack, satPayload, 11U);
    TEST_ASSERT_EQUAL_UINT(1U, transport.reservedCount);
    store.satDone = false;
    link.pollDeferred(12U);
    checkReply(std::span{transport.frame}.first(transport.len), kSatTrack, kSetRes, kFail);
}

} // namespace

extern "C" void setUp() {}
extern "C" void tearDown() {}

/** @brief 覆盖 0x28 Wi-Fi 网页配置：查询状态+IP、开启登记延迟重启、关闭/非法/不支持/重复。 */
void testWifiControl()
{
    RouterFixture f{};
    constexpr std::array<uint8_t, 8U> kOffWithIp{0U, 192U, 168U, 10U, 12U, 0U, 0U, 0U};
    for (const uint8_t sender : {kNav, kSurvey}) {
        f.expectReply(sender, kWifiCtl, kQuery, {}, kQueryRes, kOffWithIp);
    }
    // 关闭：已在蓝牙模式，直接 OK，不登记重启。
    f.expectReply(kNav, kWifiCtl, kSet, std::array<uint8_t, 5U>{0U, 0U, 0U, 0U, 0U}, kSetRes, kOk);
    f.expectReply(kNav, kWifiCtl, kSet, {}, kSetRes, kFail);
    f.expectReply(kNav, kWifiCtl, kSet, std::array<uint8_t, 1U>{2U}, kSetRes, kFail);
    TEST_ASSERT_EQUAL_UINT(0U, f.store.wifiCalls);
    // 开启：负载 [1..4] wifiDomain 忽略。
    f.expectReply(kNav, kWifiCtl, kSet, std::array<uint8_t, 5U>{1U, 192U, 168U, 4U, 1U}, kSetRes,
                  kOk, 700U);
    TEST_ASSERT_EQUAL_UINT(1U, f.store.wifiCalls);
    TEST_ASSERT_EQUAL_UINT32(700U, f.store.lastWifiNow);
    // 已有待执行的复位/切换时链路模块拒绝。
    f.store.wifiAccept = false;
    f.expectReply(kSurvey, kWifiCtl, kSet, std::array<uint8_t, 1U>{1U}, kSetRes, kFail);
    // 无 Wi-Fi 的构建：查询 IP 全 0，开启失败且不调用登记。
    f.store.wifiSupported = false;
    f.store.wifiAccept = true;
    f.expectReply(kNav, kWifiCtl, kQuery, {}, kQueryRes, std::array<uint8_t, 8U>{});
    f.expectReply(kNav, kWifiCtl, kSet, std::array<uint8_t, 1U>{1U}, kSetRes, kFail);
    TEST_ASSERT_EQUAL_UINT(2U, f.store.wifiCalls);
    TEST_ASSERT_TRUE(f.router.handle(core::Role::kAlarm, {.msgId = kWifiCtl}, 0U, f.reply) ==
                     core::DispatchResult::kDenied);
}

/** @brief 执行手机兼容路由与延迟应答主机测试。 @retval Unity 结果。 */
int main()
{
    UNITY_BEGIN();
    RUN_TEST(testSharedMessagesBothApps);
    RUN_TEST(testNavOnlyMessagesBothApps);
    RUN_TEST(testDualAppMessagesAndPppGate);
    RUN_TEST(testDeferredRepliesAndDisconnectCancel);
    RUN_TEST(testWifiControl);
    return UNITY_END();
}
