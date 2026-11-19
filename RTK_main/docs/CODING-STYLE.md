# RTK_main 编码规范

本规范面向读代码和新增功能的人，依据 ADR-0008 §3、§4、§6–§9；C/C++ 分工沿用 ADR-0003。
硬性门禁、依赖允许表、冻结范围和操作授权见 `AGENTS.md`，告警与格式配置仍由工具维护。
R0–R6 迁移已全部完成，下文描述的是现行规则；与现状不一致的旧代码按「改到哪补到哪」处理，
批量整改另立任务卡（见 ADR-0013 提议的可读性整改）。

## 1. 目录与文件

### 1.1 按功能组织，装配入口保持简单

一个功能一个目录，模块实现、命令处理及其私有辅助文件放在一起。
`AppMain.cpp` 只做初始化、按清单注册模块、启动执行器。组件依赖以 `AGENTS.md` 的允许表和 CMake
`REQUIRES` 为门禁；纯逻辑组件不得因目录或命名空间统一而引入 IDF/FreeRTOS/产品依赖。

这样写（ADR-0008 §3 的目标结构）：

```text
main/
  AppMain.cpp
  AppConfig.hpp                # 版本、执行器与任务参数
  module_list.inc               # 一行注册一个模块
  def/                          # LinkDef.hpp、RtcmDef.hpp、SystemDef.hpp
  modules/
    _template/                  # 模块、命令处理与新增模块说明
    bt_link/                    # BtLinkModule.*、BtLinkCmds.cpp
    rtcm/                       # RtcmModule.*、RtcmCmds.cpp、RtcmForwarder.*
    storage/
    system/
  support/                      # 与具体模块无关的控制台辅助等胶水
components/
  app_fw/  rtos/  cli/            # 框架层
  spp_server/  nvs_store/         # 驱动层
  device_proto/  stream_demux/    # 逻辑层，支持 host 测试
  link_core/  bt_reconnect/
```

不要这样写：

```text
main/
  BtLinkModule.cpp            # 同一功能散在根目录
  cmds_link.cpp
  async_tx.cpp
  all_modules.cpp              # 混合多个功能
```

### 1.2 共享定义与实现分离

`def/` 只放跨模块确实共享的类型、枚举和快照，不放驱动操作或模块私有状态；
其他模块通过共享定义或已有队列/Slot 交互。单文件建议不超过 500 行，超过时按职责拆分，避免机械切段。

```cpp
// 这样写：main/def/LinkDef.hpp
struct LinkSnapshot {
    bool connected = false;
};
// 不要这样写：共享头暴露内部发送循环
void run_bt_tx_forever();
```

### 1.3 注册集中，新增模块从模板开始

新增模块复制 `main/modules/_template/`（详见 [`main/modules/_template/README.md`](../main/modules/_template/README.md)），实现必需操作，再在 `AppConfig.hpp`、`module_list.inc` 及有命令时的 `AppMain.cpp` 完成注册。
不要在入口、多个命令文件中各维护一份模块列表。

```cpp
// 这样写：module_list.inc
MODULE_DEF(BtLink, kLink)
// 不要这样写：在每个调用点重复填写实例和回调表
register_module(&s_bt_link_module, &kHandwrittenBtLinkOps);
```

## 2. 文件头与生成来源

第一行用一句中文说明文件做什么；第二行起说明谁调用、可否阻塞、是否 ISR 安全。
使用堆的文件还须注明第 7 节的档位、容量和失败策略。文件头不写工具/模型或人工负责人。
脚本同样以用途、调用约束开头，本工程通过 `python3` 执行脚本。

```cpp
// 这样写：汇总链路状态并处理链路控制命令。
// init 由启动任务调用；poll/command 由链路执行器串行调用，不阻塞，非 ISR 安全。
// 堆档位：禁止区；本文件不分配堆。

// 不要这样写：把流程信息放在用途说明之前
// Generated-by: 某工具 + 某模型
// 人工负责人：UNKNOWN
```

来源写在 git 提交信息的 trailer，模型标识以任务卡为准；不得添加 `Signed-off-by`。
本条是提交者的记录规则，不授予实现方提交权限。

```text
这样写（提交 trailer）：
Generated-by: pi + codex/gpt-6.1-sol

不要这样写（源文件头）：
// Generated-by: pi + codex/gpt-6.1-sol
```

## 3. 命名与 include

### 3.1 C++ 用 lowerCamelCase（ADR-0013 §2.1，2026-11-17 起）

类型用 `PascalCase`（即配置中的 `CamelCase`）；函数、方法、变量、参数、数据成员用 `lowerCamelCase`；
常量、枚举值和命名空间作用域只读对象用 `kName`；私有/受保护成员加后缀 `_`（`lastTickMs_`）；
文件作用域可变静态保持 `s_snake_case`（`s_link_state`）并放在匿名命名空间；
缩写按单词处理（`parseGga`、`rtcmMsg`），单位后缀保留（`nowMs`）。C 文件、C 接口头与 `extern "C"` 声明仍用 `snake_case`（§8）。
迁移期间（S2 分批）尚未改名的层仍是 `snake_case`，以 `docs/tasks/S2-rename.md` 的批次进度为准。
宏全大写。不用 `p_`、`b_` 等类型前缀，也不用 `g_` 表示文件静态。

**源文件名（2026-11-16 用户确认）**：工程自有的 C/C++ 源文件与头文件（`.c`/`.h`/`.cpp`/`.hpp`）一律用
`PascalCase`，按下划线分词逐段首字母大写：`gnss_module.cpp` → `GnssModule.cpp`、`spp_server.h` → `SppServer.h`、
`test_link_service.cpp` → `TestLinkService.cpp`。没有对应类的文件同样适用（`GnssCmds.cpp`、`TraceCodes.hpp`）。
以下保持原样：
- 目录名（组件目录、include 根 `fw/`、`core/` 等）——IDF 以目录名作组件名；
- 主机测试中模拟 IDF/FreeRTOS 的 shim 头（如 `shim/esp_err.h`），必须与被替代的系统头同名；
- `reference/` 归档代码与 `docs/reports/` 历史记录；
- CMake、Kconfig、`*.inc`、Python 脚本、CMake/ctest 目标名（仍为 `snake_case`）。
CMake 里由 `snake_case` 名推导文件名时用 `cmake/pascal_case.cmake` 的 `rtk_pascal_case()`。
新增文件不得与已有文件仅大小写不同。

