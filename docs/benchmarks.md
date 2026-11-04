# 评测

所有数字都可以用 `examples/` 里的工具复现：`rdaccel_bench` 离线跑场景（编码后再解码，与原图逐帧比较），`rdaccel_netsim` 把 Host 和 Viewer 放在同一进程、中间接模拟网络实时跑。构建见 [构建与测试](building.md)。

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

## 视频区域（Linux x265 评测）

云端 Linux 上用 x265（ultrafast + zerolatency）跑 `rdaccel_bench`，背景是 `tools/make_desktop.py` 生成的 1920×1080 仿真桌面（代码编辑器、带图片的网页、图表、照片墙纸），600 帧 60 fps。`video` 场景是右下角 640×360 的视频窗口，`pause` 场景播放 3 秒、暂停 2 秒循环。其余 4 个场景没有触发视频区域，结果与关闭时逐位相同。

| 场景 | 关闭：码率 | 关闭：区域内/外 PSNR | 开启：码率 | 开启：区域内/外 PSNR | 编码耗时 | 主机 CPU |
| --- | --- | --- | --- | --- | --- | --- |
| video | 4652 kbps | 38.8 / 41.3 dB | 1764 kbps | 36.4 / 40.8 dB | 16.5 → 6.0 ms | 252% → 90% |
| pause | 2795 kbps | 38.0 / 41.4 dB | 1398 kbps | 36.4 / 40.8 dB | 16.9 → 9.7 ms | 165% → 78% |

暂停后的最终画面在视频窗口内：关闭时 36.0 dB（停在运动时的画质），开启时 46.9 dB。区域码率和画质可以换：`--region-min-qp=20 --region-bitrate=8000000` 时 4697 kbps、区域内 39.1 dB / 区域外 41.5 dB，码率与关闭时相近，两边画质都更好，编码耗时仍只有 8.5 ms。区域按 64 像素块对齐，会带进视频窗口周围一圈静止像素，这部分在区域关闭前是区域画质，所以区域外 PSNR 略降。

```bash
python3 tools/make_desktop.py desktop.ppm
./build/bin/rdaccel_bench --background=desktop.ppm --frames=600              # 开启
./build/bin/rdaccel_bench --background=desktop.ppm --frames=600 --regions=0  # 关闭对照
```

## 网络传输（Linux x265，模拟网络）

`rdaccel_netsim` 把 Host 和 Viewer 放在同一进程，中间是模拟网络（限速、单向时延、随机丢包、200 ms 瓶颈缓冲），实时跑 15 秒 `video` 场景（30 fps，配置码率 8 Mbps，背景同上），去掉前 3 秒后统计。"关闭"是固定码率（和现在走 TCP 时一样：编码器不知道网络多快，数据在发送端排队）。

| 网络 | 自适应 | 采集到显示延迟 p50 / p95 | 显示帧比例 | PSNR |
| --- | --- | --- | --- | --- |
| 20 Mbps，5 ms | 开 | 15 / 21 ms | 100% | 41.7 dB |
| 4 Mbps，20 ms，2% 丢包 | 关 | 35 / 219 ms | 100% | 41.6 dB |
| 4 Mbps，20 ms，2% 丢包 | 开 | 35 / 110 ms | 100% | 39.1 dB |
| 1.5 Mbps，20 ms | 关 | 3132 / 5929 ms | 100%（越来越晚） | 41.6 dB |
| 1.5 Mbps，20 ms | 开 | 42 / 65 ms | 98% | 37.0 dB |
| 1.5 Mbps，不做拥塞控制 | 关 | 4807 / 9701 ms | 100%（越来越晚） | 41.6 dB |
| 4 Mbps，第 6 秒降到 1 Mbps | 关 | 54 / 1472 ms | 100% | 41.6 dB |
| 4 Mbps，第 6 秒降到 1 Mbps | 开 | 41 / 113 ms | 96% | 38.4 dB |
| 0.8 Mbps，20 ms | 关 | 稳态阶段一帧都没显示 | 0% | — |
| 0.8 Mbps，20 ms | 开 | 46 / 93 ms | 76% | 33.9 dB |

带宽估计：4 Mbps 链路上估计 4.0 Mbps、RTT 43 ms（基础 40 ms），3% 丢包时吞吐仍接近链路速率。上表测于渐进式首帧之前（相当于 `--progressive=0`）。

```bash
./build/bin/rdaccel_netsim --background=desktop.ppm --seconds=15 --bandwidth=1500000            # 自适应
./build/bin/rdaccel_netsim --background=desktop.ppm --seconds=15 --bandwidth=1500000 --abr=0    # 固定码率对照
./build/bin/rdaccel_netsim --background=desktop.ppm --seconds=15 --drop-to=1000000 --drop-at=6  # 带宽骤降
```

## 渐进式首帧（Linux x265，模拟网络）

