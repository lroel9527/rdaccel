# rdaccel

面向极低带宽的远程桌面加速库：硬件 HEVC 编码 + 按区域分类传输（静态区域不发、平移区域只发拷贝指令、文字区域走无损层、视频区域走硬件编码）。

- 调研综述：<https://claude.ai/artifact/D9zzfDNtE4Juc1ZjWCnVRz>
- 架构设计：<https://claude.ai/artifact/6VVJSnadDV9ySaP3p7uJ1i>

## 这是一个库

rdaccel 是给其他程序集成的 C++20 库，只有一个 CMake 目标 `rdaccel::rdaccel`，公开 API 全部在 `include/rdaccel/` 下，入口是 `#include <rdaccel/rdaccel.h>`。`examples/` 里的程序只是演示和测试工具，它们和第三方应用一样只用公开 API。

| 公开类型 | 作用 |
| --- | --- |
| `rdaccel::Host` | 发送端：自己采集主屏（`StartScreenCapture`），或接收应用送来的画面（`SubmitFrame`）；做变化检测、HEVC 编码、发送 |
| `rdaccel::Viewer` | 接收端：解码，通过回调把 BGRA 画面和变化区域交给应用自己绘制；`SendInput` 回传输入事件 |
| `rdaccel::Transport` | 传输接口。自带进程内管道和 TCP，应用可以实现自己的（QUIC、WebRTC、中继服务器等） |
| `CreateRecordingTransport` / `ReadRecording` | 录制和回放码流（`.rdstream`），用于对比码率和跨平台互通测试 |
| `RenderTestPattern` | 确定性的合成桌面，没有真实屏幕时用来测试集成 |

最小用法（完整代码见 [examples/minimal/main.cpp](examples/minimal/main.cpp)）：

```cpp
#include <rdaccel/rdaccel.h>

auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();  // 跨机器换成 ListenTcp / ConnectTcp

rdaccel::ViewerHandlers handlers;
handlers.on_frame = [](const rdaccel::ImageView& bgra, std::span<const rdaccel::Rect> changed,
                       const rdaccel::FrameInfo& info) { /* 画到你的窗口上 */ };
auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(handlers));

rdaccel::Host host(std::move(host_end));
host.StartScreenCapture();  // 或者每帧调用 host.SubmitFrame(...)
```

集成方式二选一：

```bash
# 1) 安装后 find_package
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix /opt/rdaccel
# 你的 CMakeLists.txt：find_package(rdaccel 0.1 REQUIRED) + target_link_libraries(app PRIVATE rdaccel::rdaccel)

# 2) 作为子目录：add_subdirectory(rdaccel)，此时默认不编译测试和示例
```

API 在 1.0 之前还会变。M1 的功能都加在 `Host` / `Viewer` 后面，现有接口没有改，`HostConfig` / `HostStats` 只增加了字段。

## 当前进度：M1 按区域分类传输

M0 是整屏硬编基线；M1 在它上面加了按区域分类，每一项都可以在 `HostConfig` 里单独关掉做对比。

| 模块 | 说明 | 开关 |
| --- | --- | --- |
| 分块比对 | 64×64 块哈希，没有块变化的帧整帧跳过 | — |
| 调色板无损层 | 颜色少的块（文字、界面）编成调色板 + 索引，所有块共用一条跨帧 deflate 流；送进硬件编码器的画面里这些块被抹成底色，打字时往往一帧视频都不用编 | `lossless`、`lossless_max_colors`（静止块，默认 256）、`lossless_max_colors_hot`（每帧都在变的块，默认 4） |
| 滚动 / 拖窗检测 | 变化块多时，用每块最有细节的一行做滚动哈希，在上一帧里找位置投票出位移，逐块精确比对后发拷贝指令；滚动区和窗口边缘的块按 4 像素宽的条和像素行再拆，只把新露出的像素送去编码 | `detect_moves` |
| 块缓存 | 接收端 1024 个槽。即将被盖住的、停留过一阵的块，以及反复出现的块（转圈动画）先存起来，内容再出现时直接从槽里贴回；拖动窗口露出的背景即使只占块的一部分，也按条从槽里局部贴回。缓存、拷贝指令和无损块走同一条 deflate 流，几乎不占码率 | `tile_cache` |
| 静止区精修 | 视频块变化后，接收端在之后 N 个视频帧里继续采用编码器对它的精修 | `refine_frames`（默认 8） |
| 视频区域（M2） | 相邻多块在大部分帧里都要走视频编码（播放视频、游戏）时，把这块区域识别出来，单独开一路 HEVC 码流，只编这块区域的裁剪画面，有自己的码率和 QP 下限；全屏编码器在这里不再花码率。视频暂停约 0.5 秒后区域关闭，区域内的块再走一次普通路径，暂停画面变清晰 | `video_regions`、`video_region_min_qp`（默认 27）、`video_region_bitrate_bps`（0 = 按面积分总码率，至少 1/4） |
| 码控 | 给视频编码器设 QP 下限，避免低延迟码控在静止内容上无限精修 | `min_qp`（默认 24，0 = 编码器自定） |
| HEVC 编解码 | macOS：VideoToolbox 硬件（优先低延迟模式）；Linux：FFmpeg，NVENC 优先，否则 x265 | `encoder` |
| 屏幕采集 | macOS：ScreenCaptureKit（带脏区，不采集声音）；Linux：X11 MIT-SHM | — |
| 传输 | 进程内管道（回环）、TCP（跨机器）；QUIC 在后续里程碑 | — |
| 示例 | `rdaccel_record`、`rdaccel_play`、`rdaccel_bench`（场景评测）、`rdaccel_loopback`（macOS 窗口程序）、`minimal` | — |

