// 网页 OTA 路由与 ESP-IDF 顺序写入适配（ADR-0019）。
// 仅 httpd 任务同步处理请求，可等待接收和 Flash；跨任务只经原子状态交互，非 ISR。
// 堆档位：非实时限量；自有代码无分配，esp_ota_begin 持有单个 SDK OTA 会话。
#include "modules/web/WebModule.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "AppContext.hpp"
#include "app/PowerRequests.hpp"
#include "def/SystemDef.hpp"
#include "esp_image_format.h"
#include "fw/Log.hpp"
#include "fw/Trace.hpp"
#include "lwip/sockets.h"
#include "modules/ota/OtaRuntime.hpp"
#include "modules/web/WebOtaReply.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

using web_ota::kFlashReply;
using web_ota::kStatusOk;
using web_ota::OtaReply;
using web_ota::replyFor;
using web_ota::TransferResult;
using web_ota::UploadCode;

constexpr const char *kContentLength = "Content-Length";
constexpr const char *kContentType = "Content-Type";
constexpr std::string_view kBinaryType = "application/octet-stream";
constexpr const char *kJsonType = "application/json";
constexpr long kReceiveWaitS = 1; // s；为 httpd 关闭会话和 B2 切换 SoftAP 留出时间。

static_assert(web_ota::kHeaderBytes == sizeof(esp_image_header_t) +
                                           sizeof(esp_image_segment_header_t) +
                                           sizeof(esp_app_desc_t));
static_assert(web_ota::kChipIdOffset == offsetof(esp_image_header_t, chip_id));
static_assert(web_ota::kAppDescOffset ==
              sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
static_assert(web_ota::kVersionOffset ==
              web_ota::kAppDescOffset + offsetof(esp_app_desc_t, version));
static_assert(web_ota::kProjectOffset ==
              web_ota::kAppDescOffset + offsetof(esp_app_desc_t, project_name));
static_assert(web_ota::kTextBytes == sizeof(esp_app_desc_t::version) &&
              web_ota::kTextBytes == sizeof(esp_app_desc_t::project_name));
static_assert(ESP_CHIP_ID_ESP32 == 0 && ESP_IMAGE_HEADER_MAGIC == web_ota::kImageMagic &&
              ESP_APP_DESC_MAGIC_WORD == web_ota::kAppDescMagic);

/**
 * @brief 发送固定长度的 JSON 响应
 * @param req: 当前 HTTP 请求
 * @param status: HTTP 状态行，静态字符串
 * @param body: JSON 正文，长度最多为网页固定帧容量
 * @retval ESP_OK 发送成功且连接可保持
 * @retval 其他 HTTPD 错误，连接将关闭
 * @note 仅 httpd，可阻塞在 TCP 发送。
 */
[[nodiscard]] esp_err_t sendJson(httpd_req_t *req, const char *status,
                                 std::string_view body) noexcept
{
    esp_err_t err = httpd_resp_set_status(req, status);
    if (err == ESP_OK) {
        err = httpd_resp_set_type(req, kJsonType);
    }
    if (err == ESP_OK) {
        // 正文受网页固定帧容量限制，转 ssize_t 安全。
        err = httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
    }
    return err;
}

/**
 * @brief 读取完整的 Content-Length，避免 IDF 的 32 位 content_len 在极值时截断
 * @param req: 当前 HTTP 请求
 * @param field: httpd 独占的头字段暂存，复用应答缓冲
 * @retval 声明字节数；缺失返回 0，超出可读范围返回 uint64_t 最大值
 * @note 仅 httpd；由 HTTP 解析器先检查字段的十进制语法。
 */
[[nodiscard]] uint64_t declaredLength(httpd_req_t *req, std::span<char> field) noexcept
{
    const auto len = httpd_req_get_hdr_value_len(req, kContentLength);
    if (len == 0U) {
        return 0U;
    }
    // IDF 6.1 在长度与容量相等时可能返回 ESP_OK 却丢掉最后一个字节。
    if (len >= field.size()) {
        return std::numeric_limits<uint64_t>::max();
    }
    if (httpd_req_get_hdr_value_str(req, kContentLength, field.data(), len + 1U) != ESP_OK) {
        return 0U;
    }
    const std::string_view text{field.data(), len};
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return 0U;
    }
    const auto *digits = field.subspan(first).data();
    const auto *limit = field.subspan(len).data();
    uint64_t value = 0U;
    const auto [end, error] = std::from_chars(digits, limit, value);
    if (error == std::errc::result_out_of_range) {
        return std::numeric_limits<uint64_t>::max();
    }
    if (error != std::errc{} ||
        std::string_view{end, limit}.find_first_not_of(" \t") != std::string_view::npos) {
        return 0U;
    }
    return value;
}

