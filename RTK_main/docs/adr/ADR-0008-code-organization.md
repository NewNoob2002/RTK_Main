# ADR-0008 代码组织与编码规范重整（参考 X-TRACK）

状态：Proposed（分支 `refactor/xtrack-style`）
日期：2026-11-12

## 1. 背景

功能已经稳定（J5 PASS），但代码由多个 agent 分批写成，读起来费劲。具体问题：

| 问题 | 例子 |
| --- | --- |
| `main/` 25 个文件平铺，靠前缀区分 | `app_*`、`cmds_*`、`*_module`、`async_tx`、`console_support` 混在一起，看不出哪些属于同一个功能 |
| 一个功能散在多处 | 链路功能分在 `bt_link_module.*`、`cmds_link.cpp`、`app_data.*`、`app_ids.hpp`、`async_tx.*` 五处 |
| 模块写法啰嗦 | 自由函数 + `void *self` + 手写 `ModuleOps` 表 + 全局 `s_xxx_module`，每个模块重复一遍 |
| 命名空间 6 套 | `rtk::app`、`rtk::core`、`::app`、`demux`、`btrc`、`rtk::console` |
| include 根 3 种 | `rtk/core/x.hpp`、`app/x.hpp`、`demux/x.hpp`，另有平铺的 `rtk_bt_spp.h`、`rtk_store.h` |
| 文件头是流程信息不是说明 | 48 个文件以 `Generated-by: …` / `人工负责人：UNKNOWN` 开头，真正的用途说明反而在后面 |
| 超大文件 | `rtk_bt_spp.c` 1644 行，初始化、会话槽、收、发、GAP 全在一起 |

## 2. X-TRACK 两个分支的规范提炼

读的是 `main/Software/X-Track/USER/App`（LVGL 8，旧）和 `dev_lvgl_v9/Firmware/X-TRACK`（新）。新分支是作者整理过的版本，以它为准。

### 2.1 值得学的（结构层面）

1. **一个功能一个目录 / 一个文件。** `App/Service/DataProc/DP_Power.cpp` 就是电源的全部数据处理；`App/UI/Dashboard/` 就是仪表盘页面的全部。找代码只看目录名。
2. **定义与实现分离。** `DataProc/Def/DP_Power.h` 只放别人要用的类型（`Power_Info_t`、`enum class POWER_CMD`），实现文件不对外。别的模块只 include `Def/`。
3. **注册清单集中在一个文件。** `DataProc_NodeList.inc` 里一行 `DP_DEF(Power);` 就挂上一个节点，加功能只改这一处。
4. **模板目录。** `UI/_Template/`、`HAL/HAL_Template.cpp` 给出新增一个页面/设备的最小骨架，照抄即可。
5. **统一的事件入口。** 每个节点一个 `onEvent(param)`，`switch(param->event)`，先查 `param->size` 再拷贝，返回统一的 `RES_*` 码。
6. **类封装 + 静态跳板。** 节点是类，私有成员 `_xxx`；C 风格回调通过 `user_data` 跳回成员函数，不暴露 `void *`。
7. **分层固定：** `Frameworks/`（DataBroker、DeviceManager，与业务无关）→ `Service/`（HAL 设备、DataProc 业务节点）→ `UI/`。下层不 include 上层。
8. **常量成块并标单位。** `#define BATT_MAX_VOLTAGE 4100 // mV`，同一组放一起。
9. **公开 API 写 doxygen**（`@brief/@param/@retval`），实现里只在“为什么”处写注释。

### 2.2 不照搬的（与我们的约束冲突）

| X-TRACK 做法 | 不采用的原因 | 我们的替代 |
| --- | --- | --- |
| 方法 lowerCamel / 旧分支 PascalCase | 与 ESP-IDF、标准库、我们已有的 C 接口风格冲突，全量改名收益低 | 保持 `snake_case`，类型 `PascalCase`，常量 `kName` |
| 字符串 ID 订阅（`subscribe("Power")`）、`std::vector`、`new` | 运行时查找、堆分配；AGENTS.md 禁止 | 编译期枚举 / constexpr 表，静态存储 |
| DataBroker 无锁、单线程回调 | 我们是双核多任务 | 保留现有 Slot/FanoutRing/Executor |
| 每个设备 `virtual onRead/onWrite/onIoctl` | 虚函数仅限 Transport（CPP-011） | 模块用模板生成跳板，无虚表 |
| `#ifndef __X_H` 守卫 | 双下划线是保留标识符 | `#pragma once` |
| 函数体里大量 `LV_LOG_USER("begin")` | 噪声 | 只在状态变化和错误处打日志 |

结论：**照搬目录结构、Def/实现分离、注册清单、模板目录、统一事件入口这几条，可读性会明显变好；命名风格和内存模型不照搬。**

## 3. 目标结构

