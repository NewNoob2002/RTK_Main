# K5：boot_alloc、sys mem 与 s_app 零初始化存储

状态：**ACCEPTED（待实施）**。设计决定已确认；实施与冻结契约定稿另行下达。依据：[ADR-0011](../adr/ADR-0011-data-path-memory.md) §7～12及[修订指令](ADR-0011-revise.md)。

## 1. 目标与前置

通路布局确定后，将大块数组由静态段迁到一次性内部8bit堆；仍静态创建并绑核任务，init结束封存，运行期不分配。任何boot块失败时保留仅UART0控制台。新增sys mem，使静态段、boot表和各caps堆可对账；s_app大存储移到.bss。

前置：K1～K4完成验收并交付实测内存账，本卡冻结契约定稿。Q7门槛已接受；不能在K2布局未定前先迁移，也不能把.data→.bss算作释放运行时RAM。

## 2. 范围与允许修改文件

| 文件 | 范围 |
| --- | --- |
| `components/rtos/include/rtos/boot_alloc.hpp`、`boot_task.hpp`、`boot_stream_buffer.hpp`（新增） | 按AGENTS允许新增RAII包装，声明外供存储/封存契约；不改旧头 |
| `components/rtos/boot_alloc.cpp`（新增）、`components/rtos/CMakeLists.txt` | INTERNAL\|8BIT分配/对齐/失败清理、固定owner表；strict warnings/heap白名单 |
| `main/app_context.hpp`、`main/app_main.cpp` | 两阶段分配/绑定/启动、零初始化存储、console-only降级 |
| `main/data/gnss_output.hpp`、`.cpp`、`main/data/app_data.hpp`、`.cpp`（K4已有文件） | 环/缓存外供、span寿命和小快照 |
| `main/modules/bt_link/bt_link_module.hpp`、`.cpp` | boot缓冲绑定与不可用路径，不重做TX布局 |
| `main/modules/debug/debug_module.hpp`、`.cpp` | Trace/Log存储和trace_out栈外供，trace关闭零分配 |
| `main/modules/gnss/gnss_module.hpp`、`.cpp`、`rtcm_pipe.hpp` | GNSS Demux/pipe外供；UART IDF堆保持单列 |
| `components/spp_server/spp_server_init.c`、`spp_server_session.c`、`spp_server_rx.c`、`spp_server_tx.c`、`private_include/spp_server_priv.h` | 接收组合根纯C storage view，去掉对应内嵌数组；仍无C→C++调用 |
| `main/modules/system/system_cmds.cpp`、`system_module.cpp`、`system_module.hpp`、`main/support/console_support.cpp`、`.hpp` | sys mem与最小控制台命令表，cli统一出口 |
| `main/CMakeLists.txt`、`components/spp_server/CMakeLists.txt`、`main/Kconfig.projbuild` | 新源/功能配置与门禁；不改sdkconfig.defaults |
| `tools/g7b_check/heap_allowlist.txt` | 仅登记boot_alloc.cpp启动期分配源，精确源文件与理由 |
| `tools/main_host_check/**`、`tools/t13_host_check/**`、`test/boot_alloc/**`（必要时新增独立host工程） | 分配器替身、逐owner故障注入、生命周期/控制台/sys mem回归 |
| `docs/reports/K5.md` | map/heap账、失败路径与门禁报告 |

优先复用ExecutorStorageView外供栈能力。不得为了外供存储修改原StaticTask/StaticStreamBuffer/Executor冻结头；新增wrapper不复制一套执行器。旧接口确实无法满足时报告具体阻碍，不能扩大本卡权限。

## 3. 冻结修订提案

新增rtos RAII头属于AGENTS明确例外，验收时由架构方决定加入冻结表；既有rtos头继续冻结。`components/spp_server/include/spp_server.h` 的纯C storage view/init契约需要专项批准（含对齐、长度、初始化时机、回调终结前寿命）。ADR-0009 sys mem/堆指标、CODING-STYLE启动期堆描述若存在冲突，交架构方定稿后仅更新获批SHA项。

不改AGENTS、sdkconfig.defaults、partitions、格式/静态分析配置、test/host或reference。不全表重算SHA掩盖其他变化。

## 4. 实施契约

