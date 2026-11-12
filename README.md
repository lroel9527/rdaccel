# rdaccel

面向极低带宽的远程桌面加速库。画面按区域分类传输：没变的区域不发，平移的区域只发拷贝指令，以前出现过的内容从接收端缓存贴回，文字和界面走无损调色板层，只有视频类区域交给硬件 HEVC 编码（macOS VideoToolbox，Linux FFmpeg）。

当前版本 1.9.3。[rdmachine](https://github.com/lroel9527/rdmachine)（对标 NoMachine 的 Mac 远程桌面）就建在 rdaccel 之上：画面、光标、输入和传输由 rdaccel 负责，登录、会话和界面由 rdmachine 负责。

- 静止桌面几十 kbps，打字约 50 kbps，滚动和拖窗 1 Mbps 以内，文字保持无损（[评测](docs/benchmarks.md)）
- 文字模式（`text_mode`）：体验和普通模式一样，流量更少。静止和打字省 3–7 倍（文字减色后用专门的编码），滚动和拖窗省三到六成（[架构](docs/architecture.md#文字模式)）
- 自带 UDP 传输：丢包重传、BBR 式拥塞控制、自适应码率，0.4 Mbps 的链路上也不超发
- 光标走独立通道，链路占满时指针照样跟手
- XChaCha20-Poly1305 加密（密钥由应用提供）、连接状态、认证钩子、输入事件和应用消息通道
- 稳定的 C ABI，加一层只有头文件的 C++ 包装；支持 macOS 12.3+（Apple Silicon 和 Intel）和 Linux（X11、Wayland）

## 用法

rdaccel 是给其他程序集成的库，只有一个 CMake 目标 `rdaccel::rdaccel`。公开接口只有两个头文件：

- `<rdaccel/rdaccel.h>`：稳定的 C ABI，也是库唯一的二进制接口（`rda_*` 函数、不透明句柄、带 `struct_size` 的配置结构体、`rda_status` 错误码、日志回调）。动态库只导出这些符号，C、Swift、Rust、Python 都能直接用。
- `<rdaccel/rdaccel.hpp>`：只有头文件的 C++20 包装层（`rdaccel::Host`、`rdaccel::Viewer`、`rdaccel::Transport`），全部内联调用 C 函数，所以应用可以用和库不同的编译器和标准库。

库内部是 C++20（`core/`、`platform/`、`src/`，命名空间 `rdacore`），不对外。`examples/` 里的程序只是演示和测试工具，它们和第三方应用一样只用公开头文件。

| 公开类型 | 作用 |
| --- | --- |
| `rdaccel::Host` | 发送端：自己采集主屏（`StartScreenCapture`），或接收应用送来的画面（`SubmitFrame`）；做变化检测、HEVC 编码、发送 |
| `rdaccel::Viewer` | 接收端：解码，通过回调把 BGRA 画面和变化区域交给应用自己绘制；`SendInput` 回传输入事件 |
| `rdaccel::Transport` | 传输接口。自带进程内管道、TCP 和带拥塞控制的 UDP，应用可以实现自己的（QUIC、WebRTC、中继服务器等） |
| `CursorImage` / `CursorState` / `DrawCursor` | 光标单独传输，接收端自己画（`ViewerHandlers::on_cursor`） |
| `CreateRecordingTransport` / `ReadRecording` | 录制和回放码流（`.rdstream`），用于对比码率和跨平台互通测试 |
| `RenderTestPattern` | 确定性的合成桌面，没有真实屏幕时用来测试集成 |

最小用法（完整代码见 [examples/minimal/main.cpp](examples/minimal/main.cpp)，纯 C 版见 [examples/minimal_c/main.c](examples/minimal_c/main.c)）：

```cpp
#include <rdaccel/rdaccel.hpp>

auto [host_end, viewer_end] = rdaccel::CreateLocalPipe();  // 跨机器换成 BindUdp / ConnectUdp

rdaccel::ViewerHandlers handlers;
handlers.on_frame = [](const rdaccel::ImageView& bgra, std::span<const rdaccel::Rect> changed,
                       const rdaccel::FrameInfo& info) { /* 画到你的窗口上 */ };
handlers.on_cursor = [](const rdaccel::CursorState& c) { /* 在 (c.x, c.y) 画 c.image */ };
auto viewer = rdaccel::Viewer::Create(std::move(viewer_end), std::move(handlers));

rdaccel::Host host(std::move(host_end));
host.StartScreenCapture();  // 或者每帧调用 host.SubmitFrame(...)
```

集成方式三选一：

```bash
# 1) 安装后 find_package（或 pkg-config）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake --install build --prefix /opt/rdaccel
# CMake：find_package(rdaccel 1.9 REQUIRED) + target_link_libraries(app PRIVATE rdaccel::rdaccel)
# 其他构建系统：PKG_CONFIG_PATH=/opt/rdaccel/lib/pkgconfig pkg-config --cflags --libs rdaccel
#   （静态库加 --static，会带上 FFmpeg/X11 等依赖）

# 2) 作为子目录：add_subdirectory(rdaccel)，此时默认不编译测试和示例

# 3) 用发布包：GitHub Releases 上的 rdaccel-<版本>-<系统>-<架构>.tar.gz，解开即用
#    （共享库 + 头文件 + CMake/pkg-config 文件 + serve/view 等工具）
```

版本变化见 [CHANGELOG](CHANGELOG.md)。同一主版本内接口只加不改（见[发版](docs/releasing.md)）。

跨两台机器试用：

```bash
KEY=$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')         # 32 字节密钥，两边相同（或两边都用 --plaintext）
./build/bin/rdaccel_serve --udp=7300 --key=$KEY           # 被控机：采集主屏，等一个接收端
./build/bin/rdaccel_view 10.0.0.2 --udp=7300 --key=$KEY --seconds=30 --dump=last.ppm   # 控制端：解码并每秒打印统计
./build/bin/rdaccel_remote 10.0.0.2 --udp=7300 --key=$KEY  # 或者 Mac 控制端用窗口直接操作（被控机加 --inject）
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [集成指南](docs/integration.md) | 两端各做什么、线程、生命周期、传输选择、加密、常用配置、出错处理、平台要求 |
| [架构](docs/architecture.md) | 一帧怎么走、分流的四条路、传输和拥塞控制、码率控制、光标、代码布局、配置开关 |
| [设计背景与范围](docs/design.md) | 目标、为什么分层混合编码、库的边界和接口约定、参考文献 |
| [评测](docs/benchmarks.md) | 分区编码、视频区域、模拟网络、渐进首帧、光标通道、Mac 弱网的实测数字和复现命令 |
| [构建与测试](docs/building.md) | macOS / Linux 构建、示例工具、码流互解、sanitizer、fuzz、ABI 检查 |
| [Mac 复测](docs/mac_retest.md) | 真机复测清单和静音脚本 |
| [发版](docs/releasing.md) | 发版步骤和兼容性约定 |

## 构建

```bash
# macOS 12.3+（Apple Silicon 或 Intel）：Xcode 命令行工具 + CMake
# Linux：sudo apt-get install cmake g++ libavcodec-dev libavutil-dev libswscale-dev \
#          libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxtst-dev zlib1g-dev \
#          libpipewire-0.3-dev libglib2.0-dev   # 后两个用于 Wayland，可省
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
ctest --test-dir build
```

详见 [构建与测试](docs/building.md)。

## 目录

```text
include/rdaccel/   公开 API（唯一会被安装的头文件）
src/               公开 API 的实现
core/              内部：分块比对、编解码、会话、码率控制、传输
platform/          内部：mac/（ScreenCaptureKit、VideoToolbox）、linux/（X11、Wayland/PipeWire、FFmpeg）
examples/          只用公开 API 的示例和工具
docs/              文档
tests/ fuzz/       单元测试、libFuzzer 目标
tools/             开发辅助脚本
cmake/             find_package 配置、pkg-config 模板、共享库导出列表
```

## 许可

[Apache License 2.0](LICENSE)。Linux 版链接 FFmpeg，可能用到 x265（GPL）；带 x265 的二进制需按 GPL 分发，见 [NOTICE](NOTICE)。