```
RTK_main/
├─ main/                          # 应用层：只做装配和业务模块
│  ├─ app_main.cpp                # 唯一入口：初始化 → 按清单注册模块 → 启动执行器
│  ├─ app_config.hpp              # 版本、执行器/任务参数（原 app_version/app_ids 的配置部分）
│  ├─ module_list.inc             # 模块清单：一行一个 MODULE_DEF(BtLink, kLink)
│  ├─ def/                        # 跨模块共享的类型（仿 DataProc/Def）
│  │  ├─ link_def.hpp             # 链路状态快照、统计结构
│  │  ├─ rtcm_def.hpp
│  │  └─ system_def.hpp
│  ├─ modules/
│  │  ├─ _template/               # 新增模块骨架（module + cmds + 说明）
│  │  ├─ bt_link/                 # bt_link_module.hpp/.cpp、bt_link_cmds.cpp、async_tx.hpp/.cpp
│  │  ├─ rtcm/                    # rtcm_module.*、rtcm_cmds.cpp、rtcm_sink.*（原 app_rtcm）
│  │  ├─ storage/
│  │  └─ system/
│  └─ support/                    # 控制台辅助等与具体模块无关的胶水
└─ components/
   ├─ 框架层  app_fw、rtk_os、rtk_console
   ├─ 驱动层  rtk_bt、rtk_store
   └─ 逻辑层  rtk_proto、stream_demux、rtk_core、bt_reconnect   # 纯逻辑，可主机测试
```

依赖方向：`main/modules` → 逻辑层 → 框架层；驱动层只被 `main/modules` 用。用 AGENTS.md 的依赖表 + CMake `REQUIRES` 保证。

## 4. 编码规范（落地版）

完整条文写在 `docs/CODING-STYLE.md`，这里列要点：

- **命名**：类型 `PascalCase`；函数/变量 `snake_case`；常量与枚举值 `kName`；私有成员 `name_`；C 接口 `rtk_<组件>_<动作>`。
- **命名空间**：统一 `rtk::<组件>`：`rtk::os`、`rtk::fw`（原 `::app`）、`rtk::console`、`rtk::bt`、`rtk::store`、`rtk::proto`、`rtk::demux`（原 `demux`）、`rtk::core`、`rtk::reconnect`（原 `btrc`）、`rtk::app`（main）。
- **include 路径**：C++ 统一 `#include "rtk/<组件>/<文件>.hpp"`；C 头保持 `rtk_<组件>_*.h`。
- **模块写法**（替代 `void *self` + 手写 ModuleOps）：

  ```cpp
  class BtLinkModule final {
  public:
      fw::Result init() noexcept;
      bool poll(uint32_t now_ms) noexcept;
      void command(const fw::Command &cmd, fw::CommandResult &out) noexcept;
  private:
      uint32_t last_tick_ms_ = 0;
  };
  // module_list.inc 一行：MODULE_DEF(BtLink, kLink) → 生成实例与 fw::make_ops<BtLinkModule>() 跳板表
  ```

  `fw::make_ops<T>()` 用 `if constexpr (requires { … })` 检测成员是否存在，缺省成员填 `nullptr`，零虚函数、零堆。
- **文件头**：第一行写“这个文件做什么”，第二行起写线程/调用约束；工具与模型来源移到 git 提交 trailer，不再写进源文件。
- **注释**：公开头文件每个函数一段 `/** … */`（做什么、谁调用、可否阻塞、错误返回）；实现里只写原因与约束。
- **常量**：同组常量放一块，行尾注明单位（`// ms`、`// B`）。
- **单文件规模**：建议 ≤ 500 行，超过则按职责拆分。

## 5. 分阶段落地

每一阶段单独提交，行为不变，门禁：`idf.py build` 0 告警、`test/host` 与各组件 ctest、`main_host_check`（普通 + ASan）、`sha256sum -c docs/frozen.sha256`；涉及运行时代码的阶段再做一次 HIL 冒烟（`status` + 40 KB/s 一轮）。

| 阶段 | 内容 | 风险 |
| --- | --- | --- |
| R0 | 本 ADR + `docs/CODING-STYLE.md` + AGENTS.md 规则更新 | 无 |
| R1 | `main/` 按 §3 搬目录（`git mv`，只改 include 和 CMake 源列表） | 低 |
| R2 | `fw::make_ops<T>()` + `module_list.inc`；四个模块改为类 | 中（主机场景测试覆盖） |
| R3 | 命名空间与 include 根统一（逐组件，一组件一提交；冻结清单重算） | 中（纯机械，编译器兜底） |
| R4 | 文件头与注释清理、公开 API doxygen 补齐 | 低 |
| R5 | `rtk_bt_spp.c` 按职责拆为 init / session / rx / tx / gap 五个文件 | 中（t13_host_check 29 例覆盖 + HIL） |
| R6 | `_template/` 模块骨架与“如何新增一个模块”说明 | 无 |

## 6. 待确认

1. 命名风格保持 `snake_case`（本 ADR 建议），还是改成 X-TRACK v9 的 lowerCamel？
2. 源文件头的 `Generated-by / 人工负责人` 是否删除、改由提交 trailer 记录？
3. R3 会改动冻结头文件（命名空间、include 路径），需要重算 `docs/frozen.sha256`，是否同意？