1. 按ADR §7.1接口定稿实现，固定owner表建议32项；状态UNINIT→OPEN→SEALED或ABORTED。唯一owner静态字符串、bytes/alignment与整数溢出校验，失败out清空、不留半条账。seal后所有allocate返回kInvalidState；已seal块到重启不释放。
2. INTERNAL\|8BIT only，无PSRAM依赖；对齐同时满足StackType_t/队列或stream存储要求。为StreamBuffer哨兵、alignment、heap块头单独记账。SDK UART/BTC内部堆不冒充boot块。
3. 组合根先准备UART0最小console，后一次性申请全部启用块并绑定，seal之后才启动link/io/spp_worker/trace_out。资源init与启动分开，C驱动不能在外供存储就绪前启动回调；外部缓冲覆盖所有消费者生命期。
4. FreeRTOS仍静态任务创建、TCB静态、栈单位B；核/优先级/栈大小沿K4，新增包装注释说明绑核理由。Trace/Log关闭不申请其环/任务栈；GNSS关闭不申请其pipe/demux/io栈等。
5. 任一boot申请/绑定失败，在未启动消费者前abort释放全部，注册仅console的安全命令；业务命令ERR unavailable，不引用无效span。trace尚未可用时直接走cli安全出口。seal后任务/驱动失败则停止子系统并等待回调终结、保留sealed块到重启，不释放可能在用的栈/环。
6. s_app采用constinit零初始化存储、显式init绑定依赖，维持构造不失败/不触硬件。不通过全局动态初始化或裸memset对象清引用实现迁移。map证明大payload从.data进.bss，不能声称该移动减少静态DRAM。
7. sys mem复用cli::Verb/dispatch/out接口，末行OK/ERR；输出.data/.bss、boot状态/owner表/请求字节、caps total/free/min_free/largest及重叠说明，含console-only失败原因。arena封存后只读、控制台不做动态分配；内存计数区分requested与allocator实际耗费。

## 5. 迁移预算与验收基线

基于ADR §8估算，默认4KiB App环boot payload57856B，未来单独启用8KiB则61952B。包括任务栈13824、SPP RX8192、Trace/Log8192、GNSS Demux4096、RtcmPipe4096、TX.reply两份2048+唯一PHONE stream4096+merge1024=9216、App环4096/8192、最新句6144。ALARM不分配stream；相较初稿2×1024B stream，新布局净增2048B。实际K4布局、哨兵、对齐和关闭配置须逐owner校正，不能硬编码估算总数作为测试真值。

默认4KiB配置K4静态119003+Δmeta B，迁移57856B后预计静态61147+Δmeta+ΔK5 B；前后static+boot总量近似不变，新增分配开销另计。s_app约52KB.data迁移仅为任务书历史估计，map定稿数字优先。Q7已决定：负载最低内部8bit堆≥81920B、最大连续内部块≥32768B；余栈≥原验收30%、无持续下降/TWDT。下行门槛≥37.5KiB/s、探索2倍，迁移不得回退已验收性能；这些是已接受的验收条件，达标仍须实测。

## 6. 测试与门禁命令

必须逐owner注入第1～最后一次分配失败、登记表满、重复owner、零长度/非法对齐/整数溢出、二次init、seal后申请、abort前后/非法abort、启动中途失败、回调未结束时禁止释放、trace/GNSS关闭、控制台最小命令/业务拒绝、sys mem账与caps重叠/低内存输出；核对reply.0/reply.1/stream.phone/merge分别2048/2048/4096/1024B、stream仅一份且可用4096B，ALARM无流区。host用替身验证所有权，不宣称host证明真实DRAM段位置。

```sh
. ~/esp/esp-idf/export.sh >/dev/null
cmake -S tools/main_host_check -B builds/k5-main_host_check
cmake --build builds/k5-main_host_check
ctest --test-dir builds/k5-main_host_check --output-on-failure
# 若新增test/boot_alloc工程：
cmake -S test/boot_alloc -B builds/k5-boot_alloc
cmake --build builds/k5-boot_alloc
ctest --test-dir builds/k5-boot_alloc --output-on-failure
idf.py build
idf.py size
idf.py size-components
python3 -I tools/g7b_check/g7b_check.py --build build
sha256sum -c docs/frozen.sha256
```

追加ADR §10.2九套host普通+ASan、新boot_alloc工程普通+ASan、G5b/格式/分层/双架构与strict warnings门禁。若分配器测试并入现有main_host_check，不创建空test工程，报告对应命令NOT_RUN并给实际替代用例位置；不得删减原测试。

审查build/rtk_main.map/ELF符号，记录s_app及每大数组.data/.bss位置、static与owner表勾稽、G7b新增精确分配源。HIL需另授权后才运行：真实sys mem、负载堆/栈、启动失败仅console、双会话/长句/RTCM混流与无PSRAM配置；本轮全部NOT_RUN。

## 7. 交付与边界

`docs/reports/K5.md` 逐项PASS/FAIL/NOT_RUN/BLOCKED，列命令/日志、实际owner/bytes/alignment、allocator开销、static/Flash变化、运行期堆与栈实测、失败路径及冻结批准。只有.data变小而static+boot不明、仍有大数组暗中保留、seal后还能分配或C驱动调用C++，均不通过。

不commit/push、不串口/烧录/硬件、不安装下载；不实现SD、观测环动态记录或web。未得到SPI/观测事实继续标UNKNOWN。本轮只修订本卡文档，完成后停下待审。