/**
 * @brief 判断是否为原始固件媒体类型，允许参数与首尾 HTTP 空白
 * @param req: HTTP 请求
 * @param[out] field: httpd 独占的头字段暂存，失败时内容不保证
 * @retval true 类型匹配
 * @retval false 缺失、截断或不匹配
 * @note 仅 httpd。
 */
[[nodiscard]] bool isBinaryRequest(httpd_req_t *req, std::span<char> field) noexcept
{
    const size_t len = httpd_req_get_hdr_value_len(req, kContentType);
    if (len == 0U || len >= field.size() ||
        httpd_req_get_hdr_value_str(req, kContentType, field.data(), len + 1U) != ESP_OK) {
        return false;
    }
    const std::string_view value{field.data(), len};
    auto media = value.substr(0U, value.find(';'));
    const auto first = media.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return false;
    }
    const auto last = media.find_last_not_of(" \t");
    media.remove_suffix(media.size() - last - 1U);
    media.remove_prefix(first);
    if (media.size() != kBinaryType.size()) {
        return false;
    }
    for (size_t i = 0; i < media.size(); ++i) {
        const char actual = media[i];
        const int lower = actual >= 'A' && actual <= 'Z' ? actual + ('a' - 'A') : actual;
        if (lower != kBinaryType[i]) {
            return false;
        }
    }
    return true;
}

/** httpd 当前请求持有的 OTA 句柄；所有 SDK 写操作在同一回调串行使用。 */
class OtaUploadIo final {
public:
    /**
     * @brief 保存 HTTP 请求及本次上传的中止条件
     * @param req: HTTP 请求
     * @param pause: kLink 关机标志
     * @param stations: SoftAP 站点快照
     * @param hadStation: 准入时已观测到至少一个站点
     * @note 仅 httpd 构造，原子量由静态 WebModule 持有。
     */
    constexpr OtaUploadIo(httpd_req_t *req, const std::atomic<bool> &pause,
                          const std::atomic<uint32_t> &stations, bool hadStation) noexcept
        : req_(req), pause_(&pause), stations_(&stations), hadStation_(hadStation)
    {}

    /** @brief 获取下一更新槽。 @retval SDK 槽指针，无槽时为 nullptr。 @note 仅 httpd。 */
    [[nodiscard]] const esp_partition_t *target() const noexcept
    {
        return esp_ota_get_next_update_partition(nullptr);
    }

    /** @brief 接收一段数据。 @param dst: 外供写入视图。 @retval 实际接收字节数或超时/断开状态。 */
    [[nodiscard]] web_ota::ReceiveResult receive(std::span<char> dst) const noexcept
    {
        const int n = httpd_req_recv(req_, dst.data(), dst.size());
        // 仅正值是实际接收长度，且不超过请求缓冲。
        return {n > 0 ? static_cast<size_t>(n) : 0U, n == HTTPD_SOCK_ERR_TIMEOUT};
    }

    /** @brief 读取接收计时器。 @retval 当前单调毫秒值，按 uint32_t 回绕。 */
    [[nodiscard]] uint32_t nowMs() const noexcept
    {
        return rtos::nowMs();
    }

    /** @brief 查询已观察到的 SoftAP 站点是否全部离开。 @retval true 站点归零。 @retval false 尚未观察到站点归零。 */
    [[nodiscard]] bool stationLost() const noexcept
    {
        // 拔掉唯一站点的 Wi-Fi 时可能没有 TCP FIN，不能等通用接收超时才退出。
        return hadStation_ &&
               web_cfg::staStateCount(stations_->load(std::memory_order_acquire)) == 0U;
    }

