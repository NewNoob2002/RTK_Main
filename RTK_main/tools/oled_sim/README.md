# OLED SDL2 仿真器

仿真器编译实际固件的 `UiController`、渲染器、动画和 WouoUI。发送回调把帧写入 128×64 的 SH1106 页缓冲替身。SDL2 只用于主机窗口，固件构建不包含这个目标。

从 `RTK_main` 运行，需本机 CMake、Ninja 和 `pkg-config sdl2`。

```sh
cmake -S tools/oled_sim -B builds/oled-sim -G Ninja
cmake --build builds/oled-sim
ctest --test-dir builds/oled-sim --output-on-failure
builds/oled-sim/oled_sim --color blue --fps 30 --slow 2
```

窗口默认放大六倍，默认白色。空格松开产生短按。连续两次用固件的 300 ms 窗口判定，按住 2.5 秒进入模拟关机，松开不再产生短按。Esc 退出。`--slow N` 放慢模拟时间，物理长按仍为 2.5 秒。

| 数字键 | 场景 |
| --- | --- |
| 1 | 无卫星 |
| 2 | RTK Fixed |
| 3 | USB 充电 |
| 4 | 低电 |
| 5 | 蓝牙已连 |
| 6 | Wi-Fi |
| 7 | 配对请求 |
| 8 | GNSS 故障 |

无窗口脚本以虚拟单调毫秒运行，输出二进制 PBM 序列及 `frames.csv`。每行写时间戳、动作及可选参数。动作有 `short`、`long`、`scene 1…8`、`check PAGE`、`shot NAME`。脚本中的短按时间戳直接进入固件点击判定。用 `--golden DIR` 时，`shot` 会逐字节比较对应的 PBM。

```sh
builds/oled-sim/oled_sim --headless --script tools/oled_sim/scripts/tour.txt --dump builds/oled-tour
```

含 SDL2 的 `main_host_check` 会运行同一个 golden CTest。
