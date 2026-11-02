# rdaccel

面向极低带宽的远程桌面加速库：硬件 HEVC 编码 + 按区域分类传输（静态区域不发、平移区域只发拷贝指令、文字区域走无损层、视频区域走硬件编码）。

- 调研综述：<https://claude.ai/artifact/D9zzfDNtE4Juc1ZjWCnVRz>
- 架构设计：<https://claude.ai/artifact/6VVJSnadDV9ySaP3p7uJ1i>

## 当前进度：M0 基线

M0 是一个能跑通的整屏硬编基线，后续每项优化都和它对比。

| 已有 | 说明 |
| --- | --- |
| 分块比对 | 64×64 块哈希，没有块变化的帧整帧跳过 |
| HEVC 编解码 | macOS：VideoToolbox 硬件（优先低延迟模式）；Linux：FFmpeg，NVENC 优先，否则 x265 |
| 屏幕采集 | macOS：ScreenCaptureKit（带脏区）；Linux：X11 MIT-SHM |
| 协议 | 自定义二进制消息，HEVC 码流统一为 4 字节长度前缀格式，两端互通 |
| 传输 | 进程内管道（回环）、TCP（跨机器）；QUIC 在后续里程碑 |
| 工具 | `rdaccel_record` 录制码流文件、`rdaccel_play` 回放并校验、`rdaccel_loopback`（macOS 窗口程序） |

只支持 HEVC，没有 H.264 回退。

## 在 Mac 上编译运行

需要 macOS 14+、Apple Silicon、Xcode 命令行工具和 CMake（`brew install cmake`）。

```bash
git clone https://github.com/lroel9527/rdaccel && cd rdaccel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 单元测试 + VideoToolbox 编解码测试
./build/rdaccel_tests
./build/rdaccel_platform_tests

# 本机回环：采集主屏 -> 硬编 -> 硬解 -> 窗口显示
open build/rdaccel_loopback.app --args --fps=60 --bitrate=8000000
```

第一次运行时 macOS 会要求“屏幕录制”权限：系统设置 → 隐私与安全性 → 屏幕与系统录音，给 rdaccel_loopback（或运行命令的终端）打开后重启程序。窗口标题和终端每秒打印一次：编码帧率、跳过帧数、码率、编解码耗时、采集到显示的延迟。

### 测基线

```bash
# 录 10 秒真实屏幕（60 fps），看码率
./build/rdaccel_record --source=screen --frames=600 --out=screen.rdstream

# 合成画面（打字 + 小视频窗口），每台机器结果一致，适合对比
./build/rdaccel_record --out=mac.rdstream --frames=300 --size=1920x1080
./build/rdaccel_play mac.rdstream --check-synthetic
```

### Mac 与 Linux 码流互解

CI 每次会在 Linux 上用 x265 录一段合成码流、在 macOS 上用 VideoToolbox 解，再反过来。本地也可以手动做：在一端 `rdaccel_record` 生成 `.rdstream`，拷到另一端 `rdaccel_play FILE --check-synthetic`，结果为 `OK` 且 PSNR 高于 30 dB 即互通。

## 在 Linux 上编译

```bash
sudo apt-get install cmake g++ libavcodec-dev libavutil-dev libswscale-dev libx11-dev libxext-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
```

没有显示器时可以用 Xvfb：`Xvfb :99 -screen 0 1280x720x24 & DISPLAY=:99 ./build/rdaccel_record --source=screen --out=x.rdstream`。

## 目录

```text
include/rdaccel/   对外头文件
core/              平台无关：协议、分块比对、HEVC 码流格式、会话、传输、合成画面
platform/          platform.h 工厂接口；mac/（ScreenCaptureKit、VideoToolbox）；linux/（X11、FFmpeg）
apps/loopback/     macOS 回环窗口程序
tools/             record / play
tests/             GoogleTest
```

## 许可

尚未选定开源许可证。Linux 版链接 x265（GPL），带 x265 的二进制需按 GPL 分发。