    /** @brief 查询关机、竞争重启或已无 Wi-Fi 站点。 @retval true 需中止。 @retval false 可继续。 */
    [[nodiscard]] bool stopRequested() const noexcept
    {
        return pause_->load(std::memory_order_acquire) ||
               !power::s_power_requests.canScheduleRestart() || stationLost();
    }

    /** @brief 只在头部校验后开启顺序写入。 @retval kOk 开启。 @retval kState 待确认。 @retval kFlash SDK 失败。 */
    [[nodiscard]] UploadCode begin() noexcept
    {
        part_ = target();
        const esp_err_t err = esp_ota_begin(part_, OTA_WITH_SEQUENTIAL_WRITES, &handle_);
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            return UploadCode::kState;
        }
        if (err != ESP_OK) {
            return UploadCode::kFlash;
        }
        ota::markOtherSlotDirty();
        return UploadCode::kOk;
    }

    /** @brief 写一块镜像。 @param src: 已收齐的数据。 @retval kOk 已写入。 @retval kFlash 写入失败。 */
    [[nodiscard]] UploadCode write(std::span<const char> src) const noexcept
    {
        if (esp_ota_write(handle_, src.data(), src.size()) != ESP_OK) {
            return UploadCode::kFlash;
        }
        // 全镜像和单块都不超过 ESP32 OTA 槽大小，转 uint32_t 无损。
        fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(WebTrace::kOtaWrite), 0U,
                  static_cast<uint32_t>(src.size()), 0U);
        return UploadCode::kOk;
    }

    /** @brief 校验完整镜像并消费句柄。 @param length: 实际收到的字节数。 @retval kOk 完整。 @retval kValidate 镜像校验失败。 @retval kFlash SDK 失败。 */
    [[nodiscard]] UploadCode end(size_t length) const noexcept
    {
        const esp_err_t err = esp_ota_end(handle_);
        if (err != ESP_OK) {
            return err == ESP_ERR_OTA_VALIDATE_FAILED ? UploadCode::kValidate : UploadCode::kFlash;
        }
        // SDK 以整个分区为上限。再核对实际镜像长度，防止未擦写的旧尾段补全截断上传。
        // ESP32 槽地址、上传长度均不超过 Flash 地址范围，收窄到 uint32_t 安全。
        const esp_partition_pos_t pos{.offset = part_->address,
                                      .size = static_cast<uint32_t>(length)};
        esp_image_metadata_t image{};
        return esp_image_get_metadata(&pos, &image) == ESP_OK && image.image_len <= length
                   ? UploadCode::kOk
                   : UploadCode::kValidate;
    }

    /** @brief 原子仲裁关机/重启，再提交启动槽。 @retval kOk 已提交。 @retval kAborted 其他电源动作已受理。 @retval kFlash 提交失败。 */
    [[nodiscard]] UploadCode setBoot() const noexcept
    {
        if (!power::s_power_requests.claimRestart()) {
            return UploadCode::kAborted;
        }
        if (esp_ota_set_boot_partition(part_) != ESP_OK) {
            power::s_power_requests.releaseAction();
            return UploadCode::kFlash;
        }
        ota::clearOtherSlotDirty();
        return UploadCode::kOk;
    }

    /** @brief 失败清理。 @note 未创建或已被 esp_ota_end 消费的句柄会返回 NOT_FOUND。 */
    void abort() const noexcept
    {
        const esp_err_t err = esp_ota_abort(handle_);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            FW_LOGW(fw::TraceCat::kSys, "<ota> abort failed: %s", esp_err_to_name(err));
        }
    }

private:
    httpd_req_t *req_;
    const std::atomic<bool> *pause_;
    const std::atomic<uint32_t> *stations_;
    bool hadStation_;
    const esp_partition_t *part_ = nullptr;
    esp_ota_handle_t handle_ = 0U;
};

