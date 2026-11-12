// 控制台组件：esp_console REPL + 分组命令表注册 + 参数解析辅助。
// 只依赖 IDF（console、esp_system），不依赖任何自有业务组件，报警器固件也可复用。
// 用法：各模块提供 `inline constexpr cli::Cmd kCmds[] = {...}`；main 在 start 前逐表 add，
//       然后 start 一次。命令回调在 console 任务中执行：只读命令直接读 DataCenter，
//       改状态命令经 Executor::call 投递到所属执行器（ADR-0007 §4.3），不得直接改模块状态。
// 冻结接口（ADR-0007 §3.3）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_err.h"

namespace cli {

/** 返回值即 esp_console 的命令返回码：0 成功，非 0 由 REPL 打印 "Command returned non-zero error code"。 */
using Handler = int (*)(void *ctx, int argc, char **argv) noexcept;

/** 一条控制台命令的定义；name/help/hint 必须指向静态存储（命令表只保存指针）。 */
struct Cmd {
    const char *name = nullptr; // 命令名（分组名），例如 "status"、"alarm"、"wl"；静态字符串
    const char *help = nullptr; // 多行帮助，静态字符串
    const char *hint = nullptr; // 参数提示，可为 nullptr
    Handler handler = nullptr;
    void *ctx = nullptr; // 传给 handler，通常是模块实例
};

inline constexpr size_t kMaxCmds = 24; // 全部表合计上限
inline constexpr size_t kMacLen = 6;

/**
 * 登记一张命令表（只保存指针，表必须是静态存储）。必须在 start 之前调用。
 * 空表 → ESP_OK；任一项 name/help/handler 为空 → ESP_ERR_INVALID_ARG；与已登记名称重复 → ESP_ERR_INVALID_STATE；
 * 合计超过 kMaxCmds → ESP_ERR_NO_MEM；start 之后调用 → ESP_ERR_INVALID_STATE。失败时不登记该表的任何一项。
 */
[[nodiscard]] esp_err_t add(std::span<const Cmd> cmds) noexcept;

/**
 * 创建 REPL（UART/USB-CDC 按 sdkconfig 选择，与现有 app_console 一致）、注册内置 help 和全部已登记命令并启动。
 * prompt 为静态字符串。重复调用 → ESP_ERR_INVALID_STATE。
 */
[[nodiscard]] esp_err_t start(const char *prompt) noexcept;

/* ---- 参数解析辅助（纯函数，失败返回 false 且不改 out） ---- */
/** 十进制或 0x 前缀十六进制 → u32；空串/非法字符/溢出 → false。 */
[[nodiscard]] bool parse_u32(const char *s, uint32_t &out) noexcept;
/** 同上，但超出 0..255 → false。 */
[[nodiscard]] bool parse_u8(const char *s, uint8_t &out) noexcept;
/** "AA:BB:CC:DD:EE:FF"（大小写均可）。 */
[[nodiscard]] bool parse_mac(const char *s, std::span<uint8_t, kMacLen> out) noexcept;
/** 偶数个十六进制字符 → 字节，长度超过 out.size() → false；written 为写入字节数。 */
[[nodiscard]] bool parse_hex(const char *s, std::span<uint8_t> out, size_t &written) noexcept;

} // namespace cli
