// ADR-0019 镜像头、流式传输、超时与回滚判定的主机测试。
// 仅主机测试单线程执行，不依赖 ESP-IDF。
#include "modules/ota/OtaPolicy.hpp"
#include "modules/web/WebOtaReply.hpp"

#include <cassert>
#include <initializer_list>
#include <limits>

namespace {

using app::ota::BootOutcome;
using app::ota::RollbackAction;
using app::ota::rollbackAction;
using app::web_ota::checkHeader;
using app::web_ota::checkLength;
using app::web_ota::ReceiveResult;
using app::web_ota::transfer;
using app::web_ota::UploadCode;

constexpr size_t kSlotBytes = 3145728U;
constexpr size_t kImageBytes = 4200U;
constexpr std::string_view kProject = "rtk_main";
constexpr std::string_view kVersion = "6.0.0";

static_assert(checkLength(0U, kSlotBytes) == UploadCode::kLength);
static_assert(checkLength(287U, kSlotBytes) == UploadCode::kBadImage);
static_assert(checkLength(288U, kSlotBytes) == UploadCode::kBadImage);
static_assert(checkLength(289U, kSlotBytes) == UploadCode::kOk);
static_assert(checkLength(kSlotBytes, kSlotBytes) == UploadCode::kOk);
static_assert(checkLength(kSlotBytes + 1U, kSlotBytes) == UploadCode::kTooLarge);
static_assert(checkLength(0x100001068ULL, kSlotBytes) == UploadCode::kTooLarge);

constexpr uint32_t kValidDelayMs = 30000U;
constexpr uint32_t kReadyMs = 12345U;
static_assert(rollbackAction(true, BootOutcome::kFailed, 0U, 0U, kValidDelayMs) ==
              RollbackAction::kInvalidate);
static_assert(rollbackAction(true, BootOutcome::kReady, kReadyMs, kReadyMs + kValidDelayMs - 1U,
                             kValidDelayMs) == RollbackAction::kNone);
static_assert(rollbackAction(true, BootOutcome::kReady, kReadyMs, kReadyMs + kValidDelayMs,
                             kValidDelayMs) == RollbackAction::kConfirm);
static_assert(rollbackAction(true, BootOutcome::kReady, kReadyMs, kReadyMs + kValidDelayMs + 1U,
                             kValidDelayMs) == RollbackAction::kConfirm);
// valid 和 undefined 都由 IDF 边界映射成非 pending_verify。
static_assert(rollbackAction(false, BootOutcome::kReady, 0U, kValidDelayMs, kValidDelayMs) ==
              RollbackAction::kNone);
static_assert(rollbackAction(false, BootOutcome::kFailed, 0U, kValidDelayMs, kValidDelayMs) ==
              RollbackAction::kNone);
constexpr uint32_t kNearWrapMs = UINT32_MAX - 10U;
static_assert(rollbackAction(true, BootOutcome::kReady, kNearWrapMs, 29988U, kValidDelayMs) ==
              RollbackAction::kNone);
static_assert(rollbackAction(true, BootOutcome::kReady, kNearWrapMs, 29989U, kValidDelayMs) ==
              RollbackAction::kConfirm);

/** @brief 构造与 ADR 及 IDF 镜像格式相符的线格式样本。 @retval 固定长度镜像测试数据。 */
std::array<char, kImageBytes> makeImage()
{
    std::array<char, kImageBytes> image{};
    image[0] = '\xE9';
    image[32] = '\x32';
    image[33] = '\x54';
    image[34] = '\xCD';
    image[35] = '\xAB';
    std::ranges::copy(kVersion, image.begin() + 48);
    std::ranges::copy(kProject, image.begin() + 80);
    std::ranges::fill(std::span{image}.subspan(288U), '\x25');
    return image;
}

/** @brief 验证逐字段拒绝、防止读越界和版本保留。 @note 主机测试单线程。 */
void testHeader()
{
    const auto image = makeImage();
    std::array<char, 32> version{};
    assert(checkHeader(image, kProject, version));
    assert(std::string_view{version.data()} == kVersion);
    assert(!checkHeader(std::span{image}.first(287U), kProject, version));
    for (const size_t offset : {0U, 12U, 13U, 32U, 35U, 80U, 88U}) {
        auto bad = image;
        bad[offset] = 'x';
        assert(!checkHeader(bad, kProject, version));
    }
    assert(!checkHeader(image, "another_project", version));
    assert(std::string_view{version.data()} == kVersion);
}

/** 分块与 SDK 失败由测试直接注入；写入顺序以磁盘上的原始镜像前缀为准。 */
struct FakeOtaIo {
    std::array<char, kImageBytes> image = makeImage();
    size_t input = 0U;
    size_t written = 0U;
    uint32_t timeMs = 0U;
    unsigned calls = 0U;
    unsigned armedAtCall = std::numeric_limits<unsigned>::max();
    unsigned dropAtCall = std::numeric_limits<unsigned>::max();
    unsigned nextProgressCall = 0U;
    unsigned stallAfterProgress = 0U;
    bool began = false;
    bool ended = false;
    bool committed = false;
    bool abortAtEnd = false;
    UploadCode beginCode = UploadCode::kOk;
    UploadCode writeCode = UploadCode::kOk;
    UploadCode endCode = UploadCode::kOk;
    UploadCode commitCode = UploadCode::kOk;