不渐进时第一帧是整屏，大部分走无损层（约 520 kB），1.5 Mbps 上要 3.1 秒才出画面。渐进时第一帧只发视频（约 220 kB），文字随后补成无损。`rdaccel_netsim`，60 fps，配置码率 8 Mbps，10 秒，去掉前 6 秒后算稳态；"出画面"是第一帧从采集到显示的时间，后面几列是那一刻屏幕上画面的 PSNR（"—" = 还没有画面）。

| 场景 / 网络 | 渐进 | 出画面 | 1 秒 | 2 秒 | 3 秒 | 5 秒 | 稳态 PSNR |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 静止桌面，1.5 Mbps | 关 | 3111 ms | — | — | — | 41.8 dB | 41.7 dB |
| 静止桌面，1.5 Mbps | 开 | 1389 ms | — | 33.3 dB | 34.3 dB | 41.2 dB | 43.4 dB |
| 静止桌面，3 Mbps，2% 丢包 | 关 | 1667 ms | — | 43.7 dB | 43.7 dB | 43.7 dB | 43.7 dB |
| 静止桌面，3 Mbps，2% 丢包 | 开 | 782 ms | 33.2 dB | 35.4 dB | 41.0 dB | 43.7 dB | 43.7 dB |
| 静止桌面，5 Mbps | 关 | 1082 ms | — | 43.8 dB | 43.8 dB | 43.8 dB | 43.8 dB |
| 静止桌面，5 Mbps | 开 | 584 ms | 33.7 dB | 42.5 dB | 43.4 dB | 43.4 dB | 43.4 dB |
| 静止桌面，20 Mbps | 关 | 512 ms | 44.2 dB | 44.2 dB | 44.2 dB | 44.2 dB | 44.2 dB |
| 静止桌面，20 Mbps | 开 | 384 ms | 42.2 dB | 44.5 dB | 44.5 dB | 44.5 dB | 44.5 dB |
| 播放视频，3 Mbps | 关 | 1677 ms | — | 37.0 dB | 38.7 dB | 39.3 dB | 39.0 dB |
| 播放视频，3 Mbps | 开 | 727 ms | 33.4 dB | 33.3 dB | 35.8 dB | 40.8 dB | 40.5 dB |

出画面快一倍以上。完全清晰要比不渐进时出画面晚一些（1.5 Mbps 上约 4.7 秒对 3.1 秒），因为总数据量多了首帧视频那部分。稳态通常持平或更好，因为最后那张帧内画面不用再和文字分码率；5 Mbps 时低 0.4 dB。调参时踩过的坑：首帧之后照常"细化"视频画面反而更糊（码率降下来以后编码器把静止块编得更差），补发的无损块马上在编码器输入里抹平也会拖累周围的块，所以首帧画面在补完之前保持不动，抹平留到最后那张帧内画面一起做。

```bash
./build/bin/rdaccel_netsim --background=desktop.ppm --scenario=idle --seconds=10 --warmup=6 --bandwidth=1500000 --progressive=1
./build/bin/rdaccel_netsim --background=desktop.ppm --scenario=idle --seconds=10 --warmup=6 --bandwidth=1500000 --progressive=0
```

## 光标通道（Linux x265）

`rdaccel_bench --scenario=pointer`：桌面静止，只有鼠标指针在全屏来回移动（1080p，60 fps，10 秒，指针在左边三分之一时变成文字光标）。PSNR 按用户实际看到的画面算，含指针。

| 指针 | 稳态码率 | 视频帧 | Host CPU | PSNR |
| --- | --- | --- | --- | --- |
| 画进画面（`--cursor=frame`） | 544 kbps | 539 | 179% | 43.3 dB |
| 光标通道（`--cursor=channel`） | 22 kbps（其中光标 10 kbps） | 9 | 13% | 43.3 dB |

`rdaccel_netsim --cursor=1`：`video` 场景，每帧移动一次指针，看指针位置从 Host 到 Viewer 要多久（单向基础时延 20 ms）。

| 网络 | 画面延迟 p50 / p95 | 指针延迟 p50 / p95 |
| --- | --- | --- |
| 20 Mbps | 30 / 37 ms | 20 / 20 ms |
| 4 Mbps，2% 丢包 | 34 / 91 ms | 20 / 54 ms |
| 4 Mbps，第 8 秒降到 1 Mbps | 41 / 86 ms | 20 / 71 ms |
| 1.5 Mbps | 43 / 73 ms | 20 / 41 ms |
| 1.5 Mbps，含开头 3.2 秒的整屏首帧 | 首帧 3171 ms | 42 / 146 ms |

首帧还没传完时指针已经能动。不做拥塞控制时（`--cc=0`）数据都堵在网络缓冲里，优先级没有用，指针延迟 p50 是 198 ms。

```bash
./build/bin/rdaccel_bench --background=desktop.ppm --frames=600 --scenario=pointer --cursor=channel
./build/bin/rdaccel_netsim --background=desktop.ppm --seconds=15 --bandwidth=1500000 --cursor=1
```
