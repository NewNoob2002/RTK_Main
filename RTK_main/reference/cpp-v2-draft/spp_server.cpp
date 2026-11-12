// Bluedroid 经典蓝牙 SPP 多会话服务端（仅 ESP32：SOC_BT_CLASSIC_SUPPORTED）。
// 线程模型：BTC 回调只拷贝数据、更新拥塞标志、非阻塞投递事件；链路任务独占
// poll_event/read/write/disconnect。会话表用 portMUX 保护，回调中不阻塞、不解析、不打印数据内容。
//
// IDF v6.1 核实（reference/c-v1/docs/reports/T04.md，本任务重新对照源码）：
// 1. esp_spp_write 深拷贝（btc_spp_arg_deep_copy 中 osi_malloc+memcpy）；
// 2. SRV_OPEN 后服务端继续监听，max_session = ESP_SPP_MAX_SESSION = MAX_BD_CONNECTIONS = 2；
// 3. 对端地址在 srv_open.rem_bda。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "soc/soc_caps.h"

#if !SOC_BT_CLASSIC_SUPPORTED
#error "rtk_bt requires Bluetooth Classic (SOC_BT_CLASSIC_SUPPORTED); see ADR-0002 section 7"
#endif

#include "rtk/bt/spp_server.hpp"

#include <array>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "freertos/FreeRTOS.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"

#include "rtk/os/critical_section.hpp"
#include "rtk/os/esp_status.hpp"
#include "rtk/os/static_event_group.hpp"
#include "rtk/os/static_queue.hpp"
#include "rtk/os/static_stream_buffer.hpp"

namespace rtk::bt {

namespace {

constexpr const char *kTag = "rtk_bt";
constexpr const char *kServerName = "SPP_SERVER";
constexpr size_t kSessionCount = core::kMaxSessions;
constexpr size_t kStreamTrigger = 1;

struct Session {
    bool in_use = false;
    bool congested = false;
    core::SessionHandle handle = 0;
    core::BtAddr addr{};
    uint8_t index = 0;
    os::StaticStreamBuffer<kRxBufferSize> rx{};
};

struct HwState {
    bool controller_inited = false;
    bool controller_enabled = false;
    bool bluedroid_inited = false;
    bool bluedroid_enabled = false;
    bool spp_inited = false;
};

std::array<Session, kSessionCount> s_sessions{};
portMUX_TYPE s_session_mux = portMUX_INITIALIZER_UNLOCKED;
os::StaticQueue<Event, kEventQueueDepth> s_events{};
os::StaticEventGroup s_congestion{};
std::array<char, kNameMax + 1> s_device_name{};
uint32_t s_dropped_events = 0;
bool s_initialized = false;

EventBits_t congestion_bit(uint8_t index) noexcept
{
    return static_cast<EventBits_t>(1UL) << index;
}

void log_if_error(const char *what, esp_err_t err) noexcept
{
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "%s: %s", what, esp_err_to_name(err));
    }
}

void cleanup_objects() noexcept
{
    for (Session &session : s_sessions) {
        session.rx.deinit();
    }
    s_events.deinit();
    s_congestion.deinit();
    s_initialized = false;
}

void unwind_hardware(HwState &hw) noexcept
{
    // 逆序释放；每一步都检查返回值，失败继续释放剩余部分。
    if (hw.spp_inited) {
        log_if_error("spp deinit", esp_spp_deinit());
        hw.spp_inited = false;
    }
    if (hw.bluedroid_enabled) {
        log_if_error("bluedroid disable", esp_bluedroid_disable());
        hw.bluedroid_enabled = false;
    }
    if (hw.bluedroid_inited) {
        log_if_error("bluedroid deinit", esp_bluedroid_deinit());
        hw.bluedroid_inited = false;
    }
    if (hw.controller_enabled) {
        log_if_error("controller disable", esp_bt_controller_disable());
        hw.controller_enabled = false;
    }
    if (hw.controller_inited) {
        log_if_error("controller deinit", esp_bt_controller_deinit());
        hw.controller_inited = false;
    }
}

bool valid_name(std::string_view name) noexcept
{
    if (name.empty() || (name.size() > kNameMax)) {
        return false;
    }
    for (const char ch : name) {
        if ((ch == '\n') || (ch == '\r')) {
            return false;
        }
    }
    return true;
}

Session *session_find(core::SessionHandle handle) noexcept
{
    Session *found = nullptr;
    {
        os::CriticalSection guard(s_session_mux);
        for (Session &session : s_sessions) {
            if (session.in_use && (session.handle == handle)) {
                found = &session;
                break;
            }
        }
    }
    return found;
}

