// 异步发送装饰器实现，见 async_tx.hpp。
#include "modules/bt_link/async_tx.hpp"

#include <algorithm>

namespace rtk::app {
namespace {
// 发送任务空闲时的单次等待；只影响栈水位刷新频率，不影响时延（入队即唤醒）。
constexpr uint32_t kIdleWaitMs = 1000;
} // namespace

Status AsyncTxTransport::start(uint32_t priority, BaseType_t core) noexcept
{
    if (started_) {
        return Status::kBusy;
    }
    const Status queue_status = queue_.init();
    if (!is_ok(queue_status)) {
        return queue_status;
    }
    const Status task_status = task_.start("rtk_tx", priority, core, &AsyncTxTransport::task_entry, this);
    if (!is_ok(task_status)) {
        queue_.deinit();
        return task_status;
    }
    started_ = true;
    return Status::kOk;
}

Status AsyncTxTransport::write(core::SessionHandle handle, std::span<const uint8_t> data, uint32_t timeout_ms) noexcept
{
    if (!started_) {
        return inner_.write(handle, data, timeout_ms);
    }
    if (data.empty() || (data.size() > kMaxBytes)) {
        return Status::kInvalidArg;
    }
    tx_item_.handle = handle;
    tx_item_.timeout_ms = timeout_ms;
    tx_item_.size = static_cast<uint16_t>(data.size()); // ≤ kMaxBytes(278)，收窄无损
    std::copy(data.begin(), data.end(), tx_item_.bytes.begin());
    // 先加深度再入队：发送任务取走后才减，避免短暂下溢。
    const uint32_t depth = depth_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (!queue_.send(tx_item_, 0U)) {
        depth_.fetch_sub(1U, std::memory_order_relaxed);
        queue_full_.fetch_add(1U, std::memory_order_relaxed);
        return Status::kBusy;
    }
    uint32_t seen = max_depth_.load(std::memory_order_relaxed);
    while ((depth > seen) && !max_depth_.compare_exchange_weak(seen, depth, std::memory_order_relaxed)) {
    }
    return Status::kOk;
}

Status AsyncTxTransport::disconnect(core::SessionHandle handle) noexcept
{
    return inner_.disconnect(handle);
}

bool AsyncTxTransport::pump(uint32_t wait_ms) noexcept
{
    if (!queue_.receive(rx_item_, wait_ms)) {
        return false;
    }
    depth_.fetch_sub(1U, std::memory_order_relaxed);
    const Status status = inner_.write(rx_item_.handle, std::span<const uint8_t>{rx_item_.bytes}.first(rx_item_.size),
                                       rx_item_.timeout_ms);
    if (is_ok(status)) {
        sent_.fetch_add(1U, std::memory_order_relaxed);
    } else {
        failed_.fetch_add(1U, std::memory_order_relaxed);
    }
    return true;
}

AsyncTxStats AsyncTxTransport::stats() const noexcept
{
    AsyncTxStats out{};
    out.sent = sent_.load(std::memory_order_relaxed);
    out.failed = failed_.load(std::memory_order_relaxed);
    out.queue_full = queue_full_.load(std::memory_order_relaxed);
    out.max_depth = max_depth_.load(std::memory_order_relaxed);
    out.stack_free = task_.stack_high_water_mark();
    return out;
}

void AsyncTxTransport::task_entry(void *arg) noexcept
{
    auto &self = *static_cast<AsyncTxTransport *>(arg);
    for (;;) {
        static_cast<void>(self.pump(kIdleWaitMs));
    }
}

} // namespace rtk::app
