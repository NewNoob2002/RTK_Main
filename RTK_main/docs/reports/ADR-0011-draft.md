# ADR-0011 修订摘要

状态：**ACCEPTED（待实施）**。按[修订指令](../tasks/ADR-0011-revise.md)落实用户已确认的决定；本轮仅修订以下七份文档，不启动K1，不改代码、既有冻结契约或冻结清单，不commit、不连串口或运行蓝牙负载。

## 交付

- [ADR-0011](../adr/ADR-0011-data-path-memory.md)：§12改为Q1～Q8已决定表，附理由，并同步数据流、TX机制、内存账、接口契约与验收。
- [K1](../tasks/K1-fanout-ring.md)：typed环边界、安全短租约；[K2](../tasks/K2-spp-nonblocking-tx.md)：回复预留、PHONE整条流接纳、匿名WRITE替身用例；[K3](../tasks/K3-rtcm-rx-path.md)：RTCM直通、RX唤醒与新增窗口压测模式要求；[K4](../tasks/K4-gnss-app-ring.md)：4KiB环、超长/查询计数、控制台持久默认与现有App订阅；[K5](../tasks/K5-boot-memory.md)：boot迁移、sys mem、.bss与修订后的内存门槛。
- 本摘要为第七份输出。各文档均为ACCEPTED（待实施）；实施卡仍须单独下达，架构方定稿冻结接口后再实施。

## 已决定与同步结果

| 项 | 决定 / 已同步内容 |
| --- | --- |
| Q1 App环 | Kconfig默认4096B，typed头3B，最多4093B负载；如4096B SATSINFOA，写者整条拒绝并计oversize_drop，不截断。完整长句实际需要时另启8KiB并重验内存；K4验收显式计数。 |
| Q2 查询 | 最新句>256B返回02 02并计query_oversize，不截断、不分片；K4加入确定性测试。 |
| Q3 配置 | 0x0D维持32B COM查询/SET无回复。本期外部统一配置仅串口控制台，持久默认由控制台设置；内部App沿G-C逐消息interval复用校验，不改NVS。0x0D订阅/保存扩展与web不在本期范围。 |
| Q4 反压 | dispatch前预留280B回复空间；满只暂停该会话，已执行回复保留。App按约3KiB内线路字节窗口等待，超时不自动重发副作用命令。 |
| Q5 简化B | 每会话reply2048B，仅PHONE独享可用4096B stream，ALARM无stream。peek两span整条复制，不足不commit、cancel后下轮重试；覆盖计dropped。租约仅覆盖≤4096B锁外复制，不跨等待；TX取得完整句后源环覆盖不影响发送。已移除跨轮分块租约与覆盖后断链方案。 |
| Q6 简化A | 全局及每会话在途≤4<SDK slot队列10；镜像常量与编译断言注释引用btc_spp.c:37。匿名失败计anon_write_fail+trace，关闭当前有在途分块的会话，由CLOSE正常清理；迟到计stray_write。移除全局故障/仅控制台降级/SDK回调终结屏障前置，K2改为host替身注入验证。 |
| Q7 门槛 | 负载最低内部8bit堆≥81920B、最大块≥32768B；下行≥37.5KiB/s（基线1.5倍），探索2倍。门槛已接受，实际达标仍需实施后测量。 |
| Q8 观测 | K6仅未来占位，本期无环/栈/驱动分配，不驱动SPI；命令集合与板级未知事实留K6。 |

## 内存账与压测

原2×1024B stream改为仅PHONE一份4096B，净增2048B；TX.reply两份2048+PHONE stream4096=8192B，另merge1024B，合计9216B。K4 payload净增10332B，静态估算 `108671+10332+Δmeta=119003+Δmeta B`。K5迁移payload总和57856B，静态估算 `119003-57856+Δmeta+ΔK5=61147+Δmeta+ΔK5 B`；未来8KiB App环的boot请求为61952B。本期仍默认4KiB；数组估算不冒充sizeof/map实测，元数据、哨兵、对齐与heap块头另核。

K3已加入 `tools/hil/spp_stress.py` 的upwin实现要求：3072B按线路字节计窗口，256B payload每条280B、最多10条在途，收到完整有效回复才归还窗口。至少10000帧且连续600s窗口内零丢，验证rx_dropped及回复/内容/CRC；假socket覆盖超时不释放/不重发。以下只记录命令，本轮均NOT_RUN：

```sh
# 已有真实命令：all含up无窗口洪泛负例，逐阶段判读，不要求TOTAL lost=0。
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode all
# K3实现新增模式后再执行；当前脚本尚不支持upwin/--window-bytes。
python3 tools/hil/spp_stress.py F8:B3:B7:25:D8:B2 --mode upwin --window-bytes 3072 --flood 10000 --duration 600
```

## 实际检查与限制

| 实际检查 / 命令 | 结果 |
| --- | --- |
| 逐项核对修订指令、七文档与本地SDK/现有压测脚本 | PASS；Q1～Q8决定同步，SDK队列常量10有源码依据，新增upwin明确为未来K3工作 |
| Python文档检查：七文档状态、相对链接/锚点、代码围栏、空白、旧方案残留、决定/验收与内存公式 | PASS；七份状态一致，Q1～Q8齐全；K4净增10332B、boot57856B、K5静态基数61147B算式通过，CLI/计数/门槛交叉核对通过 |
| `git diff --check`、`git diff --name-only`、`git diff --cached --name-only`、`git status --short` | PASS；已跟踪及暂存区无改动，七份输出仍未跟踪，原有用户任务书与压测脚本保留 |
| `sha256sum -c docs/frozen.sha256 --quiet`；用户输入/原有脚本/冻结表SHA256保全检查 | PASS；冻结契约校验通过，brief/revise/spp_stress.py/冻结表与修订前SHA256一致 |
| idf build/size、host/ASan/静态分析/g7b、HIL/吞吐/堆与硬件实测 | NOT_RUN；本轮只修订文档，未实施新方案 |

与修订指令无设计偏离。Q1～Q8没有待确认项；0x0D扩展及K6明确不在本期范围。冻结契约定稿与实现/实测属于后续卡的工作，不能将本轮文档检查记为新方案通过构建或性能达标。完成后停下等待审查，不启动实施。