Session *session_alloc(core::SessionHandle handle, const core::BtAddr &addr) noexcept
{
    size_t free_idx = kSessionCount;
    {
        os::CriticalSection guard(s_session_mux);
        for (size_t i = 0; i < kSessionCount; ++i) {
            if (!s_sessions[i].in_use) {
                free_idx = i;
                break;
            }
        }
    }
    if (free_idx == kSessionCount) {
        return nullptr;
    }

    Session &session = s_sessions[free_idx];
    session.rx.reset();
    session.handle = handle;
    session.addr = addr;
    session.congested = false;
    // free_idx < kSessionCount(2)，uint8_t 截断安全。
    session.index = static_cast<uint8_t>(free_idx);
    {
        os::CriticalSection guard(s_session_mux);
        session.in_use = true;
    }
    return &session;
}

void session_free(core::SessionHandle handle) noexcept
{
    uint8_t index = 0;
    bool found = false;
    {
        os::CriticalSection guard(s_session_mux);
        for (Session &session : s_sessions) {
            if (session.in_use && (session.handle == handle)) {
                session.in_use = false;
                session.congested = false;
                index = session.index;
                found = true;
                break;
            }
        }
    }
    if (found) {
        s_congestion.set(congestion_bit(index));
    }
}

void session_set_congested(core::SessionHandle handle, bool congested) noexcept
{
    uint8_t index = 0;
    bool found = false;
    {
        os::CriticalSection guard(s_session_mux);
        for (Session &session : s_sessions) {
            if (session.in_use && (session.handle == handle)) {
                session.congested = congested;
                index = session.index;
                found = true;
                break;
            }
        }
    }
    if (found) {
        if (congested) {
            s_congestion.clear(congestion_bit(index));
        } else {
            s_congestion.set(congestion_bit(index));
        }
    }
}

void post_event(EventType type, core::SessionHandle handle, const uint8_t *addr, uint32_t dropped) noexcept
{
    Event event{};
    event.type = type;
    event.handle = handle;
    event.dropped = dropped;
    if (addr != nullptr) {
        for (size_t i = 0; i < core::kAddrLen; ++i) {
            event.addr.bytes[i] = addr[i];
        }
    }

    // 回调上下文中不得阻塞：队列满只计数并记日志。
    if (!s_events.send(event, 0)) {
        uint32_t total = 0;
        {
            os::CriticalSection guard(s_session_mux);
            ++s_dropped_events;
            total = s_dropped_events;
        }
        ESP_LOGW(kTag, "event queue full, total dropped=%u", static_cast<unsigned>(total));
    }
}

void handle_spp_init(const esp_spp_cb_param_t *param) noexcept
{
    if (param->init.status != ESP_SPP_SUCCESS) {
        ESP_LOGE(kTag, "spp init failed: %d", static_cast<int>(param->init.status));
        return;
    }

    log_if_error("start server", esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, kServerName));
    log_if_error("set device name", esp_bt_gap_set_device_name(s_device_name.data()));
    log_if_error("set scan mode", esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
}

void handle_srv_open(const esp_spp_cb_param_t *param) noexcept
{
    if (param->srv_open.status != ESP_SPP_SUCCESS) {
        ESP_LOGE(kTag, "srv open failed: %d", static_cast<int>(param->srv_open.status));
        return;
    }

    core::BtAddr addr{};
    for (size_t i = 0; i < core::kAddrLen; ++i) {
        addr.bytes[i] = param->srv_open.rem_bda[i];
    }

    const core::SessionHandle handle = param->srv_open.handle;
    Session *session = session_alloc(handle, addr);
    if (session == nullptr) {
        ESP_LOGE(kTag, "no session slot, disconnect handle=0x%08" PRIx32, handle);
        log_if_error("disconnect (no slot)", esp_spp_disconnect(handle));
        post_event(EventType::kClose, handle, nullptr, 0);
        return;
    }

    post_event(EventType::kOpen, handle, param->srv_open.rem_bda, 0);
}

void handle_close(const esp_spp_cb_param_t *param) noexcept
{
    session_free(param->close.handle);
    post_event(EventType::kClose, param->close.handle, nullptr, 0);
}

