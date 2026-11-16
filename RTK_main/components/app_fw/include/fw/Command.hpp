// CommandBus 的请求/结果：定长、可平凡拷贝（经 FreeRTOS 队列按值传递）。
// 参数是原始字节，由目标模块自行解释（建议每个模块在自己的头里写 encode/decode 辅助函数）。
// 纯数据定义，无状态；拷贝本身可跨任务/ISR，队列访问的线程安全性由调用方保证。
// 冻结接口（ADR-0007 §3.5）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "fw/Result.hpp"

namespace fw {

inline constexpr size_t kCommandArgBytes = 32; // ≥ SN 最大 16 字节 + 0；以后更大的参数走 StoreKey/Pipe

/** 模块命令请求；target 为 ModuleId，verb 与 args 的含义由目标模块定义。 */
struct Command {
    uint8_t target = 0; // ModuleId
    uint8_t verb = 0;   // 模块内定义的动词
    uint8_t arg_len = 0;
    uint32_t seq = 0; // 由 Executor::call 分配，调用方不用填
    std::array<uint8_t, kCommandArgBytes> args{};
};

/** 模块命令结果；seq 由执行器回填，detail 为模块自定义附加码。 */
struct CommandResult {
    uint32_t seq = 0;
    Result status = Result::kOk;
    int32_t detail = 0; // 模块自定义附加码（例如 esp_err_t），0 = 无
};

} // namespace fw