```cpp
// 这样写
struct LinkState { bool connected = false; };
enum class LinkEvent { kConnected, kClosed };
constexpr uint32_t kRetryMs = 1000; // ms
namespace {
constinit LinkState s_link_state{};
}
void publishLink(uint32_t nowMs) noexcept;
// 类内：uint32_t lastTickMs_ = 0;

// 不要这样写
class link_state;
void PublishLink(uint32_t now_ms);
// 类内：uint32_t _lastTick;
```

### 3.2 统一命名空间与 include 根

ADR-0008 §9 的对照如下。组件按用途命名，统一名称不改变职责与可复用性。
公共基础错误码位于 `app_fw` 的 `fw/Status.hpp`，使用 `fw::Status`、`fw::isOk`、`fw::toString`，枚举值及字符串映射不变。
C 头文件名与符号前缀取组件前缀（`dev_proto_*`、`spp_server_*`、`nvs_store_*`）；NVS 命名空间 `"rtk"`、键值与 `RTK_*` Kconfig 选项保持不变。

| 组件目录 | 命名空间 | C++ include 根 |
| --- | --- | --- |
| `rtos` | `rtos` | `rtos/` |
| `app_fw` | `fw` | `fw/` |
| `cli` | `cli` | `cli/` |
| `spp_server` | `spp` | `spp/` |
| `nvs_store` | —（仅 C 接口） | —（C 头平铺） |
| `device_proto` | `proto` | `proto/` |
| `stream_demux` | `demux` | `demux/` |
| `link_core` | `core` | `core/` |
| `bt_reconnect` | `reconnect` | `reconnect/` |
| `main` | `app` | `app/`（对外 C++ 头） |

```cpp
// 这样写（R3 目标）
#include "fw/Module.hpp"
namespace app {
// 应用模块
}
// 不要这样写（新增接口缺少组件根）
#include "Module.hpp"
namespace unrelated_app {
}
```

### 3.3 命名空间：按组件划分，层次服务于可读性

- 每个组件有自己的命名空间，按上表映射，例如 `fw`、`demux`、`reconnect`。
- 不得引入顶层 `alarm`、`link`、`sync`，避免与 POSIX 函数冲突。
- 子命名空间可以用，前提是读调用点时更清楚：协议常量表 `proto::msg_type::kQuery`、一个解码器一个子空间、`detail`。总深度不超过 `a::b::c` 三层。
- 同一组常量如果本来就是“取值集合”，优先 `enum class`；如果是协议里的一张号码表、需要当整数用，放进命名空间也可以。
- `.cpp` 中为跨组件长名写的 `namespace x = …` 别名允许，但只在 `.cpp` 局部使用，头文件不写；同一个别名在多个文件反复出现，说明命名空间该调整了。
- 头文件不写 `using namespace`。
- **类型名不跨组件重名（2026-11-16 规则 4）**：不同组件里语义不同的类型不用同一个名字，否则调用点必须写
  `::gnss::` 全限定、读者也分不清是哪个。新增类型先在全工程查重。存量 `gnss::Reply` 已在 GNSS 驱动重构
  （P3）中改名为 `gnss::ReplyVerdict`（`components/gnss/include/gnss/Receiver.hpp`），与 `core::Reply` 不再重名。

## 4. 类与函数内容

### 4.1 一个类只做一件事，成员按阅读顺序排列

顺序为 public 类型/常量 → 构造 → 公开方法 → private 方法 → 数据成员。
构造必须 `constexpr`、不失败、不访问硬件/RTOS/日志；初始化工作放在显式 `[[nodiscard]] init()`。
独占资源的类删除拷贝，全局对象 `constinit`。运行期多态仅限 `Transport`、`demux::Sink` 和
`gnss::Receiver` 驱动层次（ADR-0012 §2.8）：每个非叶子类（基类、可派生的家族类）都用受保护非虚析构，
只实例化 `final` 叶子类；不写公有虚析构（它会生成引用 `operator delete` 的删除析构函数）；继承不超过两层。
模块不增加虚函数。

```cpp
// 这样写（公开方法注释见第 6 节）
class LinkModule final {
public:
    using Snapshot = LinkSnapshot;
    constexpr LinkModule() noexcept = default;
    [[nodiscard]] fw::Result init() noexcept;
private:
    void publish() noexcept;
    Snapshot snapshot_{};
};
// 不要这样写：构造时启动 RTOS，数据和操作混排
class MixedModule {
    uint32_t tick_;
public:
    MixedModule() { start_task(); }
    virtual void poll();
};
```

### 4.2 函数保持短小，先检查再走主流程

建议函数不超过 60 行、嵌套不超过 3 层；一个函数保持同一抽象层次。
先校验参数与状态，使用提前返回（guard clause），最后保留直线主流程。
60 行是阅读建议，`.clang-tidy` 当前 100 行阈值是另一条机械门禁，不表示鼓励写满。

```cpp
// 这样写
if (!ready_) {
    return fw::Status::kInvalidState;
}
if (payload.empty()) {
    return fw::Status::kInvalidArg;
}
return send_payload(payload);
// 不要这样写
if (ready_) {
    if (!payload.empty()) {
        // 很长的发送流程包在多层条件里
    }
}
```

### 4.3 事件与命令使用 switch 分派

统一入口使用 `switch`，每个分支调用一个具名处理函数，包含 `default`。
处理函数先验证长度、范围和状态，再拷贝或解码；不得把字节缓冲区强转成结构体。

```cpp
// 这样写
switch (verb) {
case LinkVerb::kReset:
    return handle_reset(payload);
default:
    return fw::Result::kNotSupported;
}
// handle_reset 中先检查，再按字段解码
if (payload.size() != kResetPayloadBytes) {
    return fw::Result::kInvalidArg;
}
// 不要这样写
if (verb == LinkVerb::kReset) {
    const auto &request = *reinterpret_cast<const ResetRequest *>(payload.data());
    // 未检查长度、对齐、字节序，且入口里堆叠业务逻辑
}
```

### 4.4 可恢复错误返回错误码，同一错误只记一次日志

可失败函数返回 `fw::Status`/`Result` 并加 `[[nodiscard]]`。错误在发生处记一次日志，上层只传错误码；
纯逻辑层不依赖日志平台，由第一个有日志能力的适配边界记录一次。
IDF 返回值必须检查，`esp_err_t` 只在 `rtos::fromEspErr` 映射；
禁止用 `ESP_ERROR_CHECK` 处理可恢复错误或用 `(void)` 丢弃 `esp_err_t`。
ISR、C 回调蹦床、析构和移动操作标 `noexcept`。失败路径释放已获得的资源，避免部分更新。

