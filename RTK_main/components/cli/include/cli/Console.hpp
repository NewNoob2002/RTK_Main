// 控制台组件：esp_console REPL + 分组命令表注册 + 参数解析辅助。
// 只依赖 IDF（console、esp_system），不依赖任何自有业务组件，报警器固件也可复用。
// 用法：各模块提供 `inline constexpr cli::Cmd kCmds[] = {...}`；main 在 init 前逐表 add，
//       然后 init 一次、启动流程末尾 run 一次。命令回调在 console 任务中执行：只读命令直接读 DataCenter，
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
    bool hidden = false; // true：不在 `help` 列表中出现（工厂命令等），`help <name>` 仍可查看
};

inline constexpr size_t kMaxCmds = 24; // 全部表合计上限
inline constexpr size_t kMacLen = 6;

/**
 * 登记一张命令表（只保存指针，表必须是静态存储）。必须在 init/start 之前调用。
 * 空表 → ESP_OK；任一项 name/help/handler 为空 → ESP_ERR_INVALID_ARG；与已登记名称重复 → ESP_ERR_INVALID_STATE；
 * 合计超过 kMaxCmds → ESP_ERR_NO_MEM；init/start 之后调用 → ESP_ERR_INVALID_STATE。失败时不登记该表的任何一项。
 */
[[nodiscard]] esp_err_t add(std::span<const Cmd> cmds) noexcept;

/**
 * @brief  建立控制台：创建 REPL（UART/USB-CDC 按 sdkconfig 选择）、装好输出通道、注册 help 与全部已登记命令。
 *         REPL 任务创建后阻塞等待 run()，此时不打印提示符、不读输入；cli::out_* 与 ESP_LOG 已走统一出口。
 * @param  prompt: 提示符，静态字符串
 * @retval ESP_OK 成功；重复调用 → ESP_ERR_INVALID_STATE；其余为 IDF 错误码（失败时已释放 REPL）
 * @note   启动任务调用一次；须在其他任务启动前调用（会重装控制台 UART 驱动）。
 */
[[nodiscard]] esp_err_t init(const char *prompt) noexcept;

/**
 * @brief  放行 init() 创建的 REPL 任务：打印 IDF 提示语与 prompt 并开始读命令。启动流程的最后一步调用。
 * @retval ESP_OK 成功；未 init 或已运行 → ESP_ERR_INVALID_STATE
 */
[[nodiscard]] esp_err_t run() noexcept;

/** @brief init(prompt) + run() 的组合（测试应用等无需分阶段的场景）。 */
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

/* ---- 统一回执与子命令（ADR-0009 §2.5）----
 * init() 为每条命令注册一个 trampoline：handler 返回 0 → 打印 "OK"；返回非 0 且本次调用过 fail() →
 * fail() 已打印 "ERR <原因>"；返回非 0 但没调用 fail() → 打印 "ERR <返回值>"。trampoline 总是向 esp_console
 * 返回 0。回执状态是 console 任务私有的（命令只在 REPL 任务中串行执行）。
 */

inline constexpr int kCmdFail = 1; // fail() 的返回值，handler 直接 return

/** 打印 "ERR <fmt…>" 并标记本次命令已给出原因；返回 kCmdFail。只在命令 handler 中调用。 */
int fail(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

/** 一个子命令（动词）；name/usage/help 指向静态存储。 */
struct Verb {
    const char *name = nullptr;  // 动词，例如 "list"
    const char *usage = nullptr; // 参数用法，例如 "<MAC>"；无参数为 nullptr
    const char *help = nullptr;  // 一行说明
    Handler handler = nullptr;   // argv[0] 是动词本身
};

/**
 * 子命令分派：argv[0] 为组名，argv[1] 为动词。找到 → 以 (argc-1, argv+1) 调用该动词的 handler；
 * 没有动词且 default_verb 非空 → 按 default_verb 执行；否则打印本组用法并返回 fail("usage")。
 */
int dispatch(std::span<const Verb> verbs, void *ctx, int argc, char **argv,
             const char *default_verb = nullptr) noexcept;

/** 打印一组的用法（`help <组>` 与 dispatch 失败时使用）。 */
void print_usage(const char *group, std::span<const Verb> verbs) noexcept;

/**
 * 登记一组的动词表，供 `help` 与 `help <组>` 显示（组名必须已经或将要经 add() 登记）。
 * 只保存指针；必须在 init/start 之前调用；超过 kMaxCmds → ESP_ERR_NO_MEM。
 */
[[nodiscard]] esp_err_t add_verbs(const char *group, const char *summary,
                                  std::span<const Verb> verbs) noexcept;

/* ---- 分节输出辅助（status、sys 等使用，统一对齐与单位）---- */

/** 打印节标题，例如 "[link]"。 */
void print_section(const char *title) noexcept;
/** 打印一行键值：键左对齐到固定宽度，值按 fmt 格式化。 */
void print_kv(const char *key, const char *fmt, ...) noexcept __attribute__((format(printf, 2, 3)));

} // namespace cli