    /** @brief 按小块交付，模拟暂时超时、掉线或中止。 @param dst: 接收窗口。 @retval 字节数或超时。 */
    ReceiveResult receive(std::span<char> dst)
    {
        ++calls;
        timeMs += 5000U;
        if (calls >= dropAtCall) {
            return {};
        }
        if (calls < nextProgressCall) {
            return {.timedOut = true};
        }
        if (stallAfterProgress != 0U) {
            nextProgressCall = calls + stallAfterProgress;
        }
        // 测试接收上限刻意不与头部或 flash 分块对齐。
        const size_t len = std::min({dst.size(), image.size() - input, size_t{127}});
        std::ranges::copy(std::span{image}.subspan(input, len), dst.begin());
        input += len;
        return {.bytes = len};
    }

    /** @brief 提供可回绕的测试时钟。 @retval 当前毫秒数。 */
    uint32_t nowMs() const
    {
        return timeMs;
    }

    /** @brief 模拟已受理关机。 @retval true 到达中止点。 @retval false 可继续。 */
    bool stopRequested() const
    {
        return calls >= armedAtCall || (ended && abortAtEnd);
    }

    /** @brief 校验 begin 前只收齐头部。 @retval 注入的结果。 */
    UploadCode begin()
    {
        assert(!began && input == 288U);
        began = true;
        return beginCode;
    }

    /** @brief 核对写入顺序与数据。 @param data: 写入块。 @retval 注入的结果。 */
    UploadCode write(std::span<const char> data)
    {
        assert(began && !ended);
        assert(data.size() <= 2048U);
        assert(std::ranges::equal(data, std::span{image}.subspan(written, data.size())));
        written += data.size();
        return writeCode;
    }

    /** @brief 校验收到全部数据才结束。 @param len: 声明长度。 @retval 注入的结果。 */
    UploadCode end(size_t len)
    {
        assert(began && !ended && written == kImageBytes && len == kImageBytes);
        ended = true;
        return endCode;
    }

    /** @brief 检查先 end 后设置启动槽。 @retval 注入的结果。 */
    UploadCode setBoot()
    {
        assert(ended && endCode == UploadCode::kOk && !committed);
        committed = true;
        return commitCode;
    }
};

/** @brief 覆盖成功、Flash 失败、主动取消、断开与无数据超时。 @note 主机测试单线程。 */
void testTransfer()
{
    std::array<char, 2048> storage{};
    FakeOtaIo io{};
    auto out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kOk && out.received == kImageBytes && io.committed);
    assert(std::string_view{out.version.data()} == kVersion);