```cpp
// 这样写：init 已在有日志能力的边界记录错误
const auto status = transport.init();
if (!isOk(status)) {
    return status;
}
// 不要这样写：重复记录，或把失败直接忽略
if (!isOk(status)) {
    ESP_LOGE("link", "link: init_failed_again");
}
(void)esp_some_fallible_call();
```

**失败返回类型（2026-11-16 规则 1）**：一个函数只用一种方式表达失败，按所在层选择：

| 返回类型 | 用途 |
| --- | --- |
| `bool` | 只用于谓词（`is_*`/`has_*`/`can_*`、`poll()` 是否还有工作），`false` 不代表“出错” |
| `fw::Status` | 组件内部与组件之间的可恢复错误（驱动、管道、存储绑定、`bind_storage`） |
| `fw::Result` | `app_fw` 模块接口：`fw::Module` 生命周期、`StoreBinding` 编解码、命令处理结果 |
| `esp_err_t` | 只出现在 IDF/C 接口边界，进入 C++ 前立即用 `rtos::fromEspErr` 映射 |

- 不用 `const char *` 返回失败原因、`nullptr` 表示成功；需要诊断文本时返回错误码，原因字符串由常量表提供（规则 2）。
- 新代码强制执行；存量代码改动到哪个函数就顺带迁移哪个，不做一次性全量改动（GNSS 部分在 P3 迁移）。

### 4.5 参数表达所有权与长度，输出放最后

只读大对象传 `const T &`，连续数据传 `std::span<const T>`（轻量视图按值传递）；输出参数放最后。
小标量按值传；业务 API 不用裸 `void *` 传上下文，只有 C/框架跳板可以恢复已知对象类型。
以下 `static_cast` 必须在同行或上一行注释说明数值范围或类型恢复为何安全：窄化（宽→窄、浮点→整数）、符号变化（有符号↔无符号）、指针↔整数、枚举↔整数、`void *` 恢复对象类型；拓宽且同符号的转换不强制注释（Q2 决定：原规则覆盖所有转换无法执行，存量不批量补，改到哪处补哪处）。C++ 不用 C 风格转换。

```cpp
// 这样写
[[nodiscard]] fw::Status decode(std::span<const uint8_t> wire, Reply &reply) noexcept;
[[nodiscard]] fw::Status apply(const Settings &settings) noexcept;
// 不要这样写：长度和类型都不明确，输出在前
fw::Status decode(Reply *reply, void *data);
fw::Status apply(Settings settings); // 复制大对象
```

## 5. 日志、常量与并发

### 5.1 日志记录状态变化、错误与周期统计

组件与应用模块统一用 `fw/Log.hpp`（C 组件用 `fw/LogC.h` 的 `FW_CLOG*`，ADR-0009 §2.12）：调用处直接写 `FW_LOGx(fw::TraceCat::kXxx, ...)`。
行格式见 ADR-0009 §2.12：`[t] [CAT] <sub> 句子`。类别名（`GNSS`、`LINK`…）由输出端加在行首，正文不再重复；
正文以小写子标签 `<sub>` 开头（GNSS 用 `<sm>`/`<probe>`/`<cfg>`/`<out>`/`<uart>`/`<rx>`/`<rtcm>`/`<app>`，其他组件用自身名），
后接一句完整的话；`k=v` 只用于列多个数值的清单；收发方向写 `TX`/`RX`。级别：

- `E`：功能失效，需要人工处理（保存失败、初始化失败、不可恢复的协议错误）；
- `W`：异常但已自动恢复或降级（重试、丢弃、回退）；
- `I`：状态变化与周期统计（连接/断开、探测结果、配置生效）；
- `D`：定位问题时才需要的细节（单条命令收发、解析中间值）。

每个类别单独设级别，默认 I（`debug log <类别|all> <e|w|i|d>`），与 trace 开关无关；因此默认就能看到的 I 级只写状态变化，
逐帧/逐句内容用 D。实时每轮路径、ISR、BT 回调任务只用 `fw::trace()`（16 B 事件，不格式化），
不写日志。不逐次打印函数进入/退出，不打印蓝牙数据内容（需要时用 `tlogHex`，受 hex 掩码控制）。
禁止在固件自有代码中调用 `ESP_LOG*`/`printf` 输出日志；只允许 `esp_log_level_set`/`esp_log_set_vprintf` 控制 IDF 自身日志。

```cpp
// 这样写
FW_LOGI(fw::TraceCat::kGnss, "<probe> Detected %s at %lu baud.", modelName(m), baud);
FW_LOGW(fw::TraceCat::kGnss, "<cfg> Step %u was rejected, retry %u.", step, retry);
// 不要这样写
FW_LOGI(fw::TraceCat::kGnss, "GNSS: begin poll"); // 每轮噪声，重复类别名，无子标签
```

### 5.2 常量按用途分组并注明单位

协议字段、容量、时间等有业务含义的数字必须命名，同组常量放在一起；单位写在名称及行尾注释。
0、1 等直接表达空值、计数起点的数字无需套上无意义的名字。

```cpp
// 这样写
constexpr uint32_t kRetryDelayMs = 1000; // ms
constexpr uint32_t kConnectTimeoutMs = 5000; // ms
constexpr size_t kTxCapacityBytes = 512; // B
// 不要这样写
if (now_ms - last_ms > 5000) {
    retry_after(1000);
}
```

**诊断字符串集中成表（2026-11-16 规则 2）**：会被保存、比较或对外显示的原因字符串（降级原因、停机原因、
故障注入原因）放在一个常量表里，调用点只引用常量，例如 `main/app/BootReasons.hpp` 的
`app::boot_reason::kLinkInit`。这样同一原因不会写出两种拼法，也能一眼看全所有可能取值。
一次性日志文本不受此限。

**位掩码使用具名位（2026-11-16 规则 3）**：每一位都有名字，不写 `mask |= 1U`、`2U`、`4U`。
位集合用 `enum class X : uint32_t` 定义，需要原子运算的掩码保持 `uint32_t`，经 `constexpr` 转换函数取位：