/**
 * @brief 检查状态、长度及媒体类型，再进入可注入的流式传输
 * @param req: HTTP 请求
 * @param io: 已独占的 OTA 会话
 * @param buffer: BootArena 外供收包缓冲
 * @param length: 解析后的声明长度
 * @param startMs: 本次上传准入时的单调毫秒起点
 * @retval 流或前置检查结果
 * @note 仅 httpd；不在失败路径设置启动槽。
 */
[[nodiscard]] TransferResult runUpload(httpd_req_t *req, OtaUploadIo &io, std::span<char> buffer,
                                       uint64_t length, uint32_t startMs) noexcept
{
    if (ota::partitionState(esp_ota_get_running_partition()) == ESP_OTA_IMG_PENDING_VERIFY) {
        return {.code = UploadCode::kState};
    }
    const auto *target = io.target();
    if (length == 0U) {
        return {.code = UploadCode::kLength};
    }
    if (target == nullptr || buffer.size() < web_ota::kHeaderBytes) {
        return {.code = UploadCode::kFlash};
    }
    const auto code = web_ota::checkLength(length, target->size);
    if (code != UploadCode::kOk) {
        return {.code = code};
    }
    // 媒体类型校验后立即用同一工作区接收镜像头。
    if (req->content_len != length || !isBinaryRequest(req, buffer)) {
        return {.code = UploadCode::kBadImage};
    }
    const int fd = httpd_req_to_sockfd(req);
    timeval previous{};
    socklen_t previousSize = sizeof(previous);
    const timeval wait{.tv_sec = kReceiveWaitS, .tv_usec = 0};
    // 缩短单次等待以尽早观察关机；流式状态机按 30 s 无进展与 150 s 总时长判超时。
    if (fd < 0 || lwip_getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, &previousSize) != 0 ||
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) != 0) {
        return {.code = UploadCode::kFlash};
    }
    const auto result =
        web_ota::transfer(io, buffer, req->content_len,
                          ota::imageText(esp_app_get_description()->project_name), startMs);
    // 失败后会关闭会话；成功后短暂保留连接直到计划重启。
    if (result.code == UploadCode::kOk &&
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, previousSize) != 0) {
        FW_LOGW(fw::TraceCat::kSys, "<ota> could not restore socket receive timeout");
    }
    return result;
}

/**
 * @brief 尝试发送一次完整的关闭式 HTTP 错误应答
 * @param req: HTTP 请求
 * @param reply: 契约错误应答，含静态状态行与正文
 * @param[out] out: httpd 独占的传输缓冲
 * @retval ESP_FAIL 已发送，须关闭会话且跳过余下请求体
 * @retval ESP_ERR_HTTPD_RESP_SEND 应答未完整发送，须关闭会话
 * @note 仅 httpd。发送不等待 TCP 窗口，也不经过 IDF 的应答事件队列。
 */
[[nodiscard]] esp_err_t sendUploadError(httpd_req_t *req, const OtaReply &reply,
                                        std::span<char> out) noexcept
{
    const int fd = httpd_req_to_sockfd(req);
    // 所有错误正文均为固定短字符串，字节数可用 unsigned 表示。
    const int len = std::snprintf(out.data(), out.size(),
                                  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                  "Connection: close\r\n\r\n%s",
                                  reply.status, kJsonType,
                                  static_cast<unsigned>(std::strlen(reply.body)), reply.body);
    // 正值可以安全转换成无符号长度。
    if (fd < 0 || len <= 0 || static_cast<size_t>(len) >= out.size()) {
        return ESP_ERR_HTTPD_RESP_SEND;
    }
    // 关机请求也可能在普通错误应答期间到达，所有失败响应都不叠加发送超时。
    // len 已确认是缓冲中可发送的正字节数。
    return lwip_send(fd, out.data(), static_cast<size_t>(len), MSG_DONTWAIT) == len
               ? ESP_FAIL
               : ESP_ERR_HTTPD_RESP_SEND;
}

/**
 * @brief 发送上传结果，失败时关闭连接以跳过 IDF 的剩余正文清空
 * @param req: HTTP 请求
 * @param result: 流或准入结果
 * @param out: httpd 独占的 JSON 输出缓冲
 * @retval ESP_OK 成功响应已发送
 * @retval 其他 HTTPD 需要关闭本次连接
 * @note 仅 httpd，可因发送而阻塞。
 */