只支持 HEVC，没有 H.264 回退。

### 评测结果（Apple M6，VideoToolbox HEVC 低延迟硬编）

`rdaccel_bench` 用一张真实桌面截图（1408×880）驱动 5 个场景，各 600 帧（60 fps，10 秒），编码后再解码，与原图逐帧比较。码率为稳态码率（去掉第一帧全屏刷新），PSNR 为逐帧平均，"精确" 为与原图逐像素完全相同的比例。

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

### 视频区域（M2，Linux x265 评测）

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

## 在 Mac 上编译运行

需要 macOS 14+、Apple Silicon、Xcode 命令行工具和 CMake（`brew install cmake`）。

```bash
git clone https://github.com/lroel9527/rdaccel && cd rdaccel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 单元测试 + VideoToolbox 编解码测试 + 公开 API 测试
./build/bin/rdaccel_tests
./build/bin/rdaccel_platform_tests

# 本机回环：采集主屏 -> 硬编 -> 硬解 -> 窗口显示
open build/bin/rdaccel_loopback.app --args --fps=60 --bitrate=8000000
```

第一次运行时 macOS 会要求“屏幕录制”权限：系统设置 → 隐私与安全性 → 屏幕与系统录音，给 rdaccel_loopback（或运行命令的终端）打开后重启程序。窗口标题和终端每秒打印一次：编码帧率、跳过帧数、码率、编解码耗时、采集到显示的延迟。

### 测基线

```bash
# 录 10 秒真实屏幕（60 fps），看码率
./build/bin/rdaccel_record --source=screen --frames=600 --out=screen.rdstream

# 合成画面（打字 + 小视频窗口），每台机器结果一致，适合对比
./build/bin/rdaccel_record --out=mac.rdstream --frames=300 --size=1920x1080
./build/bin/rdaccel_play mac.rdstream --check-synthetic
```

### Mac 与 Linux 码流互解

CI 每次会在 Linux 上用 x265 录一段合成码流、在 macOS 上用 VideoToolbox 解，再反过来。本地也可以手动做：在一端 `rdaccel_record` 生成 `.rdstream`，拷到另一端 `rdaccel_play FILE --check-synthetic`，结果为 `OK` 且 PSNR 高于 30 dB 即互通。

## 在 Linux 上编译

```bash
sudo apt-get install cmake g++ libavcodec-dev libavutil-dev libswscale-dev libx11-dev libxext-dev zlib1g-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
```

没有显示器时可以用 Xvfb：`Xvfb :99 -screen 0 1280x720x24 & DISPLAY=:99 ./build/bin/rdaccel_record --source=screen --out=x.rdstream`。

## 目录

```text
include/rdaccel/   公开 API（唯一会被安装的头文件）
src/               公开 API 的实现
core/              内部：协议、分块比对、HEVC 码流格式、会话、传输、合成画面
platform/          内部：mac/（ScreenCaptureKit、VideoToolbox）、linux/（X11、FFmpeg）
examples/          只用公开 API 的示例：record、play、bench、loopback_mac、minimal（独立 CMake 工程）
tools/             开发辅助脚本（make_desktop.py：生成评测用的仿真桌面）
tests/             GoogleTest（test_api.cpp 只用公开 API）
cmake/             find_package 配置模板
```

## 许可

[Apache License 2.0](LICENSE)。Linux 版链接 FFmpeg，可能用到 x265（GPL）；带 x265 的二进制需按 GPL 分发，见 [NOTICE](NOTICE)。