```cpp
// 这样写（main/AppContext.hpp）
enum class BootTask : uint32_t { kLink = 1U << 0U, kIo = 1U << 1U, kTrace = 1U << 2U };
launched |= taskBit(BootTask::kLink);
// 不要这样写
launched |= 1U; // 哪个任务？
```

### 5.3 明确线程所有权与实时边界

链路状态由链路任务独占，其他任务通过队列/快照交互。SMP 临界区使用 `portMUX_TYPE` 和守卫，
其间不调用 FreeRTOS API、不阻塞；不得以 `volatile` 同步。任务静态创建、显式绑核并注释原因，
IDF 栈大小单位为字节。蓝牙回调不阻塞、不解析、不打印数据内容。

```cpp
// 这样写：只在短临界区更新共享计数
{
    const rtos::CriticalSection guard(stats_mux_);
    ++stats_.events;
}
// 不要这样写
volatile bool ready = false; // 不能建立跨核同步关系
// 持有 stats_mux_ 时调用 xQueueReceive(..., portMAX_DELAY)
```

### 5.4 全局可变状态：允许，但要有主人、成组、看得出生命周期

全局变量和文件静态变量不是禁止项。单实例的驱动、模块、应用顶层对象放在文件作用域，往往比层层传参更好读（Arduino `BluetoothSerial.cpp` 的驱动状态就是文件静态）。判断标准是读代码的人能否一眼看出：**这份状态归谁、什么时候初始化、谁会改它**。

可以：
- 单实例对象放文件作用域：`constinit` 初始化，放匿名命名空间或 `static`，名字带 `s_` 前缀。例如模块实例、执行器栈、C 驱动的 `s_spp`。
- 跨文件共享的少量对象，用 `extern` 暴露**一个**对象或用访问函数，例如 `data::center()`。
- 一个概念的多个字段收进一个结构体，例如 RTCM 批缓冲 + 长度 + 计数 → `struct RtcmBatch`，不要拆成 5 个并列全局。

避免（读者需要跨文件拼图才能理解的写法）：
- “存储区 + 全局指针 + init 里 placement new 后赋值”成对出现：能 constexpr 构造就直接定义对象；必须延迟构造时用一个 `fw::Lazy<T>`/`std::optional<T>`，不要裸指针。
- `extern` 后再用 `inline constexpr auto &kX = s_x` 换名二次暴露。
- 只被一个函数使用的状态提到文件作用域：应该放进函数或类成员。
- 同一份状态被多个任务写：要么只有一个写者，要么在旁边注明锁或临界区。
- 一个文件里堆积大量互相关联的全局（参考：`BluetoothSerial.cpp` 的 30 多个 static 加上 `#define` 位标志，阅读时很难看出哪些状态成组变化）：先按“对象/概念”合并成结构体或类成员。

是否把模块全局收进类、收进 `AppContext`，按可读性决定，不追求“全工程只剩一个全局”。

## 6. 模块与注释

### 6.1 模块类 + 模板跳板 + 一行清单注册

模块以类保存状态，按需要提供 `init`、`start`、`poll`、`command`。
`fw::makeOps<T>()` 用 `if constexpr (requires { ... })` 检测成员，未提供的操作填 `nullptr`；
框架内部生成静态跳板，业务方法不暴露 `void *self`。实例静态存储，零虚表、零堆。
实现见 `components/app_fw/include/fw/Module.hpp` 的 `makeOps`。

```cpp
// 这样写：命名空间 app 中，fw 指通用 app_fw 组件
class BtLinkModule final {
public:
    constexpr BtLinkModule() noexcept = default;
    [[nodiscard]] fw::Result init() noexcept;
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    void command(const fw::Command &cmd, fw::CommandResult &out) noexcept;
private:
    uint32_t last_tick_ms_ = 0;
};
// 清单的一行展开为静态实例与 fw::makeOps<BtLinkModule>() 跳板表
MODULE_DEF(BtLink, kLink)

// 不要这样写：业务层重复恢复指针并手工维护操作表
void bt_link_command(void *self, const fw::Command &cmd, fw::CommandResult &out);
const fw::ModuleOps kOps{init_trampoline, nullptr, poll_trampoline, bt_link_command};
```

### 6.2 每个函数都写 doxygen 注释（2026-11-16 用户要求，门禁 `tools/doc_check`）

**范围**：每个函数，包括公开 API、类的私有方法、`static`/匿名命名空间里的内部函数、构造/析构函数。
豁免只有：lambda；`= default`/`= delete`；覆写函数（`override`）在契约与基类声明一致时可不写，
由基类声明处的注释覆盖，行为有差异时必须写。

**位置**：公开函数写在头文件声明处，`.cpp` 定义处不重复；只在 `.cpp` 里出现的函数写在定义处。
同一函数只写一份，避免两处说法不一致。

**格式**：标签用英文，正文用中文（保留必要英文术语）。

```cpp
/**
 * @brief  在列表中查找节点
 * @param  vec: 列表指针
 * @param  id:  节点 ID
 * @retval 找到时返回节点指针；找不到返回 nullptr
 * @note   只在 io 执行器任务调用；不可在 ISR 中调用
 */
DataNode *search_node(std::span<DataNode *> vec, NodeId id) noexcept;
```

- `@brief`：必填，一句话说明做什么（不是怎么做）。
- `@param name: 说明`：每个具名参数一条；输出参数写 `@param[out]`，写明调用失败时它的内容。
- `@retval`：返回值不是 `void` 时必填。返回 `bool`、`fw::Status`、`fw::Result`、`esp_err_t` 时
  每种结果各写一条（如 `@retval kOk 成功`、`@retval kBusy 上次请求未完成`）。
- `@note`：有约束时写——调用线程/任务、是否可在 ISR 中调用、是否阻塞、只能在启动阶段调用、持锁要求。

**实现内注释**只解释为什么、约束与副作用，不复述语句，不保留注释掉的代码。

```cpp
// 不要这样写
// 初始化
fw::Status init();
// count 加一
// ++old_count;  // 历史代码应交给版本控制
```

**门禁**：`python3 -I tools/doc_check/doc_check.py --base <基线提交>` 检查相对基线新增或改动的函数，
不合格即失败；`--report` 按组件统计覆盖率，`--list <路径前缀>` 列出某个组件的全部不合格函数。
改动到的函数必须补齐。存量缺口（2026-11-17 `--report`：1293 个函数中 315 个合格，24%）按 ADR-0013
的分批任务卡补齐；全部补齐后 `--report` 合计 100% 成为最终门禁，此后不再只查增量。

