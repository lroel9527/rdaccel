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

API 在 1.0 之前还会变，M1（文字无损层、滚动检测）会在 `Host` / `Viewer` 后面加功能，尽量不改现有接口。

## 当前进度：M0 基线


M0 是一个能跑通的整屏硬编基线，后续每项优化都和它对比。

| 已有 | 说明 |
| --- | --- |
| 分块比对 | 64×64 块哈希，没有块变化的帧整帧跳过 |
| HEVC 编解码 | macOS：VideoToolbox 硬件（优先低延迟模式）；Linux：FFmpeg，NVENC 优先，否则 x265 |
| 屏幕采集 | macOS：ScreenCaptureKit（带脏区）；Linux：X11 MIT-SHM |
| 协议 | 自定义二进制消息，HEVC 码流统一为 4 字节长度前缀格式，两端互通 |
| 传输 | 进程内管道（回环）、TCP（跨机器）；QUIC 在后续里程碑 |
| 示例 | `rdaccel_record` 录制码流文件、`rdaccel_play` 回放并校验、`rdaccel_loopback`（macOS 窗口程序）、`minimal`（最小集成） |

只支持 HEVC，没有 H.264 回退。

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
sudo apt-get install cmake g++ libavcodec-dev libavutil-dev libswscale-dev libx11-dev libxext-dev
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
examples/          只用公开 API 的示例：record、play、loopback_mac、minimal（独立 CMake 工程）
tests/             GoogleTest（test_api.cpp 只用公开 API）
cmake/             find_package 配置模板
```

## 许可

[Apache License 2.0](LICENSE)。Linux 版链接 FFmpeg，可能用到 x265（GPL）；带 x265 的二进制需按 GPL 分发，见 [NOTICE](NOTICE)。
