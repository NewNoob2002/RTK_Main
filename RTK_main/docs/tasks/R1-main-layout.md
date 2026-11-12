# R1 main/ 目录重整（ADR-0008 §3、§5 R1）

执行方：pi + antigravity/gemini-3.8-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`、`main/` 全部文件、
`tools/main_host_check/`（CMakeLists、app_test_adapter.hpp、main_host_check.cpp）。

## 目标
`main/` 现在 25 个文件平铺。按“一个功能一个目录”重排，**只搬文件、拆分头文件、改 include 与 CMake 源列表，不改任何逻辑**。

目标布局（可按实际依赖微调，报告里说明理由）：

```
main/
├─ app_main.cpp                 # 入口与装配（保留原位）
├─ app_config.hpp               # 原 app_version.hpp + app_ids.hpp 中的全局常量、ModuleKind/ExecKind 与 id()
├─ CMakeLists.txt  Kconfig.projbuild
├─ def/                         # 跨模块共享类型（原 app_data.hpp 按功能拆）
│  ├─ link_def.hpp              # SessionSnapshot、LinkStatus、Sessions、AlarmState、Heartbeat、LinkStats、LinkVerb
│  ├─ rtcm_def.hpp              # DemuxStats、RtcmBatch、RtcmVerb
│  ├─ storage_def.hpp           # StoreStatus、StorageVerb
│  └─ system_def.hpp            # SystemStatus、Mode、SystemMode、SystemVerb
├─ data/app_data.hpp/.cpp       # DataCenter 实例/Topic 等（原 app_data 中除类型外的部分；include def/*）
├─ modules/
│  ├─ bt_link/  bt_link_module.hpp/.cpp  bt_link_cmds.cpp(原 cmds_link.cpp)  async_tx.hpp/.cpp
│  ├─ rtcm/     rtcm_module.hpp/.cpp     rtcm_cmds.cpp(原 cmds_rtcm.cpp)    rtcm_forwarder.hpp/.cpp(原 app_rtcm)
│  ├─ storage/  storage_module.hpp/.cpp  storage_cmds.cpp(原 cmds_storage.cpp)
│  └─ system/   system_module.hpp/.cpp   system_cmds.cpp(原 cmds_system.cpp)
└─ support/console_support.hpp/.cpp
```

规则：
- 用 `git mv` 搬文件（保留历史）；拆分 `app_data.hpp`、`app_ids.hpp` 时内容原样移动，不改类型定义。
- `app_cmds.hpp` 删除：每个模块的 `xxx_cmds()` 声明放进该模块的 `xxx_module.hpp`。
- `main/CMakeLists.txt` 仍只有 `INCLUDE_DIRS "."`；所有 include 写成从 main 根起的路径，如 `#include "modules/bt_link/bt_link_module.hpp"`、`#include "def/link_def.hpp"`。
- 命名空间、函数名、日志/控制台输出文本、常量值一律不变（这些是 R2/R3 的事）。
- 新建/拆出的头文件第一行写“这个文件做什么”（ADR-0008 §7）；被搬动的旧文件头暂不改（R4 统一处理）。
- 同步 `tools/main_host_check/CMakeLists.txt` 源路径及测试代码中的 include。

## 自检（写入 `docs/reports/R1.md`，逐项 PASS/FAIL）
1. `cmake -S tools/main_host_check -B builds/r1-mh && cmake --build builds/r1-mh && ctest --test-dir builds/r1-mh --output-on-failure` 29/29；ASan/UBSan 版（`builds/r1-mh-asan`，flags 同 AGENTS.md）29/29。
2. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B builds/r1 build`：0 warning。报告搬迁前（先用 `git stash` 以外的方式：在 `builds/r1-base` 用 HEAD 源码构建，或直接引用 `build/` 现有产物的 `idf.py size` 输出）与搬迁后 `idf.py size` 的 DRAM/IRAM/Flash 差异，应为 0 或仅字符串长度差，需解释。
3. `git diff -M --stat HEAD`：列出全部重命名；对“纯搬迁”的文件证明内容差异只有 include 行（`git diff -M HEAD -- <file>`）。
4. 从新旧源码提取控制台命令名、help 文本、printf 格式串对比，逐字节一致；附日志。
5. clang-format `--dry-run --Werror` 检查改动/新建文件。
6. 新布局的目录树（`tree main` 或等效）贴进报告。

## 并行约束
R0（`docs/CODING-STYLE.md`、`AGENTS.md`、`docs/adr/ADR-0002*`、`docs/acceptance.md`、`tools/g7b_check/*`）与 R5（`components/rtk_bt/*`、`tools/t13_host_check/*`）同时运行，你不得修改这些文件。
只能使用 `builds/r1*`，不得使用 `build/`。若 IDF 构建因 R5 半成品失败，等待后重试，仍失败写进报告。

## 禁止
修改 `components/`、`test/`、冻结文件；改逻辑；烧录、连接串口、联网安装、git commit（`git mv` 产生的暂存允许）。

完成后在报告最后一行写 `R1 DONE` 或 `R1 BLOCKED: 原因`。