## 7. 堆内存分级（ADR-0008 §8，2026-11-12 任务卡确认采用）

### 7.1 先判断调用路径，再选择分配方式

| 档位 | 路径 | 规则 |
| --- | --- | --- |
| 禁止区 | ISR、蓝牙回调、链路任务每轮路径、发送任务 | 禁止分配堆；使用静态存储、固定容量缓冲区或调用者存储；避免释放造成不可预测耗时 |
| 启动期一次性 | 任务启动前的初始化 | 允许一次分配，之后不释放；规定容量与失败处理，初始化成功后只用既有存储 |
| 非实时限量 | 控制台、配置导入导出、OTA、文件解析等非实时路径 | 允许有明确上限的分配，检查失败、明确所有者并释放；不能因入口叫“控制台”就豁免其在链路任务执行的处理函数 |

禁止区检查沿调用链生效；文件登记白名单不能让其中的实时函数分配。
C 文件继续遵守既有 `malloc`/`free` 禁令；下列 C++ 分级不是修改 C 协议/驱动内存语义的授权。
其余被禁容器和特性不因本节解禁。

```cpp
// 这样写：实时路径使用固定容量
std::array<uint8_t, kTxCapacityBytes> tx{};
// 不要这样写：即使源文件已登记，发送任务仍禁止分配
std::vector<uint8_t> tx(payload.begin(), payload.end());
```

### 7.2 异常关闭时显式处理分配失败

`std::vector`/`std::string` 分配失败会走 `abort` 并重启，不能靠 `try/catch` 恢复。
必须处理失败的地方使用 `new (std::nothrow)` 并检查空指针、返回错误码。
也可在允许档位预先 `reserve`，使用时限制长度不超过已保留容量，避免随后增长分配；
**`reserve` 自身仍可能 abort，它不是失败处理机制**。若初次分配失败也必须恢复，应选择 nothrow 存储方案。
`new`/`delete`、`malloc`/`free`、`<vector>`、`<string>` 均须符合档位与白名单要求；
placement new 只构造于已有存储，不分配堆，仍须保证容量、对齐和生命周期。

```cpp
// 这样写：仅用于已登记的非实时限量路径；kImportMaxBytes 为固定上限
if (size > kImportMaxBytes) {
    return fw::Status::kInvalidArg;
}
auto *buffer = new (std::nothrow) uint8_t[size];
if (buffer == nullptr) {
    return fw::Status::kNoSpace;
}
const auto status = load_and_parse_import({buffer, size});
delete[] buffer;
return status;
// 不要这样写：关闭异常后无法可靠返回内存不足
std::string buffer(size, '\0');
```

### 7.3 按精确源文件登记，不按目录放宽

使用堆的文件必须在文件头标档位，并在 `tools/g7b_check/heap_allowlist.txt` 登记仓库相对路径，
行内 `#` 写档位与理由、上限及失败策略。初始白名单为空，不预先豁免未来文件。
G7b 从 `compile_commands.json` 映射目标文件到源文件，逐项列出放行的源文件与堆符号；
未登记则 FAIL，无法确认映射不放行。该门禁不能证明调用路径属于正确档位，仍需审查。
异常/RTTI 禁令和全 ELF 精确白名单/固定拉入链独立生效。

```text
# 这样写（示例路径，只有对应文件实际存在且审查通过时才可登记）
main/modules/storage/config_import.cpp # 非实时限量；配置导入最多 4096 B，nothrow 失败返回 kNoSpace
# 不要这样写
main/modules/storage/* # 任意文件与任意调用路径
```

## 8. C 文件规则（整理既有规则，不改变语义）

### 8.1 命名与 C/C++ 边界

C11 子集；公共符号/宏带组件前缀（`dev_proto_*`、`DEV_PROTO_*`、`spp_server_*`、`nvs_store_*`）；
文件静态变量 `s_`，类型 `_t`；不用 `p_`/`g_`/`b_`。
头文件 `#pragma once`，用 `__cplusplus` 保护 `extern "C"`，只含 C 构造；C 不调用 C++。
布局、错误检查与并发规则同样适用；C 中不用 `malloc`/`free`、不用 `goto`，内部函数 `static`。

```c
/* 这样写：C 公共头（省略具体类型定义） */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
dev_proto_err_t dev_proto_decode(const uint8_t *data, size_t len);
#ifdef __cplusplus
}
#endif
/* 实现内部：static uint32_t s_frame_count = 0U; */
/* 不要这样写：公共 C 头包含 C++ 构造 */
/* class Decoder;  std::span<const uint8_t> decode(); */
```

### 8.2 保持 BARR-C 安全核心

每个 `switch` 有 `default`；不混用有/无符号；窄化转换写范围注释；变量使用前初始化；
不将缓冲区强转为结构体指针；多字节字段逐字节小端读写；循环有界。
纯 C 错误返回本模块枚举，依赖 IDF 的 C 模块检查并传播 `esp_err_t`，不在 C 中调用 C++ 映射器。

```c
/* 这样写：调用方已检查 data 至少有两个字节 */
uint16_t value = 0U;
/* 两个 uint8_t 拼成 0..65535，窄化到 uint16_t 安全。 */
value = (uint16_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8U));
/* 不要这样写：布局、对齐和字节序均未经保证 */
/* const frame_t *frame = (const frame_t *)data; */
```

## 9. 格式与静态检查交给工具

布局不在此重复维护：日常用仓库 `.clang-format`，C++ 静态检查用 `.clang-tidy`，C 用 `.clang-tidy-c`；
权威格式门禁是 `clang-format --style=file`（仓库 `.clang-format`，用户 2026-11-13 决定）；IDF 的 astyle 规则与
`.clang-format` 冲突（续行缩进、`T {x}` 空格），不得用 astyle 改写本仓库文件，仅可作参考。
生成文件（脚本输出、厂商生成代码）第一行写明用途和生成脚本路径（如 `由 <脚本> 生成`），不要手改，不参加格式门禁，
由生成脚本保证可复现（技能 GEN-007；当前唯一一处是 `test/stream_demux_dev/h2/H2Vectors.h`，排除写在
`docs/acceptance.md` G5）。
格式工具不能替代线程契约、命名语义、内存档位及错误路径审查；不得通过放宽告警或改冻结配置消除发现。