void handle_data(const esp_spp_cb_param_t *param) noexcept
{
    if ((param->data_ind.status != ESP_SPP_SUCCESS) || (param->data_ind.data == nullptr)) {
        return;
    }
    // status 成功时 len ≥ 0；先判负再做 size_t 转换，避免截断。
    if (param->data_ind.len <= 0) {
        return;
    }

    Session *session = session_find(param->data_ind.handle);
    if (session == nullptr) {
        return;
    }

    const size_t len = static_cast<size_t>(param->data_ind.len);
    const std::span<const uint8_t> data(param->data_ind.data, len);
    const size_t written = session->rx.send(data, 0);
    if (written > 0) {
        post_event(EventType::kData, param->data_ind.handle, nullptr, 0);
    }
    if (written < len) {
        // len ≤ INT_MAX，written ≤ len，差值转 uint32 无损。
        const uint32_t dropped = static_cast<uint32_t>(len - written);
        post_event(EventType::kRxDrop, param->data_ind.handle, nullptr, dropped);
        ESP_LOGW(kTag, "rx overflow handle=0x%08" PRIx32 " dropped=%u", param->data_ind.handle,
                 static_cast<unsigned>(dropped));
    }
}

void handle_write(const esp_spp_cb_param_t *param) noexcept
{
    if (param->write.status != ESP_SPP_SUCCESS) {
        ESP_LOGW(kTag, "write failed handle=0x%08" PRIx32 " status=%d", param->write.handle,
                 static_cast<int>(param->write.status));
    }
}

void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) noexcept
{
    if (param == nullptr) {
        return;
    }
    switch (event) {
    case ESP_SPP_INIT_EVT:
        handle_spp_init(param);
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(kTag, "server started, scn=%u", static_cast<unsigned>(param->start.scn));
        } else {
            ESP_LOGE(kTag, "server start failed: %d", static_cast<int>(param->start.status));
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        handle_srv_open(param);
        break;
    case ESP_SPP_CLOSE_EVT:
        handle_close(param);
        break;
    case ESP_SPP_DATA_IND_EVT:
        handle_data(param);
        break;
    case ESP_SPP_CONG_EVT:
        session_set_congested(param->cong.handle, param->cong.cong);
        break;
    case ESP_SPP_WRITE_EVT:
        handle_write(param);
        break;
    case ESP_SPP_UNINIT_EVT:
        ESP_LOGI(kTag, "spp uninitialized");
        break;
    default:
        // 其余事件本阶段不处理。
        break;
    }
}

void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) noexcept
{
    if (param == nullptr) {
        return;
    }
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(kTag, "auth success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(kTag, "auth failed: %d", static_cast<int>(param->auth_cmpl.stat));
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code{};
        uint8_t pin_len = 0;
        if (param->pin_req.min_16_digit) {
            pin_len = 16;
        } else {
            // 旧设备固定 PIN "1234"（phone_protocol.md §6）。
            pin_len = static_cast<uint8_t>(kLegacyPin.size());
            for (size_t i = 0; i < kLegacyPin.size(); ++i) {
                pin_code[i] = static_cast<uint8_t>(kLegacyPin[i]);
            }
        }
        log_if_error("pin reply", esp_bt_gap_pin_reply(param->pin_req.bda, true, pin_len, pin_code));
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        log_if_error("ssp confirm", esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true));
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        break;
    default:
        break;
    }
}

Status wait_uncongested(const Session &session, uint32_t timeout_ms) noexcept
{
    const uint32_t start_ms = os::now_ms();
    const EventBits_t bit = congestion_bit(session.index);
    for (;;) {
        bool congested = false;
        {
            os::CriticalSection guard(s_session_mux);
            congested = session.congested;
        }
        if (!congested) {
            return Status::kOk;
        }

        const uint32_t elapsed = static_cast<uint32_t>(os::now_ms() - start_ms);
        if (elapsed >= timeout_ms) {
            return Status::kTimeout;
        }
        const uint32_t remaining = timeout_ms - elapsed;
        if ((s_congestion.wait(bit, false, true, remaining) & bit) == 0) {
            return Status::kTimeout;
        }
    }
}

} // namespace

SppServer &SppServer::instance() noexcept
{
    // 常量初始化，不生成 __cxa_guard（CPP-009）。
    static constinit SppServer s_server;
    return s_server;
}

