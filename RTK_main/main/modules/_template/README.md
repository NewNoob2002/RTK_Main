# 如何新增一个模块

本目录是 `RTK_main` 的新模块参考骨架，本身通过 `main/CMakeLists.txt` 排除在固件之外，由主机测试 `tools/main_host_check` 持续编译验证。请勿直接在本目录编写业务代码。

## 分步接入清单

下面以新增模块 `<name>`（例如 `demo` / `DemoModule` / `kDemo`）为例：

### 1. 复制骨架目录

将 `main/modules/_template/` 整目录复制为 `main/modules/<name>/`（如 `main/modules/demo/`），并删除新目录中的 `README.md`。
`main/CMakeLists.txt` 会通过 `file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "modules/*.cpp")` 自动发现新目录中的 `.cpp` 文件（只排除 `modules/_template/`）。

### 2. 全局改名与裁剪操作

在 `main/modules/<name>/` 内重命名文件与符号：

| 类别 | 模板标识 | 替换为（以 `demo` 为例） |
| --- | --- | --- |
| 文件名 | `template_module.hpp` / `template_module.cpp` / `template_cmds.cpp` | `demo_module.hpp` / `demo_module.cpp` / `demo_cmds.cpp` |
| 头文件引用 | `"modules/_template/template_module.hpp"` | `"modules/demo/demo_module.hpp"` |
| 模块类名 | `TemplateModule` | `DemoModule`（保持 `final`、禁拷贝/移动、`constexpr explicit DemoModule(AppContext &) noexcept`） |
| 状态结构体 | `TemplateState` | `DemoState` |
| 动词枚举 | `TemplateVerb`（可移入 `def/`） | `DemoVerb` |
| 命令表导出 | `template_cmds()` | `demo_cmds()` |
| 内部命令符号 | `kTemplateCmds` / `cmd_tmpl` / `"tmpl"` | `kDemoCmds` / `cmd_demo` / `"demo"`（含 `usage:` 与提示文本） |
| 命令目标占位 | `kTargetPlaceholder = 0xFFU` | 删去占位常量定义，`.target` 改为 `id(ModuleKind::kDemo)` |

`components/app_fw/include/rtk/fw/module.hpp` 的 `fw::make_ops<T>()` 检测以下 4 个**全部可选**的 `noexcept` 成员，不需要的直接删除声明，不要写空函数：
- `fw::Result init() noexcept`（启动前 `app_main` 单线程调用）
- `fw::Result start() noexcept`（执行器任务进入主循环前调用一次）
- `bool poll(uint32_t now_ms) noexcept`（执行器每轮非阻塞调用；返回 `true` 表示仍有积压，下一轮 wait 取 0）
- `void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept`（执行器收到命令时调用；只写 `result.status`/`result.detail`，不得修改 `result.seq`）

若模块不提供控制台命令，删除 `<name>_cmds.cpp` 与头文件中的 `<name>_cmds()` 声明即可。

### 3. 常量与跨模块类型（`main/def/<name>_def.hpp`）

- **跨模块共享类型/快照/命令动词**：新建 `main/def/<name>_def.hpp`，在 `namespace rtk::app` 下定义快照结构体、`enum class <Name>Verb : uint8_t` 及 `verb()` 转换函数（参考 `main/def/rtcm_def.hpp`）。
- **模块内部常量**：放在 `<name>_module.cpp` 的匿名命名空间中并标明单位（如 `kDefaultLimitMs`），避免裸数字触发 `.clang-tidy` 魔法数字检查。

### 4. 添加 `ModuleKind`（`main/app_config.hpp`）

在 `main/app_config.hpp` 的 `enum class ModuleKind : uint8_t` **末尾**追加 `k<Name>`（例如 `kDemo`）。

> 约束：注释已要求“枚举顺序即 ID，勿插入或重排”，只能在枚举末尾追加（当前末尾已有 `kSystemMode`）。若引入新执行器，同步在 `ExecKind` 末尾追加。

### 5. 注册一行 `MODULE_DEF`（`main/module_list.inc`）

1. 在 `#ifndef MODULE_DEF` 区域添加头文件包含：
   ```cpp
   #include "modules/<name>/<name>_module.hpp"
   ```
2. 在 `#else` 区域添加一行 6 参数宏展开：
   ```cpp
   MODULE_DEF(DemoModule, demo, "demo", kDemo, kLink, 0)
   ```

各列含义与顺序约束：

| 列 | 含义 | 约束与示例 |
| --- | --- | --- |
| 1. `类型` | 模块类名 | 位于 `rtk::app` 内，如 `DemoModule` |
| 2. `成员` | `AppContext` 成员名 | 小写蛇形，如 `demo` |
| 3. `名称` | 静态日志/诊断名 | 字符串字面量，如 `"demo"` |
| 4. `ModuleKind` | 模块编号枚举项 | 不带 `ModuleKind::` 前缀，同一执行器内不可重复，如 `kDemo` |
| 5. `ExecKind` | 所属执行器枚举项 | 不带 `ExecKind::` 前缀，如 `kLink` |
| 6. `poll_period_ms` | 轮询周期（ms） | `0` = 每轮调用；`>0` = 距上次调用 ≥ 该值才调用 |