```sh
# 这样写：检查实际改动文件
clang-format --style=file --dry-run --Werror path/to/changed.cpp
clang-tidy -p builds/host path/to/pure.cpp
clang-tidy -p builds/host --config-file=.clang-tidy-c path/to/pure.c
# 不要这样写：用忽略退出状态隐藏发现
# clang-tidy path/to/pure.cpp || true
```

## 附录：从 X-TRACK 学了什么、改了什么

来源为 ADR-0008 §2；参考 `main/Software/X-Track/USER/App` 与
`dev_lvgl_v9/Firmware/X-TRACK`（本地参考目录名 `v9`），新分支为主要结构参考。

| X-TRACK 做法 | 本工程采用或调整 |
| --- | --- |
| 一个功能一个目录/文件 | 采用功能目录，超 500 行按职责拆分 |
| `DataProc/Def` 分离共享类型 | 使用 `main/def/`，实现与私有状态留在模块目录 |
| `DataProc_NodeList.inc` 一行注册 | 使用 `module_list.inc`，避免多份注册表 |
| `_Template` 最小骨架 | 提供 `_template/`，新增模块从最小必需操作开始 |
| `onEvent` + switch + 长度检查 | 采用统一入口与先校验后解码，每分支调用具名函数 |
| 类 + 静态回调跳转 | 采用模块类与 `fw::makeOps<T>()`，业务方法隐藏 `void *` |
| 固定分层 | 保留允许依赖表与 host 可测的纯组件边界 |
| 成组常量/单位、doxygen 注释 | 采用并扩大到每个函数（§6.2，`tools/doc_check` 门禁）；实现内注释解释原因 |
| lowerCamel 方法、前缀私有成员 | 方法采用 lowerCamelCase（ADR-0013 起）；私有成员用后缀 `name_` 而非前缀 |
| 字符串 ID 订阅、通用堆容器 | 用枚举/constexpr 表、静态存储；堆仅在允许档位按文件登记 |
| 单线程无锁 DataBroker | 保留 Slot/FanoutRing/Executor 和双核同步契约 |
| 每个设备虚函数 | 模块用模板跳板；多态仅限既有 Transport/Sink |
| 双下划线 include guard | 使用 `#pragma once`，避免保留标识符 |
| 函数进入日志 | 只记录状态变化、错误与周期统计 |

```cpp
// 这样写：借鉴统一入口，保持本工程命名和类型 ID
switch (event) {
case LinkEvent::kClosed:
    handle_closed();
    break;
default:
    break;
}
// 不要这样写：把参考工程的字符串订阅与命名一并照搬
// node->subscribe("Power");  void onEvent(void *param);
```

## 10. 与 embedded-engineering 技能的对照（技能 v0.1）

技能版本以 `SKILL.md` frontmatter 的 `metadata.version` 为准（源仓库 `/home/gtc/Desktop/workspace/Tools/Skills`，v0.1 = 提交 `3254fb6`）。Q1 审计读取的是 git `5cae6d9` 加当时尚未提交的工作区改动（含 CPPF-001/CPPF-002/DOC-001），这些改动随后定为 v0.1；单凭 `5cae6d9` 复现不出本表依据的规则。

本表由 Q1 审计（2026-11-17）建立：只列对本工程适用或需要说明不适用的技能规则。状态取值：
**覆盖**（已落地且有门禁/证据，全量代码证据由 Q4 复核，见 `docs/reports/Q4.md`）、**有意偏离**（本工程规则更具体且有理由）、**缺口**（技能要求尚无对应做法，
Q1 待决提议的处理结果见 `docs/tasks/Q2-conformance-fixes.md`）、**不适用**（本工程无该场景）。规则优先级见技能 `SKILL.md`：
项目显式规则高于技能默认值，所以「有意偏离」不等于错误，但理由必须写在「依据或理由」列。