[[nodiscard]] esp_err_t sendUpload(httpd_req_t *req, const TransferResult &result,
                                   std::span<char> out) noexcept
{
    const OtaReply reply = replyFor(result.code);
    if (result.code != UploadCode::kOk) {
        return sendUploadError(req, reply, out);
    }
    const size_t len = web_ota::renderSuccess(ota::imageText(result.version), out);
    return len != 0U ? sendJson(req, reply.status, {out.data(), len}) : ESP_FAIL;
}

/**
 * @brief 输出 OTA 槽信息，另一槽不可读时用 JSON null
 * @param running: 当前运行槽
 * @param other: 下一更新槽（本工程的另一 app 槽）
 * @param scratch: httpd 独占的 OTA 接收工作区，此处暂用作 app 描述符存储
 * @param[out] out: httpd 独占的 JSON 输出缓冲
 * @retval JSON 字节数，容量或状态异常返回 0
 * @note 仅 httpd，读取 Flash；不修改槽信息。
 */
[[nodiscard]] size_t renderInfo(const esp_partition_t &running, const esp_partition_t *other,
                                std::span<std::byte> scratch, std::span<char> out) noexcept
{
    // 在字节数组内构造 SDK 对象。HTTP 回调串行，GET 结束后 POST 才能使用该存储。
    auto *desc = ::new (scratch.data()) esp_app_desc_t;
    const auto otherVersion = ota::otherSlotVersion(other, *desc);
    const auto len =
        web_ota::renderInfo({.running = appVersion(),
                             .project = ota::imageText(esp_app_get_description()->project_name),
                             .part = ota::imageText(running.label),
                             .state = ota::stateName(ota::partitionState(&running)),
                             .other = otherVersion,
                             .max = other != nullptr ? other->size : 0U,
                             .busy = ota::transferBusy()},
                            out);
    desc->~esp_app_desc_t();
    return len;
}

} // namespace

fw::Status WebModule::bindOtaStorage(std::span<std::byte> buffer) noexcept
{
    static_assert(web_ota::kBufferBytes >= sizeof(esp_app_desc_t));
    if (initialized_ || httpd_ != nullptr) {
        return fw::Status::kInvalidState;
    }
    // 只检查地址的数值对齐，工作区可安全构造 SDK 描述符。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 仅检查对齐。
    const auto address = reinterpret_cast<uintptr_t>(buffer.data());
    if (buffer.size() != web_ota::kBufferBytes || address % alignof(esp_app_desc_t) != 0U) {
        return fw::Status::kInvalidArg;
    }
    otaBuffer_ = buffer;
    return fw::Status::kOk;
}

esp_err_t WebModule::registerOtaHandlers() noexcept
{
    const httpd_uri_t info{.uri = "/ota/info",
                           .method = HTTP_GET,
                           .handler = &WebModule::handleOtaInfo,
                           .user_ctx = this,
                           .is_websocket = false,
                           .handle_ws_control_frames = false,
                           .supported_subprotocol = nullptr,
                           .ws_post_handshake_cb = nullptr};
    auto upload = info;
    upload.uri = "/ota";
    upload.method = HTTP_POST;
    upload.handler = &WebModule::handleOtaPost;
    auto rollback = upload;
    rollback.uri = "/ota/rollback";
    rollback.handler = &WebModule::handleOtaRollback;
    esp_err_t err = httpd_register_uri_handler(httpd_, &info);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(httpd_, &upload);
    }
    return err == ESP_OK ? httpd_register_uri_handler(httpd_, &rollback) : err;
}

esp_err_t WebModule::handleOtaInfo(httpd_req_t *req) noexcept
{
    // user_ctx 原样指向注册时的 WebModule；运行槽由 IDF 返回。
    const auto &self = *static_cast<const WebModule *>(req->user_ctx);
    auto &out = self.httpdTxFrame_->data;
    const auto *running = esp_ota_get_running_partition();
    const auto *other = esp_ota_get_next_update_partition(running);
    const size_t len = running != nullptr ? renderInfo(*running, other, self.otaBuffer_, out) : 0U;
    if (len == 0U) {
        return sendJson(req, kFlashReply.status, kFlashReply.body);
    }
    return sendJson(req, kStatusOk, {out.data(), len});
}

