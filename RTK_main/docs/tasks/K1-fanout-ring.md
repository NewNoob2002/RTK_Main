# K1：FanoutRing 类型记录与安全零拷贝读取

状态：**ACCEPTED（待实施）**。设计决定已确认；实施与冻结契约定稿另行下达。依据：[ADR-0011](../adr/ADR-0011-data-path-memory.md) §3、§5、§10～12及[修订指令](ADR-0011-revise.md)。

## 1. 目标与前置

保留 legacy FanoutRing 的冻结行为，新增 typed mode：1 B类别、读者订阅掩码、两段span的peek/commit租约，为App输出与未来SD提供同一环原语。组件仍为纯C++20，无IDF/FreeRTOS/产品依赖，不分配堆。

前置：ADR-0011 D8/Q5已接受，架构方定稿本卡冻结接口后实施。App环默认4KiB，超容量整条拒绝并计oversize_drop；通用环仍覆盖2/4/8KiB边界测试，未来改8KiB不改变本卡原语。

## 2. 范围与允许修改文件

| 文件 | 允许改动 |
| --- | --- |
| `components/app_fw/src/fanout_ring.cpp` | 复用既有逻辑，增加typed mode、租约/预留区间、统计与回绕处理 |
| `components/app_fw/src/` 内新增必要私有头 | 仅实现细节，不建立第二套环框架 |
| `test/app_fw/test_fanout_ring.cpp`、`test/app_fw/CMakeLists.txt` | 保留原用例，添加typed/并发/边界用例；如拆文件，仅新增 `test/app_fw/test_fanout_typed.cpp` |
| `components/app_fw/CMakeLists.txt` | 仅新增源/编译选项确有需要时；保持host/IDF双构建、strict warnings |
| `docs/reports/K1.md` | 实施报告 |

不涉及main、蓝牙、RTCM、GNSS、boot_alloc或SD实现。不改 `test/host/**`。

## 3. 冻结文件修订提案

仅在专项批准后：`components/app_fw/include/fw/fanout_ring.hpp`、`docs/adr/ADR-0007-app-services-split.md` §3.6-1；由架构方同步 `docs/frozen.sha256` 相应条目。不得把设计接受视为已授权冻结改动。本轮这些文件保持原字节。

保留旧常量/结构字段/函数和2B记录格式；typed新增3B头与独立init入口。公共类布局可能变化，属于重编译兼容，不承诺二进制ABI兼容。旧源码/旧测试行为兼容。只有typed允许二次幂容量≥max_record+3，legacy仍执行原双最大记录容量校验。新统计与Result使用既有类型，不扩展Result枚举。

## 4. 实施契约

以ADR-0011 §5签名为准。必须先解决以下机制再接入调用者：

1. peek租约生效期间span不被覆盖或搬动；每reader最多一租约；两span只含负载，不含类型/长度头。过期token、重复commit、detach有租约均返回明确定义错误，不改变新读者状态。
2. typed无pin时整记录覆盖；阻挡于pin时写者立即整条拒新kBusy，累计writer及匹配读者pinned_drop，不阻塞。无匹配在线读者不写，计no_subscriber；筛掉记录计filtered，覆盖落后的订阅记录才计dropped。
3. 单写者/每id单读者契约；LockOps保护发布/索引，不持锁调用RTOS或IO。长记录只写入已保留空闲区域，最后发布；临界区每段≤1KiB，读者看不到半记录，writer不能锁外修改正被读取的区域。
4. typed u32回绕不旋转存储，位置差值<2^31；lease序号和generation耗尽策略必须定稿，不允许旧token碰撞后重新有效。mask变化无活动租约才允许，并从当前head开始。
5. legacy与typed接口混用返回kNotSupported；不静默隐藏类型。实现兼容层不得使legacy统计含义改变。
6. BT调用方一次peek后整条转入仅PHONE所有的4096B TX.stream；空间不足cancel而不commit，下轮重新peek，旧记录若被覆盖计dropped。租约只覆盖一次总量≤4096B的锁外有界复制，不跨等待；FanoutLease仅在当次peek/commit/cancel中验证生命周期，不保存为跨轮发送进度。单次复制上限与每段≤1KiB临界区上限分别约束，不能把4096B复制放进portMUX。

## 5. 测试与门禁命令

定向覆盖：legacy全部原用例；type0/31及非法32、mask0/多类型、无读者、attach最新/重attach、两读者不同进度与mask、头/负载各自跨环尾、4093B负载在4KiB成功、4096B负载在4KiB整条拒绝且oversize_drop+1/8KiB成功、最大记录正好容纳、无记录/小输出、pin保护、pin拒新计数、cancel不消费、重复/过期token、detach kBusy、generation/位置回绕、未发布copy并发读者、多个读者并发统计。用受控LockOps/线程调度制造交错，不能靠随机压力替代确定性竞态用例。

```sh
. ~/esp/esp-idf/export.sh >/dev/null
cmake -S test/app_fw -B builds/k1-app_fw
cmake --build builds/k1-app_fw
ctest --test-dir builds/k1-app_fw --output-on-failure
cmake -S test/app_fw -B builds/k1-asan-app_fw \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build builds/k1-asan-app_fw
ctest --test-dir builds/k1-asan-app_fw --output-on-failure
idf.py build
python3 -I tools/g7b_check/g7b_check.py --build build
sha256sum -c docs/frozen.sha256
```

另外执行ADR-0011 §10.2的九套host普通+ASan、改动文件clang-format、现有G5b静态分析/分层/双架构门禁；不能跳过冻结host。权威astyle由验收方执行，不安装下载。IDF主工程仅build/，其他构建仅builds/。

## 6. 验收与报告

全部既有测试通过、新typed故障用例通过，0warning/0sanitizer发现，纯组件无平台依赖、无堆，锁外span生命周期经审查成立。报告新增对象/reader/lease的sizeof及最坏临界区工作量，区分typed拒新和legacy覆盖语义，逐项PASS/FAIL/NOT_RUN/BLOCKED。

交付 `docs/reports/K1.md`，列实际命令、日志、冻结批准/条目、接口定稿偏离和未解问题。若零拷贝安全性或冻结批准未解决，报告K1 BLOCKED，不制作可误用的裸span接口。后续K2～K5不得以K1设计文档代替验收结果。

## 7. 边界

不commit/push、不串口/烧录/硬件、不安装下载；不修改非本卡批准的冻结文件。当前轮只修订文档，以上命令均为未来实施清单；本卡实施另行下达。
