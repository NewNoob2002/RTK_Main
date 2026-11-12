# 全工程 DeepSeek 复审派工与硬件授权状态

## Herdr 环境诊断补充

用户确认界面正在 Herdr 中。之前“当前进程不在 Herdr”判断过强：实际只证明工具进程缺少会话变量，不否定宿主窗口处于 Herdr。重新检查普通 shell、`login=false` shell、通过审批在沙箱外执行的 shell，HERDR_ENV / HERDR_WORKSPACE_ID / HERDR_TAB_ID / HERDR_PANE_ID 均缺失；herdr/codex 可执行文件均可找到。沙箱外结果相同，所以单纯关闭文件系统沙箱不能证实会解决，具体丢失层级仍 UNKNOWN。

只读解析用户配置 `/home/gtc/.codex/config.toml`，没有显式 shell_environment_policy 的 inherit/exclude/include_only 或 HERDR set；当前目录 `.codex/config.toml` 和 `/etc/codex/config.toml` 不存在。未完整枚举其他配置层，不据此排除启动参数、托管配置、启动服务或环境继承链。官方文档 https://learn.chatgpt.com/docs/config-file/config-advanced 的 Shell environment policy 确认变量传递由 shell_environment_policy 控制，区别于文件/网络沙箱权限。未修改配置、伪造变量或执行 Herdr 控制命令。该诊断 PASS；具体根因 NOT_RUN/待进一步定位，原完整复审和硬件任务状态不变。

日期：2026-11-11。用户要求：在当前 Herdr 窗口分屏启动 Codex，使用 DeepSeek 模型完整复审工程并列出结果；同时明确授权 ESP32 `/dev/ttyACM0` 硬件测试。

## 本轮实际状态

| 检查/动作 | 状态 | 证据 |
|---|---|---|
| 读取 Herdr、ponytail-audit 工作流 | PASS | 已读取本机技能文件 |
| 检查 `HERDR_ENV`、workspace/tab/pane 上下文 | BLOCKED | 环境变量均未设置，`test "${HERDR_ENV:-}" = 1` 不通过；Herdr 技能明确禁止从外部控制聚焦会话 |
| Herdr 分屏、Codex DeepSeek 启动、全工程复审 | NOT_RUN | 尚未调用任何 Herdr 控制命令；没有用其他代理冒充指定的可见分屏 |
| 源码/测试范围清单 | PASS | 已列出 main、5 个自有组件、host/目标测试及门禁工具；未修改源码 |
| 沙箱外审批请求：`python -m esptool --chip esp32 --port /dev/ttyACM0 chip-id`（先激活本地 IDF） | BLOCKED | PreToolUse 的 `rtk_main_guard` 拒绝：AGENTS.md §6 禁止直接访问芯片的工具。命令未执行，未复位/写入芯片，未绕过拒绝 |
| 烧录、备份、串口/HIL | NOT_RUN | 硬件访问审批仍受钩子限制；会话授权已记录，不需重复请求同一范围授权 |

当前缺少完整复审结果，不将先前 T15/TIDY-FIX 的局部复审当作全工程结论。已向用户请求选择从 Herdr 内继续或允许后台 DeepSeek 替代；尚未获得选择。

## 待派给指定 DeepSeek Codex 的完整范围

只读复审，中文报告，不修改产品源码、冻结输入或门禁配置。读取 AGENTS.md、ADR-0002/0003、协议和验收计划，遵循 embedded-engineering 与 ponytail-audit。存在 `.codegraph/`，先用 CodeGraph 定位并对照源码补全省略段。保留全部现有和未跟踪文件，不执行 git 写操作或安装下载。复审代理不接触硬件，硬件测试由主线程独占。

完整覆盖 main、rtk_proto C 核心/C++ 外观、rtk_core 链路/命令/报警逻辑、rtk_os 静态资源封装、rtk_bt C 驱动/适配器、rtk_store NVS、构建/Kconfig/分区与 host/目标测试、G5b/G7b 工具。参考目录和旧固件只作为只读证据，不把构建目录/厂商库当作自有源码审计。预期负载为经典 ESP32 双 SPP 会话、有限队列/RX/帧空间、手机查询与报警器心跳同时运行。

追踪端到端输入和资源所有权：串口/蓝牙外部输入 → 验证/组帧解析 → 策略与命令 → 队列/分块 TX/ACK → NVS；重点检查跨核同步、句柄复用、迟到回调、部分写/拥塞/超时、销毁与失败回滚、掉电持久化、配对授权、缓冲区边界、整数窄化、生命周期与 C/C++ ABI、电源保持启动窗口。审查测试能否捕获真实故障，识别不必要的重复和复杂度。

每条发现必须有严重度、文件行号、具体输入/时序及错误结果、规则/契约依据、最小修复建议和不修的影响；无法给出可达场景的项目列为疑问。优先核实既有测试/源码，不直接采信历史报告。运行可用且必要的本地门禁，分列 PASS/FAIL/NOT_RUN/BLOCKED、日志和未覆盖范围；没有 HEAD 时记录快照摘要，不伪造 diff。最终列出完整结果，区分必须修复、建议修复与已知限制，不自行修复。

## 硬件阶段恢复条件

遵循 `HIL-PLAN.md`：指定 ESP32 `/dev/ttyACM0`，使用已有电源保持候选镜像；先核实芯片/容量/保护及完整备份，再执行产品电源保持与基础控制台测试。纯逻辑测试镜像会覆盖部分产品 NVS，且不驱动 GPIO26，需确认持续供电及恢复路径。钩子正式放行前，不通过其他工具、其他代理或修改规则绕过。