esp_err_t WebModule::handleOtaPost(httpd_req_t *req) noexcept
{
    // user_ctx 原样指向注册时的 WebModule。
    auto &self = *static_cast<WebModule *>(req->user_ctx);
    const bool hadStation =
        web_cfg::staStateCount(self.staState_.load(std::memory_order_acquire)) != 0U;
    OtaUploadIo io{req, self.powerPauseRequested_, self.staState_, hadStation};
    auto &out = self.httpdTxFrame_->data;
    const uint32_t startMs = io.nowMs();
    const auto length = declaredLength(req, out);
    FW_LOGI(fw::TraceCat::kSys, "<ota> upload started size=%llu B",
            static_cast<unsigned long long>(length));
    const bool admitted =
        self.ctx().bootPhase.load(std::memory_order_acquire) == BootPhase::kRunning &&
        !io.stopRequested() && ota::beginOperation(ota::Operation::kUpload);
    // char 视图用于 HTTP 接收及镜像字节处理，工作区在当前 httpd 回调内独占。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char 可访问字节数组的对象表示。
    const std::span<char> buffer{reinterpret_cast<char *>(self.otaBuffer_.data()),
                                 self.otaBuffer_.size_bytes()};
    const auto result = admitted ? runUpload(req, io, buffer, length, startMs)
                                 : TransferResult{.code = UploadCode::kBusy};
    if (result.code != UploadCode::kOk) {
        io.abort();
    }
    const auto reply = replyFor(result.code);
    if (admitted) {
        ota::rememberUpload(reply.code);
    }
    FW_LOGI(fw::TraceCat::kSys, "<ota> upload ended result=%s bytes=%zu elapsed=%lu ms", reply.code,
            result.received, static_cast<unsigned long>(io.nowMs() - startMs));
    const esp_err_t sent = result.code == UploadCode::kAborted && io.stationLost()
                               ? ESP_FAIL
                               : sendUpload(req, result, out);
    if (result.code == UploadCode::kOk) {
        // 提交已成功，即使浏览器断开也必须由 kLink 延迟重启。
        self.otaRebootRequested_.store(true, std::memory_order_release);
    } else if (admitted) {
        self.otaIdleReset_.store(true, std::memory_order_release);
        ota::endOperation();
    }
    return sent;
}

esp_err_t WebModule::handleOtaRollback(httpd_req_t *req) noexcept
{
    // user_ctx 原样指向注册时的 WebModule。
    auto &self = *static_cast<WebModule *>(req->user_ctx);
    if (self.ctx().bootPhase.load(std::memory_order_acquire) != BootPhase::kRunning ||
        self.powerPauseRequested_.load(std::memory_order_acquire)) {
        const auto busy = replyFor(UploadCode::kBusy);
        return sendJson(req, busy.status, busy.body);
    }
    esp_app_desc_t desc{};
    switch (ota::switchToOther(desc)) {
    case ota::SwitchResult::kBusy: {
        const auto busy = replyFor(UploadCode::kBusy);
        return sendJson(req, busy.status, busy.body);
    }
    case ota::SwitchResult::kNoValidOther:
        return sendJson(req, "409 Conflict", R"({"ok":false,"err":"no_valid_other"})");
    case ota::SwitchResult::kOk: {
        auto &out = self.httpdTxFrame_->data;
        const auto len = web_ota::renderSuccess(ota::imageText(desc.version), out);
        const auto sent = len != 0U ? sendJson(req, kStatusOk, {out.data(), len}) : ESP_FAIL;
        // 提交已成功，连接中断也沿用上传的延迟重启流程。
        self.otaRebootRequested_.store(true, std::memory_order_release);
        return sent;
    }
    case ota::SwitchResult::kFlash:
    default:
        return sendJson(req, kFlashReply.status, kFlashReply.body);
    }
}

} // namespace app
