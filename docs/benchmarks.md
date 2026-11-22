# 评测

数字都是用 `examples/` 里的工具测的：`rdaccel_bench` 离线跑场景（编码后再解码，与原图逐帧比较），`rdaccel_netsim` 把 Host 和 Viewer 放在同一进程、中间接模拟网络实时跑。构建见 [构建与测试](building.md)。

文字模式和各版本在真实屏幕片段上的流量、文字清晰度对比见 [测试片段](clips.md) 和 [CHANGELOG](../CHANGELOG.md)。

## 弱网（0.4，Apple Silicon Mac，VideoToolbox）

真实 Mac 上用 `rdaccel_netsim` 跑默认的 `video` 场景（30 fps，配置码率 8 Mbps）：

| 网络 | 结果 |
| --- | --- |
| 1.5 Mbps，视频占满链路（`--cursor=1`） | 光标延迟 p50 约 23 ms |
| 4 Mbps，第 6 秒降到 1 Mbps | 99% 的帧送达，画面延迟 p95 约 165 ms |
| 4 Mbps，第 6 秒降到 0.4 Mbps | 实际发送约 350 kbps（低于链路），画面延迟中位数约 100–140 ms |

0.4 Mbps 时 VideoToolbox 每帧有近乎固定的开销（码率给到 300 kbps 时实发约 490 kbps），所以到了码率下限以后靠跳帧把总发送量压到估计带宽的 90% 以内，见 [架构：自适应码率](architecture.md#自适应码率)。

```bash
./build/bin/rdaccel_netsim --seconds=10 --bandwidth=1500000 --cursor=1
./build/bin/rdaccel_netsim --seconds=15 --bandwidth=4000000 --drop-to=1000000 --drop-at=6
./build/bin/rdaccel_netsim --seconds=15 --bandwidth=4000000 --drop-to=400000 --drop-at=6
```

## 分区编码（Apple M6，VideoToolbox HEVC 低延迟硬编）

`rdaccel_bench` 用一张真实桌面截图（1408×880）驱动 5 个场景，各 600 帧（60 fps，10 秒），编码后再解码，与原图逐帧比较。码率为稳态码率（去掉第一帧全屏刷新），PSNR 为逐帧平均，"精确" 为与原图逐像素完全相同的比例。M0 是整屏硬编基线（无损层、平移、精修、缓存全部关掉），M1 是分区编码（默认配置，测于视频区域、光标通道和渐进首帧加入之前）。

| 场景 | M0 码率 | M0 PSNR | M1 码率 | M1 PSNR | M1 精确 |
| --- | --- | --- | --- | --- | --- |
| idle（只有转圈动画） | 338 kbps | 38.8 dB | 17 kbps | 42.3 dB | 67% |
| 打字 | 70 kbps | 39.0 dB | 49 kbps | 43.9 dB | 76% |
| 滚动（滚 2 秒停 1 秒） | 1592 kbps | 39.2 dB | 736 kbps | 46.1 dB | 84% |
| 拖动窗口 | 969 kbps | 39.3 dB | 775 kbps | 41.5 dB | 62% |
| 视频窗口 + 转圈 | 2557 kbps | 38.9 dB | 1240 kbps | 41.6 dB | 61% |

真实屏幕（`rdaccel_loopback`，桌面基本静止、只有应用里的动画）：M0 约 390 kbps、采集到显示 14.7 ms；M1 约 90 kbps、约 10 ms。复现：

```bash
./build/bin/rdaccel_play some.rdstream --dump=dump --dump-every=300   # 从真实录屏导出一帧当背景
./build/bin/rdaccel_bench --background=dump/frame_000300.ppm --frames=600
./build/bin/rdaccel_bench --background=dump/frame_000300.ppm --frames=600 \
    --lossless=0 --moves=0 --refine=0 --cache=0 --min-qp=0              # M0 对照
```
