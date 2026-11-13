# K4：GNSS App 流环、订阅与统一输出配置

状态：**ACCEPTED（待实施）**。设计决定已确认；实施与冻结契约定稿另行下达。依据：[ADR-0011](../adr/ADR-0011-data-path-memory.md) §3.3、§5、§6.2/6.3、§8～12及[修订指令](ADR-0011-revise.md)。并入/替代 [G-C](G-C-phone-subscription.md) 对应实现范围，保留其协议回归。

## 1. 目标与前置

kIo写NMEA/hash App typed环，每个已接纳PHONE一个读者，按掩码/周期输出原始报文；报警器不登记/不收GNSS、无stream区。本期外部统一输出配置入口只接串口控制台，接收机配置与NVS持久默认由控制台设置；内部App订阅沿用G-C逐消息interval并复用校验。0x0D保持32B COM查询/SET无回复，其订阅/保存扩展及web不在本期范围。二进制观测SD仅K6占位。

前置：K1租约安全性、K2非阻塞TX、K3直通/RX均验收，冻结接口定稿。Q1/Q2/Q3/Q5已决定：默认4KiB写者oversize整条丢并计数，查询>256B返回02 02并计query_oversize，PHONE独享4096B流区整条接纳；不再以完整长句或新0x0D协议未定阻塞本卡。

## 2. 范围与允许修改文件

| 文件 | 范围 |
| --- | --- |
| `main/def/gnss_sub_def.hpp`（新增） | 六类映射、类别索引、interval/默认周期、能力位；不将msg_id直接作为mask bit |
| `main/data/gnss_output.hpp`、`.cpp`（新增） | 统一值对象配置入口/共享快照/有界查询邮箱与环存储组合；不建额外调度框架 |
| `main/modules/gnss/gnss_module.hpp`、`.cpp`、`gnss_cmds.cpp`、`gnss_keys.hpp` | 写者端筛选、最新句、降采样、配置命令、状态与NVS键表 |
| `main/modules/bt_link/bt_link_module.hpp`、`.cpp` | PHONE reader attach/detach/epoch，peek整条接纳至唯一PHONE流区，查询/既有逐消息订阅路由接入 |
| `main/app_context.hpp`、`main/data/app_data.hpp`、`.cpp`、`main/def/gnss_def.hpp`、`main/app_main.cpp` | 拥有存储/小状态/追加trace码与装配 |
| `components/link_core/services/gnss_service.cpp`（新增） | 按既有服务函数表方式接入；不新增多态层次 |
| `components/link_core/services/device_service.cpp`、`components/link_core/command_router.cpp` | 仅替代GNSS占位路由、0x02功能字节；其余应答逐字节保持 |
| `components/gnss/src/dialects/unicore*.cpp`、`ublox*.cpp` | 按现有方言机制补输出超集；修改前确认具体文件，不动未列公共接口 |
| `main/modules/storage/store_keys.cpp`、`store_keys.hpp`、`store_catalog.cpp`、`storage_module.cpp` | 仅gnss/out版本化有界blob登记/保存/加载；现有键和命名空间保持 |
| `main/modules/debug/trace_text.cpp`、`main/modules/system/system_cmds.cpp` | 新诊断映射/显示；保留旧两行 |
| `main/Kconfig.projbuild`、`main/CMakeLists.txt`、`components/link_core/CMakeLists.txt` | App环容量/未来SD disabled占位、源接入与strict warnings |
| `test/app_fw/**`、`test/gnss/**`、`test/services/**`、`test/stream_demux/**`、`tools/main_host_check/**` | 映射/协议字节/频率/跨任务/长句/会话回归 |
| `docs/reports/K4.md` | 与G-C范围对照、协议决策/内存/测试报告 |

