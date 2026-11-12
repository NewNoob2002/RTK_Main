# J3 rtk_console 组件 + 命令表按模块拆分（ADR-0007 §3.3、§4.3）

执行方：pi + antigravity/gemini-3.8-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`（§3.3、§4.3）、
`docs/reports/J1.md`、冻结头 `components/rtk_console/include/rtk/console/console.hpp`，以及现有 `main/app_console.{hpp,cpp}`、`main/app_ids.hpp`。

## 交付
1. `components/rtk_console/CMakeLists.txt` 与 `components/rtk_console/src/console.cpp`：实现冻结头全部函数。
   - `add`/`start` 语义严格按头文件注释（原子登记：失败不登记该表任何一项；重复名、超过 `kMaxCmds`、start 之后调用的错误码）。
   - `start`：REPL 配置沿用现有 `main/app_console.cpp`（UART0 115200、prompt、max_cmdline_length、栈、优先级、绑 core 0、`tx/rx_gpio_num=-1`），
     这些参数作为组件内部常量或 Kconfig（`components/rtk_console/Kconfig`，默认值与现值相同）。命令用 `esp_console_cmd_t::func_w_context` 注册，ctx 透传。
     注册或启动失败时按现有逻辑 `repl->del` 清理。
   - 解析辅助：`parse_u32`（十进制 / `0x` 十六进制，溢出、空串、尾随字符 → false）、`parse_u8`、`parse_mac`、`parse_hex`，失败不改 out。
   - 依赖只允许 `console`、`esp_system` 及串口相关 IDF 组件；不得依赖 `rtk_*` 或 `app_fw`（AGENTS.md §3）。
2. 命令表按模块拆分（文件归 main，不改模块实现文件）：
   - `main/console_support.{hpp,cpp}`：从 `app_console.cpp` 搬出 `submit()`/`report()` 等公共辅助（行为与输出文本不变）。
   - `main/cmds_link.cpp`（alarm、pair、wl、policy）、`main/cmds_storage.cpp`（sn）、`main/cmds_rtcm.cpp`（rtcm）、`main/cmds_system.cpp`（status），
     每个文件导出一张 `std::span<const rtk::console::Cmd>`（声明放 `main/app_cmds.hpp`）。数字参数解析改用 `rtk::console::parse_*`，取值范围与错误提示保持不变。
   - `main/app_main.cpp`：逐表 `rtk::console::add` 后 `rtk::console::start`；删除 `main/app_console.{hpp,cpp}`；更新 `main/CMakeLists.txt`。
   - 全部命令名、帮助文本、输出文本（含 `stats:`、`rtcm:` 两行 printf 格式，上板脚本依赖）**逐字节不变**。
3. host 测试 `test/rtk_console/`：只测解析辅助（把 `src/console.cpp` 中解析函数放到可单独编译的 `src/parse.cpp`，不含 IDF 头）；普通 + ASan/UBSan。
4. 报告 `docs/reports/J3.md`。

## 自检（报告逐项 PASS/FAIL）
1. `sha256sum -c docs/frozen.sha256` 全部 OK（J2b 并行期间允许 `test/host/CMakeLists.txt`、`test/host/test_link_service.cpp`、`test/host/test_command_dispatcher.cpp`、`components/rtk_core/include/rtk/core/command_dispatcher.hpp` 报 FAILED/缺失，那是 J2b 的授权改动，报告里注明即可）。
2. `test/rtk_console` 普通 + ASan/UBSan 通过。
3. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B build-j3 -D SDKCONFIG=build-j3/sdkconfig build`：0 warning；报告 DRAM 静态与 app 剩余（对比 J1：76,709 B / 62.37%）。
4. 用脚本从新旧源码中提取全部命令名、help 字符串和 printf 格式串做对比，只允许文件位置变化；附对比日志。
5. clang-format 检查改动文件。

## 并行约束
J2b 同时运行，它修改 `components/rtk_core/*`、`main/bt_link_module.cpp`、`test/host`、`tools/main_host_check`。
你不得修改这些文件；默认 `build/` 与根目录 `sdkconfig` 属于 J2b，你只能用 `build-j3/`。构建若因 J2b 的半成品失败，等待后重试，仍失败写进报告。

## 禁止
修改冻结文件（含 `console.hpp`）；修改 `main/*_module.*`、`main/app_ids.hpp`、`main/app_data.*`（确需改动时停下写进报告）；
烧录、连接串口、联网安装、git commit。

完成后在报告最后一行写 `J3 DONE` 或 `J3 BLOCKED: 原因`。