Status SppServer::init(std::string_view name) noexcept
{
    if (!valid_name(name)) {
        return Status::kInvalidArg;
    }
    if (s_initialized) {
        return Status::kBusy;
    }

    // 静态 RTOS 对象先建立；失败时按逆序清理。
    Status status = s_events.init();
    if (!is_ok(status)) {
        cleanup_objects();
        return status;
    }
    status = s_congestion.init();
    if (!is_ok(status)) {
        cleanup_objects();
        return status;
    }
    for (size_t i = 0; i < kSessionCount; ++i) {
        status = s_sessions[i].rx.init(kStreamTrigger);
        if (!is_ok(status)) {
            cleanup_objects();
            return status;
        }
    }

    s_device_name.fill('\0');
    for (size_t i = 0; i < name.size(); ++i) {
        s_device_name[i] = name[i];
    }

    HwState hw{};
    auto fail = [&hw](esp_err_t err) -> Status {
        unwind_hardware(hw);
        cleanup_objects();
        return os::from_esp_err(err);
    };

    const esp_err_t mem_err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (mem_err != ESP_OK) {
        return fail(mem_err);
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    const esp_err_t ctrl_init = esp_bt_controller_init(&bt_cfg);
    if (ctrl_init != ESP_OK) {
        return fail(ctrl_init);
    }
    hw.controller_inited = true;

    const esp_err_t ctrl_enable = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ctrl_enable != ESP_OK) {
        return fail(ctrl_enable);
    }
    hw.controller_enabled = true;

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    const esp_err_t bd_init = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (bd_init != ESP_OK) {
        return fail(bd_init);
    }
    hw.bluedroid_inited = true;

    const esp_err_t bd_enable = esp_bluedroid_enable();
    if (bd_enable != ESP_OK) {
        return fail(bd_enable);
    }
    hw.bluedroid_enabled = true;

    const esp_err_t gap_reg = esp_bt_gap_register_callback(gap_callback);
    if (gap_reg != ESP_OK) {
        return fail(gap_reg);
    }
    const esp_err_t spp_reg = esp_spp_register_callback(spp_callback);
    if (spp_reg != ESP_OK) {
        return fail(spp_reg);
    }

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    const esp_err_t iocap_err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    if (iocap_err != ESP_OK) {
        return fail(iocap_err);
    }

    esp_bt_pin_code_t pin_code{};
    const esp_err_t pin_err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);
    if (pin_err != ESP_OK) {
        return fail(pin_err);
    }

    esp_spp_cfg_t spp_cfg{};
    spp_cfg.mode = ESP_SPP_MODE_CB;
    spp_cfg.enable_l2cap_ertm = true;
    spp_cfg.tx_buffer_size = 0;
    const esp_err_t spp_init = esp_spp_enhanced_init(&spp_cfg);
    if (spp_init != ESP_OK) {
        return fail(spp_init);
    }
    hw.spp_inited = true;

    s_initialized = true;
    return Status::kOk;
}

bool SppServer::poll_event(Event &event, uint32_t timeout_ms) noexcept
{
    if (!s_initialized) {
        return false;
    }
    return s_events.receive(event, timeout_ms);
}

size_t SppServer::read(core::SessionHandle handle, std::span<uint8_t> out) noexcept
{
    Session *session = session_find(handle);
    if (session == nullptr) {
        return 0;
    }
    return session->rx.receive(out, 0);
}

Status SppServer::write(core::SessionHandle handle, std::span<const uint8_t> data, uint32_t timeout_ms) noexcept
{
    if (data.empty() || (data.size() > static_cast<size_t>(INT_MAX))) {
        return Status::kInvalidArg;
    }

    Session *session = session_find(handle);
    if (session == nullptr) {
        return Status::kNotFound;
    }

    const Status wait = wait_uncongested(*session, timeout_ms);
    if (!is_ok(wait)) {
        return wait;
    }

    // IDF 形参非 const；esp_spp_write 会立即深拷贝（T04 结论 1），返回后 data 不再被引用。
    const esp_err_t err = esp_spp_write(handle, static_cast<int>(data.size()), const_cast<uint8_t *>(data.data()));
    return os::from_esp_err(err);
}

Status SppServer::disconnect(core::SessionHandle handle) noexcept
{
    return os::from_esp_err(esp_spp_disconnect(handle));
}

Status SppServer::local_address(core::BtAddr &addr) const noexcept
{
    const uint8_t *local = esp_bt_dev_get_address();
    if (local == nullptr) {
        return Status::kInvalidState;
    }
    for (size_t i = 0; i < core::kAddrLen; ++i) {
        addr.bytes[i] = local[i];
    }
    return Status::kOk;
}

uint32_t SppServer::dropped_events() const noexcept
{
    os::CriticalSection guard(s_session_mux);
    return s_dropped_events;
}

} // namespace rtk::bt