    io = {};
    io.image[80] = 'x';
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kBadImage && out.received == 288U && !io.began);

    io = {};
    io.beginCode = UploadCode::kFlash;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kFlash && io.began && io.written == 0U && !io.ended);

    io = {};
    io.writeCode = UploadCode::kFlash;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kFlash && !io.ended && !io.committed);

    io = {};
    io.endCode = UploadCode::kValidate;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kValidate && io.ended && !io.committed);

    io = {};
    io.commitCode = UploadCode::kFlash;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kFlash && io.committed);

    io = {};
    io.armedAtCall = 4U;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kAborted && io.written == 288U && !io.ended);

    io = {};
    io.abortAtEnd = true;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kAborted && io.ended && !io.committed);

    io = {};
    io.dropAtCall = 2U;
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kAborted && out.received == 127U && !io.began);

    io = {};
    io.timeMs = UINT32_MAX - 2000U;
    io.nextProgressCall = 7U;
    const auto start = io.nowMs();
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kTimeout && out.received == 0U && !io.began);
    assert(io.nowMs() - start == 30000U);

    io = {};
    io.stallAfterProgress = 5U; // 每次进展后超时四次；累积总耗时远超 30 s。
    out = transfer(io, storage, kImageBytes, kProject);
    assert(out.code == UploadCode::kOk && io.committed && io.nowMs() > 30000U);
}

/** @brief 核对 §2.3 的字段顺序、JSON 字节及全部错误状态。 @note 主机测试单线程。 */
void testHttpContract()
{
    using app::web_ota::OtaInfo;
    using app::web_ota::renderInfo;
    using app::web_ota::renderSuccess;
    using app::web_ota::replyFor;

    std::array<char, 320> storage{};
    const OtaInfo info{"6.1.0-dev", "rtk_main", "ota_0", "valid", "6.0.0", kSlotBytes, false};
    const size_t len = renderInfo(info, storage);
    assert((
        std::string_view{storage.data(), len} ==
        R"({"running":"6.1.0-dev","project":"rtk_main","part":"ota_0","state":"valid","other":"6.0.0","max":3145728,"busy":false})"));
    auto noOther = info;
    noOther.other.reset();
    noOther.busy = true;
    const size_t nullLen = renderInfo(noOther, storage);
    assert((
        std::string_view{storage.data(), nullLen} ==
        R"({"running":"6.1.0-dev","project":"rtk_main","part":"ota_0","state":"valid","other":null,"max":3145728,"busy":true})"));
    assert(renderInfo(noOther, std::span{storage}.first(nullLen - 1U)) == 0U);
    assert(renderInfo(noOther, std::span{storage}.first(nullLen)) == nullLen);

    const size_t ackLen = renderSuccess(R"(6.0.0-"dev")", storage);
    assert((std::string_view{storage.data(), ackLen} ==
            R"({"ok":true,"ver":"6.0.0-\"dev\"","reboot_ms":1500})"));
    assert(renderSuccess("6.0", {}) == 0U);
    const size_t escapedLen = renderSuccess("\b\f\n\r\t\x01\\测", storage);
    assert((std::string_view{storage.data(), escapedLen} ==
            R"({"ok":true,"ver":"\b\f\n\r\t\u0001\\测","reboot_ms":1500})"));
    assert(std::string_view{replyFor(UploadCode::kOk).status}.starts_with("200 "));

    struct Expected {
        UploadCode code;
        std::string_view status;
        std::string_view body;
    };
    constexpr std::array kErrors{
        Expected{UploadCode::kBusy, "409 ", R"({"ok":false,"err":"busy"})"},
        Expected{UploadCode::kState, "409 ", R"({"ok":false,"err":"state"})"},
        Expected{UploadCode::kLength, "411 ", R"({"ok":false,"err":"length"})"},
        Expected{UploadCode::kTooLarge, "413 ", R"({"ok":false,"err":"too_large"})"},
        Expected{UploadCode::kBadImage, "400 ", R"({"ok":false,"err":"bad_image"})"},
        Expected{UploadCode::kValidate, "400 ", R"({"ok":false,"err":"validate"})"},
        Expected{UploadCode::kTimeout, "408 ", R"({"ok":false,"err":"timeout"})"},
        Expected{UploadCode::kFlash, "500 ", R"({"ok":false,"err":"flash"})"},
        Expected{UploadCode::kAborted, "503 ", R"({"ok":false,"err":"aborted"})"},
    };
    for (const auto &error : kErrors) {
        const auto reply = replyFor(error.code);
        assert(std::string_view{reply.status}.starts_with(error.status));
        assert(std::string_view{reply.body} == error.body);
    }
}

} // namespace

/** @brief 运行头部及流式测试；回滚判定和长度边界在编译期覆盖。 @retval 0 全部通过。 */
int main()
{
    testHeader();
    testTransfer();
    testHttpContract();
    return 0;
}
