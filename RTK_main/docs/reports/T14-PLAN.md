# T14 应用集成计划

日期：2026-11-10。依据：`docs/tasks/T14-store-app.md`、其引用的 `reference/cpp-v2-draft/tasks/T14-store-app.md`、ADR-0003、`phone_protocol.md` §6。用户授权规划并调用 omp 编写 T14；本计划限定 T14，T15 尚未执行，不能据此宣称 BATCH-F 全部完成。

## 实现范围与顺序

1. `rtk_store.c`：使用冻结 C 接口；命名空间 rtk、键 sn/alarm_wl；参数/字符串/blob 边界验证；成功打开的 NVS 句柄所有路径关闭，修改成功后 commit；仅按契约在初始化遇到特定 NVS 错误时擦除 NVS 分区并重试。
2. 应用启动：保留版本与复位原因；初始化 NVS，失败记录并使用空白名单与默认 SN；无有效 SN 时按 BT MAC 后两字节生成；名称 E1-Lite-<SN>；装配核心对象、SppTransport 和静态资源；BT 初始化失败不启动链路任务，控制台仍可用。
3. 链路任务：静态创建，core 1、优先级 10、6144 字节栈；唯一持有并调用 LinkManager/AlarmSource/CommandDispatcher/LinkService。poll_event(20 ms)，消费 OPEN/CLOSE/DATA/RX_DROP；每 100 ms tick；白名单变动持久化；循环完成后才喂 TWDT。
4. 控制台：UART0 115200；status、alarm set/clear、pair、wl clear、sn、policy。修改经 StaticQueue 请求；status 只读链路任务发布的快照。快照包含会话 handle/角色/MAC、白名单、配对、报警、解析计数、LinkStats、最近心跳、堆与任务栈水位；portMUX 仅保护固定数据拷贝，不在临界区调用 RTOS/IDF/日志。
5. Kconfig/CMake：默认 CONCURRENT，可切 PHONE_PREEMPT；沿用现有告警与分层规则。无自有堆分配、无异常/RTTI、无动态初始化的函数局部静态。非 constexpr 核心对象使用启动阶段显式构造的静态存储，不修改冻结构造函数。

## 文件与不变量

生产编辑由 Herdr 右侧 omp DeepSeek 独占：`components/rtk_store/{rtk_store.c,CMakeLists.txt}`、`main/{app_main.cpp,app_link.cpp,app_link.hpp,app_console.cpp,app_console.hpp,Kconfig.projbuild,CMakeLists.txt}`。必要的行为验证放非冻结测试位置；不改既有 test/host、已验收 core/proto/BT 实现与公共头。

T13 的冻结例外授权仅属于此前明确列出的修复，不延伸为本轮任意冻结修改权限。T14 若确需更改冻结输入，提供具体证据；先完成其余可实现内容。不新增 GNSS/电台/电池/LED/电源驱动，不进行串口连接/烧录/下载/安装/commit/push。

## 验证

- NVS 缺键、空/超长 SN、损坏/超限 blob、读写/commit 失败与句柄关闭行为；命令非法输入、队列满/链路未启动、快照一致性和启动降级。
- 用实际应用入口或命令处理面作 host smoke；依赖替身只能模拟 IDF/RTOS，不替代待测应用逻辑。
- 普通及 ASan/UBSan host 回归、T13 驱动回归按影响复核、ESP32 构建、格式与冻结校验。
- 集成镜像 ≤1.2 MB，记录组件体积；检查自有目标文件无 malloc/free/new/delete、异常/RTTI；使用本地 cpp_cost.py 或明确说明替代证据。
- HIL-1..6 全部 NOT_RUN。构建、host 与硬件证据分别记录。

实现报告 `docs/reports/T14.md`；主窗口独立审查并写 `docs/reports/QA-T14.md`，发现问题先修复再交付。