> 顺序约束：`AppContext` 成员初始化以及执行器 `init()` / `start()` / `poll()` 遍历顺序严格按照 `module_list.inc` 的书写顺序执行。依赖前置模块的条目必须排在被依赖模块之后。

### 6. `main/app_context.hpp` 的自动展开

`AppContext::kModuleCount` 与成员 `type member{*this};` 均通过 `#include "module_list.inc"` 由 `MODULE_DEF` 自动展开，**常规模块无需改动 `main/app_context.hpp`**。

需要手改 `main/app_context.hpp` 的例外情况：
- 新模块使链路任务栈需求增大时，调整 `AppContext::kLinkStackBytes`（当前 `6144` 字节）。
- 新建了另一个 `ExecKind` 执行器时，需在 `AppContext` 手动添加对应的 `os::Executor` 和 `os::ExecutorStorage<...>` 成员，并在 `main/app_main.cpp` 中执行其 `init`/`start`。

### 7. 控制台命令表接入 `main/app_main.cpp`

`main/app_main.cpp` 的 `start_console()` 当前以定长数组集中注册控制台命令表。若新模块提供 `<name>_cmds()`，需修改 `main/app_main.cpp`（无需重复 `#include`，`app_context.hpp` 已通过 `module_list.inc` 间接引入各模块头文件）：

1. 将 `cmd_tables` 的 `std::array<std::span<const rtk::console::Cmd>, N>` 元素个数 `N` 加 `1`（当前为 `4`）。
2. 在初始化列表追加 `rtk::app::<name>_cmds(),`。
3. 确认全部命令表条目总数不超过 `rtk::console::kMaxCmds`（`24`，见 `components/rtk_console/include/rtk/console/console.hpp`）。

（若新模块需要在执行器 `init()` 前做一次性前置准备，再在 `app_main()` 中按顺序调用；普通模块无需改 `app_main()`。）

### 8. 发布数据到 `DataCenter`（Topic / Slot）

1. 在 `main/def/<name>_def.hpp` 定义纯数据快照结构体（可默认构造、按值拷贝、无悬垂指针）。
2. 在 `main/data/app_data.hpp` 包含 `def/<name>_def.hpp`，并在 `struct DataCenter` 中增加 `fw::Slot<DemoStatus> demo_status{};`。
3. 在 `main/data/app_data.cpp` 的 `DataCenter::init()` 中**务必**补上一行：
   ```cpp
   demo_status.bind_lock(lock);
   ```
4. 在模块内（`kLink` 任务中）写：`context_.data.demo_status.write(snapshot);`；只读控制台命令或订阅者读：`app_context().data.demo_status.read()`。

### 9. 需要持久化时的 `StoreKey`

- `components/app_fw/include/rtk/fw/store_key.hpp` 定义了描述表类型 `fw::StoreKey`、`fw::StoreType` 与编译期校验 `fw::valid()`、`fw::unique_keys()`（`ns` 与 `key` 均 ≤ 15 字符）。可按需在模块中声明 `inline constexpr fw::StoreKey kKeys[]` 并附 `static_assert` 校验。
- **现行实现注意**：目前 `StorageModule`（`main/modules/storage/storage_module.cpp`）尚未自动扫描各模块的 `kKeys[]`，NVS 读写仍通过 `components/rtk_store`（C11/BARR-C）的接口显式完成。因此新增持久化项时，除填写 `StoreKey` 元数据外，还需在 `components/rtk_store` 新增 C 存取函数并在 `StorageModule` 或模块内部接入。

### 10. 需要堆内存时的登记（`CODING-STYLE.md` §7）

- `kLink` 每轮轮询、TX 任务、蓝牙回调及 ISR **严禁**分配堆。
- 若非实时或启动期一次性路径确实需要堆，必须：
  1. 在对应 `.cpp` 文件第 2 行起的文件头写明档位、容量上限与分配失败处理策略；
  2. 将精确源文件路径登记到 `tools/g7b_check/heap_allowlist.txt`（未登记者会被 `g7b_check.py` 拦截）。

### 11. 提交前必跑检查清单

```sh
# 1. 目标固件零告警构建与堆/异常门禁
. ~/esp/esp-idf/export.sh >/dev/null
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r6 build
python3 -I tools/g7b_check/g7b_check.py --build builds/r6

# 2. 主机测试（普通 + ASan/UBSan）
cmake -S tools/main_host_check -B builds/r6-mhc && cmake --build builds/r6-mhc && ctest --test-dir builds/r6-mhc --output-on-failure
cmake -S tools/main_host_check -B builds/r6-mhc-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  && cmake --build builds/r6-mhc-asan && ctest --test-dir builds/r6-mhc-asan --output-on-failure

# 3. 格式、空白与冻结校验
clang-format --style=file --dry-run --Werror main/modules/<name>/*.hpp main/modules/<name>/*.cpp
git diff --check
sha256sum -c docs/frozen.sha256
```