Kconfig采用 `RTK_GNSS_APP_RING_BYTES`（默认4096；2048仅边界试验、8192供实际长句需求另行启用）；默认4KiB超容量即写者整条丢并计oversize_drop。`RTK_GNSS_STATIC_OBS_ENABLE` 默认n，本期固定不可启用，`RTK_GNSS_STATIC_OBS_RING_BYTES` 估算16384仅未来K6使用。不得出现本期可打开但没有驱动的假功能，不修改sdkconfig.defaults，不加入SD源文件/任务/分配或SPI驱动。

## 3. 冻结变更提案

需架构方批准并更新相应 `docs/frozen.sha256` 条目：

- `components/stream_demux/include/demux/decoders/unicore_hash.hpp`：最大帧1024→4096，含校验/行尾，测试证明缓冲约束；与原G-C专项授权重新核对，不能当成新卡自动继承。
- 新增 `components/link_core/include/core/services/gnss_service.hpp`：纯逻辑服务契约、线程模型与声明，验收后冻结。若必须修改 `command_router.hpp`、`device_service.hpp` 等旧头，先列精确字段/理由再获得额外批准，不预授权。
- `docs/adr/ADR-0010-gnss.md` §2.9、`docs/tasks/G-C-phone-subscription.md`：明确新环/订阅/NVS方案替代NmeaOut+async_tx，禁非原子seqlock；旧任务字节级要求保留。
- `docs/protocol/phone_protocol.md`：offset19 interval与原协议对齐，明确>256B最新句查询返回02 02且不分片；0x0D保持32B COM查询/SET无回复，无订阅/保存线上扩展。

已有rtos、device_proto、gnss公共头和test/host冻结文件不在本卡允许变更内。

## 4. 实施契约

- App环只NMEA/hash，类别映射0～31，写者kIo、读者kLink；现准入最多1个PHONE不变，ALARM/PENDING不挂读者。无在线读者/无人订阅该类就不写；断开cancel租约/detach、归档dropped，重连空mask/最新head。
- App环默认4KiB，4093B payload+3B头为容量边界；4096B SATSINFOA在写者端整条拒绝并计oversize_drop，不截句、不分片，无长句验收阻塞。若实际需要完整长hash，未来另启8KiB并重验内存；通用环host测试可覆盖8KiB成功。GNSS Demux现4096B不重复增配。最新句固定预算6144B：4种NMEA各256、HEADINGA1024、SATSINFOA4096；超过各自上限显式计数。
- 保留六条G-C映射与默认周期、缺能力02 02、正/负/零interval；-128先提升再取绝对值、正值最短50ms。模块输出频率低于请求则按实际模块速率，配置入口不能虚报频率。
- kLink拥有订阅，kIo消费小加锁快照并维护每读者降采样时钟；并集到期写入，读者自行掩码/周期筛选。周期改变/重连按epoch重置，不回放旧报文；不能对非原子payload用seqlock。真实单写/单读及读者侧attach约束与K1一致。
- 每会话reply2048B，PHONE另有唯一可用4096B TX.stream，ALARM无stream。peek两span后整条复制进TX.stream，空间不足不接纳、不commit，立即cancel释放租约，下轮重新peek；若旧记录已被覆盖只计读者dropped。租约只覆盖一次≤4096B的锁外有界复制，不跨等待、不保存跨轮进度；TX持有完整句后可按SDK块提交，reply只在句边界抢占，App环后续覆盖不会截断该句或触发断链。
- interval=0查询需取得最新句线程安全快照。≤256B经有界邮箱封装现协议；>256B确定返回02 02并计query_oversize，不截断、不分片。异步查询在dispatch前准备数据，pending request仍只执行一次，不能让LinkService Router阻塞等kIo。
- 统一配置入口区分session订阅与持久默认：本期串口控制台设置接收机输出和NVS默认，内部App逐消息interval只复用校验并改变会话订阅，不保存默认；都不直接跨任务改模块。gnss/out有版本/长度/范围校验，NVS失败与运行生效分开报告，重连不自动输出。0x0D保持32B COM查询/SET无回复；其订阅/保存扩展与web均不在本期范围。
- 原G-C 0x02功能字节保持 `kRunning ? 0x81 : 0x01`，不影响其他字节；方言补GST/SATSINFOA输出超集，不支持的方言明确拒绝。
- 未来观测表可在文档/类型占位表达可配置集合、16384B/10Hz/22KBps/≥460800估算；没有观测命令和SPI引脚事实，不发送命令、不分配环、不启动任务。

