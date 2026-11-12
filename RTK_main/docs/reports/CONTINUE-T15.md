# T15 后续验收、TIDY-FIX 复审与电源保持

日期：2026-11-11。执行者：Codex；人工负责人 UNKNOWN（待人工审查签字）。Profile：esp-idf。用户要求 T15 验收后安排 TIDY-FIX 复审及烧录上板，随后确认 ESP32、`/dev/ttyACM0`，并要求开机电源保持。

## 交付与结论

- T15 已有 `T15-REVIEW.md` 的独立软件验收 PASS，本轮复核两个已有验收构建目录成功；目标运行仍未执行。
- 已委派独立 QA 完成 TIDY-FIX 复审，软件建议 PASS，详见 `TIDY-FIX-REVIEW.md`，含独立干净构建、G5b/G7b、22 项驱动回归。没有 HEAD，不能声称核对过可信的变更前逐行 diff；复审保存了修改前源码摘要。
- 新增电源保持：`main/app_main.cpp` 在任何 NVS/蓝牙初始化之前，先预置 GPIO26 高电平，再配置为无上下拉、无中断的输出；两个 IDF API 均检查返回值，失败记录日志并停止本次 app_main 初始化。`main/CMakeLists.txt` 显式依赖 `esp_driver_gpio`。仅驱动用户本轮要求的电源保持引脚。
- 电源保持独立复核见 `POWER-HOLD-REVIEW.md`：真实 app_main 配最小 GPIO/初始化替身，成功路径先 GPIO 再初始化、level 失败、config 失败共 3 场景实际通过；此为 host 替身证据，不是实板电平验证。
- 来源：只读旧工程 `../../E1-5.4/main/main.cpp:90` 定义 GPIO26，`main/src/Begin.cpp:81-82` 设置 OUTPUT 并写 1；本地 IDF `esp_driver_gpio/src/gpio.c:249-253,351-478` 支持先预置锁存器、再选择 GPIO 输出。无猜测极性/寄存器；不提供关机、深睡或 bootloader 修改。
- 上板具体顺序、备份/恢复条件与判据写入 `HIL-PLAN.md`。硬件阶段被自动审批钩子阻断，未执行串口或烧录。

## 本轮主线程命令与结果

每个构建 shell 先 `. ~/esp/esp-idf/export.sh >/dev/null`；IDF 构建使用 `IDF_COMPONENT_MANAGER=0`。没有安装、下载、git commit/push 或修改冻结文件。

| 命令/检查 | 状态 | 实际结果 |
|---|---|---|
| `sha256sum -c docs/frozen.sha256`（改动前；改动后输出存日志） | PASS | 115/115 OK；`logs/CONTINUE-T15/frozen.log` |
| T15 目录：`idf.py -B build-accept-esp32 build`，随后 `idf.py -B build-accept-esp32c3 build` | PASS | 均退出 0，无 warning/error；本轮为增量复核，原独立干净验收见 T15-REVIEW。二进制分别 135920 / 138144 字节；构建自身 `log/` 保存输出 |
| 两目录 `project_description.json` 检查 | PASS | 目标分别 esp32/esp32c3，自有组件仅 rtk_proto/rtk_core，不引入产品外设组件；镜像摘要见 prepared.sha256 |
| `cmake -S test/host -B /tmp/rtk-continue-host && cmake --build /tmp/rtk-continue-host -j 4 && ctest --test-dir /tmp/rtk-continue-host --output-on-failure` | PASS | 新目录独立构建，8/8 可执行文件、93 用例、0 Failures/0 Ignored |
| `cmake -S test/host -B /tmp/rtk-continue-asan -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' -DCMAKE_C_FLAGS='-fsanitize=address,undefined' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' && cmake --build /tmp/rtk-continue-asan -j 4 && ctest --test-dir /tmp/rtk-continue-asan --output-on-failure` | FAIL（环境） | 构建成功；沙箱内 8/8 因 `LeakSanitizer has encountered a fatal error` / `does not work under ptrace` 退出，非已定位的产品错误；另有 make jobserver FIFO 已存在诊断，不影响构建完成 |
| 沙箱外审批机制：`ctest --test-dir /tmp/rtk-continue-asan --output-on-failure` | PASS | 未关闭/修改 sanitizer；8/8、93 用例、0 Failures/0 Ignored，无 sanitizer 报告；`asan-escalated-tests.log` |
| `clang-format --style=file -i main/app_main.cpp` 及 `--dry-run --Werror main/app_main.cpp` | PASS | 退出 0；仅格式化本次修改文件 |
| `idf.py -B build-power-hold build`（新目录） | PASS | 完整 1363 步，退出 0，0 warning/error；bin 625328 字节；`power-build.log` |
| `python3 -I tools/tidy_check/tidy_check.py --host-build /tmp/rtk-continue-host --idf-build build-power-hold --test-app-build test_apps/rtk_pure/build-accept-esp32` | PASS | 27 文件，0 发现；`power-tidy.log` |
| `python3 -I tools/g7b_check/g7b_check.py --build build-power-hold` | PASS | 自有 17 对象通过，ELF 异常/RTTI 6/6 精确白名单及拉入链通过；`power-g7b.log` |
| `idf.py -B build-power-hold size size-components` | PASS | app 小于 1.2 MB，分区剩余 63%；完整组件占用见 `power-size.log` |
| 只读串口设备路径枚举 | BLOCKED | PreToolUse 的 `rtk_main_guard` 拒绝，理由 AGENTS.md §6 禁止连接串口。未改用其他工具/权限重试 |
| pytest 依赖 METADATA 文本搜索 | BLOCKED | 同钩子判为 pytest-embedded 访问开发板而拒绝；未能核实依赖安装状态，未尝试运行或安装 |
| 上板烧录、串口采集、HIL、恢复 | NOT_RUN | 硬件阶段整体 BLOCKED；备份状态 UNKNOWN，供电保持的实测时序未知 |
| astyle_py 权威格式检查 | NOT_RUN | 不安装/下载；保留现有规则 |

本轮持久化日志在 `docs/reports/logs/CONTINUE-T15/`；编译产物保留在各自构建目录。源文件和候选完整镜像的 SHA-256 在 `prepared.sha256`，无提交 SHA 可以替代。

## 契约偏离及剩余边界

用户本轮对 GPIO26 电源保持的明确要求覆盖 AGENTS.md 本阶段仅用 UART0 的默认限制；相应增加 main 对 esp_driver_gpio 的直接依赖。冻结规则/ADR/公共头均保持不动，此项扩展记录于本报告，待人工审查签字。实际补丁由 Codex 生成，无新增生产源文件，既有文件原溯源头保留；未加 Signed-off-by。

电源保持只在 app_main 开始建立，不保证 ROM/bootloader 阶段或复位间隙的供电；用户需在实测中确定按键保持时间。GPIO 调用成功也不证明板上电平和电池保持成功。T15 纯测试镜像不驱动 GPIO26，且其 app 从 0x10000 开始覆盖产品部分 NVS；不能在没有稳定供电、完整备份与恢复方案时直接替换运行。详情见 HIL-PLAN。

自动审批拒绝不能由会话授权直接绕过；设备、备份、供电及实际硬件结果均未核实。不把已有 host/静态证据算作电源保持或 HIL PASS。
