# OL7 报告：单选项关机弹窗 + 5 格电量显示

分支 `feat/ol7-dialog-battery`（基于 main `01af210`），任务卡 `docs/tasks/OL7-dialog-battery-bars.md`。

## 改动文件

- `main/board/PowerLogic.{hpp,cpp}`：新增纯逻辑 `BatteryBars`（含 `BatteryBarsInput/View`、命名常量与依据注释）。
- `main/modules/oled/OledUi.{hpp,cpp}`：`UiController` 持有 `bars_`，替换 `batRoll_` 百分比滚动；关机弹窗单击/双击都确认；电量格节拍并入唤醒周期（`barsWaitMs`）。
- `main/modules/oled/OledRender.cpp`：顶栏/表盘/电源子页卡片/充电待机页全部改为 5 格电池图形，移除百分比数字；关机弹窗只画一个 `POWER OFF` 按钮，保留 5 s 倒计时条。电池详情页保留 mV 与 CHG/FULL/USB 文字。
- `main/modules/oled/OledModule.cpp`、`main/modules/power/PowerModule.cpp`、`main/def/SystemDef.hpp`：`PowerSnapshot` 末尾新增 `batterySimSession`，供 `power simbat` 会话变化时立即重新定格。
- 测试：`tools/main_host_check/PowerLogicCheck.cpp`（BatteryBars 7 组）、`OledUiCheck.cpp`（弹窗/渲染）。
- `tools/oled_sim`：`scripts/tour.txt`、`README.md`，golden 重新生成 17 张，删除 `power-off-confirm-ok.pbm`。
- `docs/adr/ADR-0020-oled-display.md` §5.2/§5.3 更新。

## 与契约的偏离

- `main/def/SystemDef.hpp` 不在任务卡授权清单内，但为把 simbat 会话号传给 OLED 必须在 `PowerSnapshot` 末尾加字段（`oled_sim`/`oled_module_check` 不链接 `PowerModule.cpp`，OLED 无法直接调用 `power::simulatedBatterySession()`）。需 Architect 验收。
- 电量格状态机放在 `UiController` 而非 `PowerModule`，原因同上（host 测试与模拟器可覆盖）；`PowerModule` 仅多发布 `batterySimSession`。

## 验证

| 项 | 结果 |
| --- | --- |
| `tools/ci/run_gates.sh --prefix ol7 --base 8c6ef0e`（43 项：全部 host 套件普通 + ASan/UBSan、G5 format、G6 doc_check、G3 七种 IDF 配置构建、G3b、G5b clang-tidy、G7b、G7c） | PASS（43/43；首轮 doc_check、tidy 失败，已拆出 `stepHysteresis` 并补注释后重跑通过） |
| `sha256sum -c docs/frozen.sha256` | PASS |
| oled_sim headless/comm/standby golden（ctest 在 G2 内） | PASS |
| 烧录 / 串口 / 真机 HIL | NOT_RUN（任务禁止） |

## 资源对比（`idf.py size`，基线 `01af210` vs 本分支）

| 项 | 基线 | 本分支 | 差 |
| --- | --- | --- | --- |
| IRAM | 116139 | 116139 | 0 |
| DRAM | 98611 | 98619 | +8 B（.data +256、.bss −248） |
| Flash Code | 1140848 | 1141200 | +352 |
| 镜像 | 1585114 | 1585706 | +592 |

DRAM 增量远低于 128 B 预算，IRAM 无增长，无运行期堆分配。

## 未解决 / 未验证

- 真机 OLED 观感、充电动画节奏、电池曲线下的实际迟滞效果未经 HIL。
- 迟滞门限（降 3 点/30 s、升 5 点/60 s）为卡片给定值，未用实测放电曲线校准。