## 5. 测试与门禁命令

覆盖各talker与hash名称、六类映射、-128/-1/0/1/127、模拟时钟降采样及周期切换、无读者零写、mask过滤、PHONE与ALARM角色转换/断开/重连、双策略、ring覆盖/pinned/oversize、两span整条接纳与reply不插句；默认4096B环下4093B成功、4096B整条拒绝且oversize_drop准确增加、无残片；通用8KiB边界成功；TX.stream有效容量4096B、空间不足不commit/cancel后可覆盖、TX持有后源环覆盖不损句且不断链；无缓存/缺能力/查询≤256逐字节及>256返回02 02/query_oversize+1；0x0D原32B查询/SET无回复逐字节回归；控制台持久默认与App逐消息订阅复用校验、App不改NVS、NVS版本坏值/写失败/重启默认；方言脚本输出字节、GNSS关闭/trace关闭配置。

```sh
. ~/esp/esp-idf/export.sh >/dev/null
cmake -S test/gnss -B builds/k4-gnss
cmake --build builds/k4-gnss
ctest --test-dir builds/k4-gnss --output-on-failure
cmake -S test/stream_demux -B builds/k4-stream_demux
cmake --build builds/k4-stream_demux
ctest --test-dir builds/k4-stream_demux --output-on-failure
cmake -S tools/main_host_check -B builds/k4-main_host_check
cmake --build builds/k4-main_host_check
ctest --test-dir builds/k4-main_host_check --output-on-failure
idf.py build
idf.py size
idf.py size-components
python3 -I tools/g7b_check/g7b_check.py --build build
sha256sum -c docs/frozen.sha256
```

再执行ADR-0011 §10.2全九套普通+ASan、G5b/格式/分层/双架构门禁，方言ON/OFF按现有CMake开关各构建（不以禁用方言代替默认全开验收）。主工程用build/，关闭GNSS/trace和两策略经独立配置验证，不能改冻结默认配置；无新依赖下载。

HIL计划（当前NOT_RUN）：App发0x04 interval=1收到1Hz原始GGA，0x05=-5订阅SATSINFOA；实际记录≤4093B才按周期原样输出，4096B记录在默认4KiB环下不输出且oversize_drop增加。interval=0停止并查询，>256B返回02 02且query_oversize增加；断开重连无流，ALARM无stream且GNSS字节0。开流同时win16/窗口上行与RTCM测丢失/回复延迟；0x0D维持旧查询/SET无回复。控制台保存默认后重启验证，不通过App新增保存字段。

## 6. 验收与交付

本卡实际替代G-C本期范围，所有既有字节回归保留；无NmeaOut/async_tx残留、无运行时分配、无跨模块调用。默认4KiB超容量整条丢并计oversize_drop；落后覆盖计dropped；查询超长返回02 02并计query_oversize，0x0D订阅/保存扩展不在本期范围，不标协议未决BLOCKED。整条流接纳后不受源环覆盖影响，ALARM无stream区。实际sizeof进入内存账：K4默认4KiB静态估算119003+Δmeta B，未来8KiB再加4096B；不得为链接通过擅做K5。负载最低内部8bit堆≥81920B、最大块≥32768B；性能门槛按ADR §10。

`docs/reports/K4.md` 列Q1/Q2/Q3/Q5已决定事项的验收结果、与G-C本期逐项对应、控制台NVS格式/失败行为、实际命令/日志/状态、static/heap/栈变化和HIL未测项；未来0x0D扩展明确记不在本期范围。

不commit/push、不串口/烧录/SD硬件、不安装下载。本轮仅修订文档，K6另卡。