| 技能 Rule ID | 技能要求（一句话） | 本项目做法 | 状态 | 依据或理由 |
| --- | --- | --- | --- | --- |
| GEN-001 | 烧录/擦除/eFuse/量产密钥/执行器上电等不可逆动作需授权与恢复路径 | 授权边界写在 AGENTS §6；报告逐项记 PASS/FAIL/NOT_RUN/BLOCKED | 覆盖 | AGENTS.md §6 |
| GEN-002 | 硬件事实须有仓库来源或数据手册，查不到写 UNKNOWN | AGENTS §1 事实表带来源列；PSRAM 记 UNKNOWN | 覆盖 | AGENTS.md §1 |
| GEN-003 | 证据分 PASS/FAIL/NOT_RUN/BLOCKED，仿真不等于硬件 | 每份 `docs/reports/` 用同一套结论词，HIL 未授权记 NOT_RUN | 覆盖 | docs/acceptance.md |
| GEN-004 | 提交不带 agent `Signed-off-by`，生成代码记录工具与模型并指定人类 owner | `Generated-by: pi + <模型>` trailer，禁止 Signed-off-by | 覆盖 | AGENTS.md §2 |
| GEN-005 | 架构级改动走 Phase 1 审批 | ADR 由架构方批准后才落地 | 覆盖 | docs/adr/*.md |
| GEN-006 | 测试基准来自规格，不取自实现输出 | 解码向量由脚本独立算 CRC/长度 | 覆盖 | test/stream_demux_dev/h2/gen_vectors.py |
| GEN-007 | 厂商生成代码不重排、不进门禁 | `reference/` 只读不参与构建；生成物 `test/stream_demux_dev/h2/H2Vectors.h` 不参加格式门禁，规则见 §9 | 覆盖 | §9、docs/acceptance.md G5 |
| GEN-008 | 向上游提交只用上游规格 | 本仓库无上游提交 | 不适用 | — |
| GEN-009 | 外设等待有界、时钟在该阶段有效、tick 回绕安全 | BT 初始化按阶段位 + 超时；时间差用无符号运算 | 覆盖 | components/spp_server/SppServerInit.c、components/rtos/ |
| GEN-010 | 校验外部输入的长度与范围 | 帧长在进缓冲前判上界 | 覆盖 | components/stream_demux/、components/device_proto/ |
| DOC-001 | 新增自有函数都要 Doxygen（默认英文），声明或定义写一处，类注释不算 | §6.2 比技能更严：每个函数都要，正文中文；门禁只查增量，存量 1293 个中 315 个合格（24%，2026-11-17） | 缺口（存量）；正文语言为有意偏离 | §6.2；AGENTS.md §5 规定中文正文；存量按 ADR-0013 分批补齐 |
| CPPF-001 | 布局按项目选定的风格 | 仓库 `.clang-format` | 有意偏离 | AGENTS.md §5：clang-format 为唯一门禁 |
| CPPF-002 | 命名按项目风格 | §3 + ADR-0013 §2.1（readable-cpp lowerCamelCase；文件静态保留 `s_`） | 覆盖（S2 迁移中） | §3 |
| ARCH-007 | 驱动状态放设备上下文，分段初始化逐个查返回 | GNSS 驱动实例层次 + `init()` 返回 Status | 覆盖 | components/gnss/ |
| C-001 | 不用 volatile 做任务同步 | §5.3 同款；`components/cli/src/Out.cpp` 的 volatile 双重检查在 Q2 改为原子一次性初始化 | 覆盖 | §5.3、Q2 |
| C-002 | 模块外部以外的函数/变量 static | 文件作用域 `s_` 静态放匿名命名空间 | 覆盖 | §3 |
| C-003 | 不留注释掉的代码 | 审查项 | 覆盖 | — |
| C-004 | 宽度敏感处用定宽整数 | 协议字段全定宽 | 覆盖 | components/device_proto/ |
| C-005 | 不对有符号数做位运算 | 掩码一律 `uint32_t` | 覆盖 | main/AppContext.hpp |
| C-006 | 不混用有/无符号比较与算术 | 纯组件开 `-Wsign-conversion`；IDF 组件靠 `rtk_strict_warnings` 重开 `-Wsign-compare` | 覆盖 | cmake/strict_warnings.cmake |
| C-007 | 浮点不用 `==`/`!=` | 浮点只出现在日志格式化 | 覆盖 | — |
| C-008 | 硬件/帧结构体注意对齐与位序 | 帧按字节解析，不做结构体强转 | 覆盖 | §8 |
| C-009 | 少用参数宏，括号、单次求值、不做控制转移 | 日志宏族 + `MODULE_DEF` 注册 | 覆盖 | §6 |
| C-010 | 使用前初始化、判空 | `{}` 初始化 + `constinit` | 覆盖 | §4 |
| C-011 | 不在 if 条件里赋值 | 审查项 | 覆盖 | — |
| C-012 | 每个 switch 都有 default | §8 强制 | 覆盖 | §8 |
| C-013 | 有意 fallthrough 加注释 | `-Wimplicit-fallthrough` | 覆盖 | 组件编译选项 |
| C-014 | 不用隐式函数声明 | `-Werror` | 覆盖 | — |
| C-016 | 头文件用 `#ifndef` 保护 | 本项目用 `#pragma once` | 有意偏离 | 技能 ESP-006 已声明该例外 |
| ISR-001/002/003/004/006 | ISR 只取事件、W1C 直接赋值、ISR↔任务靠队列/环形/通知、FromISR API、时间预算测量 | 自有代码无 ISR：UART/GPIO 全轮询 | 不适用 | — |
| RTOS-001 | 周期任务用绝对延时 | 执行器按 `now - last >= period` 比较，无漂移 | 覆盖 | components/rtos/Executor.cpp |
| RTOS-002 | 每任务栈预算、溢出检测、高水位测量 | 栈字节常量 + canary + `uxTaskGetStackHighWaterMark` | 覆盖 | main/AppContext.hpp、Executor::stats_ |
| RTOS-003 | 内核对象静态创建，初始化后不再分配 | 任务/信号量静态；堆仅 BootAlloc 启动期 | 覆盖 | components/rtos/BootAlloc.cpp |
| RTOS-004 | 一个外设/总线一个所有者任务 | link 执行器拥有 BT，io 执行器拥有 GNSS UART | 覆盖 | main/app/Executors.cpp |
| RTOS-005 | 看门狗只在关键子系统推进后喂 | 每轮末尾喂，复位失败即停 | 覆盖 | components/rtos/Executor.cpp |
| DMA-001/002 | D-cache 对齐、ISR 只通知 | 无 DMA | 不适用 | — |
| CPP-001 | T0–T2 关异常/RTTI/cxa_atexit，ELF 内无异常运行时 | `-fno-exceptions -fno-rtti`；ELF 仅 6 个白名单符号 | 覆盖 | docs/acceptance.md G7b |
| CPP-002 | 无 RTTI typeinfo | 同上 | 覆盖 | 同上 |
| CPP-003 | 堆分档：实时路径禁止、T1 只启动期、T2 固定池或注入分配器 | §7 三档 + g7b 白名单；全仓只有 `BootAlloc` 一处启动期分配，不提供注入分配器 API | 有意偏离 | §7、tools/g7b_check/heap_allowlist.txt；单一分配点不值得建分配器接口（Q2 决定） |
| CPP-004 | 不用 `std::function` | 模块用 ops 结构体 + 静态蹦床 | 覆盖 | §6 |
| CPP-005 | T0 不用分配容器，T1/T2 有界 | 只用 `std::array`/`std::span`/`std::string_view` | 覆盖 | §4 |
| CPP-006 | 不用 iostream | 控制台走 `cli::outPrintf` | 覆盖 | §5.1 |
| CPP-007 | printf 只用于非实时路径，T0 禁 | `snprintf` 只出现在命令/日志格式化 | 覆盖 | §5.1 |
| CPP-008 | 共享原子要 `static_assert(is_always_lock_free)` | 共享原子变量旁写 `static_assert(fw::kAtomicUsable<T>)`（`fw/Atomic.hpp`）：有原生原子指令的目标必须无锁，RISC-V 无 A 扩展时接受 IDF 关中断模拟（B7） | 覆盖 | main/AppContext.hpp、components/app_fw/src/、components/cli/src/Out.cpp、BtLinkModule.cpp |
| CPP-009 | 函数内静态的动态初始化受限 | 函数内静态与全局表须常量初始化；g7b 检查自有目标文件不得引用 `__cxa_guard_*` | 覆盖 | components/gnss/src/Registry.cpp、docs/acceptance.md G7b（Q2） |
| CPP-010 | 全局/静态构造不访问硬件/RTOS/日志，显式 `init()` | `constexpr` 构造 + `[[nodiscard]] init()` | 覆盖 | §4.1 |
| CPP-011 | 多态基类受保护非虚析构、实现类 `final` | 虚函数只限 `Transport`/`demux::Sink`/`gnss::Receiver`；非叶子类受保护非虚析构，只实例化 `final` 叶子 | 缺口（整改中） | §4.1；P3s 起 `UnicoreReceiver`/`UbloxReceiver`/`ComnavReceiver` 用公有虚析构且被直接实例化，整改见 ADR-0013 S1 |
| CPP-012 | volatile 只用于 MMIO/DMA，不做复合运算 | 无 MMIO volatile | 覆盖 | — |
| CPP-013 | ISR 向量 `extern "C"`、C 回调 `noexcept` 蹦床、跨边界类型 standard-layout | 无自有 ISR；BT 回调经 C 函数转发 | 覆盖 | components/spp_server/ |
| CPP-014 | SDK 错误码在 bsp 层映射一次并检查 | `esp_err_t` 只在 IDF 边界，`rtos::fromEspErr` 映射 | 覆盖 | §4.4 |
| CPP-015 | 自有代码开严格告警 + `-Werror`，第三方头用 `-isystem` | `rtk_strict_warnings()` + 组件选项集 | 覆盖 | cmake/strict_warnings.cmake |
| CPP-017 | 不把位域用于寄存器映射 | 无位域映射 | 覆盖 | — |
| CPP-020 | 全局用 `constinit`/`constexpr` | 执行器与模块表 `constinit` | 覆盖 | main/app/Executors.cpp |
| TEST-001 | 测试基线：MCU C 用 Ceedling 等；已有框架可延续 | host 用 Unity + CTest，目标侧用 IDF Unity | 覆盖 | test/、test_apps/ |
| TEST-002 | 新增/修改的 C 公共函数要有测试（正常/参数错误/依赖错误/边界） | C 侧用例集中在 device_proto/stream_demux，nvs_store 无 host 测试（需先对 nvs_flash 打桩） | 缺口 | 已知缺口，单独开卡 |
| TEST-004 | 报告 Flash/RAM 增量与 `cpp_cost.py diff` | 报告给镜像字节、基线差与 `cpp_cost.py diff <基线 elf> <新 elf>` 结果 | 覆盖 | docs/acceptance.md G4（Q2 起） |
| TEST-006 | C++ 公共函数同样要有测试 | 纯组件各有 host 测试目录 | 覆盖 | test/ |
| FSM-001 | 每个状态都显式处理意外事件 | 重连/链路状态机的 switch 都有 default | 覆盖 | components/bt_reconnect/ |
| PROTO-001 | 访问前校验帧长与剩余缓冲，校验通过才执行命令 | 解码器先校验长度/CRC 再交付 | 覆盖 | components/stream_demux/ |
| PROTO-002 | 显式端序转换，不对齐缓冲不强转结构体 | 多字节字段逐字节小端读写 | 覆盖 | §8 |
| ERR-001 | assert 只用于程序员错误，release 失败进安全态 | 全仓库无 `assert` | 覆盖 | Q1 审计 grep |
| FAULT-001 | 启动记录复位原因，故障处理器写 noinit RAM/保留 flash | 记录并打印 `reset_reason`，无自有故障处理器 | 覆盖 | main/support/ResetReason.cpp |
| DBG-001 | 调试器 halt/reset/flash/option bytes 属硬件动作需授权 | AGENTS §6 禁止 | 覆盖 | AGENTS.md §6 |
| BOOT-001 | 跳转前校验镜像哈希 | 无自有 bootloader/现场升级 | 不适用 | — |
| BOOT-002 | 掉电安全更新 + 回滚 | 本期不做现场升级；分区表保留 `ota_0`/`ota_1` 只为兼容 E1-5.4 布局 | 有意偏离（延后） | ADR-0002 Q2 补充决定 |
| BOOT-003 | 反回滚计数器 | 同上 | 不适用 | — |
| RV-001 | RISC-V 目标原子 RMW 走临界区 | 产品固件只构建 esp32；esp32c3（RV32IMC，无 A 扩展）上 `std::atomic` 由 IDF 的 `__atomic_*` 关中断模拟实现，即临界区语义，单核下任务与 ISR 均安全；`rtk_pure` 双架构编译验证 | 覆盖 | `fw/Atomic.hpp`；ADR-0002 §偏离 |
| ESP-001 | SMP 临界区用自旋锁，临界区内不阻塞 | `portMUX_TYPE` + `taskENTER_CRITICAL` | 覆盖 | §5.3 |
| ESP-002 | 关 cache 期间的 ISR 必须 IRAM 安全 | 无此类 ISR | 不适用 | — |
| ESP-003 | 栈按字节、绑核并写明理由、单核降级 | 栈字节常量 + 每处绑核注释 | 覆盖 | main/AppContext.hpp、main/app/Executors.cpp |
| ESP-004 | 每个 `esp_err_t` 都检查，可恢复错误不用 `ESP_ERROR_CHECK` | 无 `ESP_ERROR_CHECK`、无 `(void)` 丢弃 | 覆盖 | Q1 审计 grep |
| ESP-005 | eFuse/Secure Boot/Flash Encryption/JTAG 关闭需授权 | AGENTS §6 禁止 efuse | 覆盖 | AGENTS.md §6 |
| ESP-006 | IDF 风格：4 空格、函数花括号换行、120 列、`s_` 前缀、组件前缀、`#pragma once`、Astyle | 除列宽（ADR-0013 改为 100）外前五项已落地，格式工具改用仓库 `.clang-format` | 有意偏离 | AGENTS.md §5：astyle 与 `.clang-format` 冲突 |
| ESP-007 | OTA 至少两个分区 + otadata + 开启回滚 + mark valid | 有 otadata/`ota_0`/`ota_1`；本期不做 OTA，未开回滚 | 有意偏离（延后） | ADR-0002 Q2 补充决定：做 OTA 时开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 并在首次连通后 mark valid |
| ESP-008 | Xtensa 与 RISC-V 都要能编，用 `CONFIG_IDF_TARGET_*`/`SOC_*` 隔离 | 产品固件只 esp32（经典蓝牙限制），纯组件经 `test_apps/rtk_pure` 双架构编译 | 有意偏离 | ADR-0002:70,76 |
| ESP-009 | 用锁定版本的 IDF API，不关默认 `-Werror` | IDF v6.1 固定路径；无托管组件，IDF 不生成 `dependencies.lock` | 覆盖 | AGENTS.md §1、.gitignore 注释（Q2） |
