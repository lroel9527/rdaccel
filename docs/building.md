# 构建与测试

## macOS

需要 macOS 12.3+（Apple Silicon 或 Intel）、Xcode 命令行工具和 CMake（`brew install cmake`）。默认部署目标是 12.3（ScreenCaptureKit 从这一版开始有），在新系统上构建的产物也能在 12.3 上运行；用到更新系统才有的接口时必须加 `@available` 分支，否则编译报错（`-Werror=unguarded-availability-new`）。

```bash
git clone https://github.com/lroel9527/rdaccel && cd rdaccel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

./build/bin/rdaccel_tests            # 单元测试 + 公开 API 测试
./build/bin/rdaccel_platform_tests   # VideoToolbox 编解码、采集等平台测试

# 本机回环：采集主屏 -> 硬编 -> 硬解 -> 窗口显示
open build/bin/rdaccel_loopback.app --args --fps=60 --bitrate=8000000
```

第一次采集屏幕时 macOS 会要求“屏幕录制”权限：系统设置 → 隐私与安全性 → 屏幕与系统录音，给程序（或运行命令的终端）打开后重启程序。输入注入（`rdaccel_serve --inject`）还需要“辅助功能”权限。库不采集也不播放声音。

## Linux

```bash
sudo apt-get install cmake g++ libavcodec-dev libavutil-dev libswscale-dev \
    libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxtst-dev zlib1g-dev \
    libpipewire-0.3-dev libglib2.0-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
```

`libpipewire-0.3-dev libglib2.0-dev` 用于 Wayland 抓屏，找到就自动编进去（`-DRDACCEL_PIPEWIRE=ON` 强制要求，`OFF` 关掉）。

编码器按 NVENC、x265 的顺序自动选择；`encoder = "hevc_vaapi"` 可以用 Intel / AMD 显卡（实验性，设备用 `RDACCEL_VAAPI_DEVICE` 指定）。没有显示器时可以用 Xvfb：

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 ./build/bin/rdaccel_record --source=screen --out=x.rdstream
```

### Wayland

Wayland 桌面（GNOME、KDE、sway、Hyprland 等）只能通过 xdg-desktop-portal 的 ScreenCast 接口抓屏，画面走 PipeWire。
`XDG_SESSION_TYPE=wayland`（或者有 `WAYLAND_DISPLAY`、没有 `DISPLAY`）时自动走这条路；`RDACCEL_CAPTURE=x11` / `=pipewire` 可以强制指定。

- 共享哪块屏由系统弹窗决定，`display_index` 只能是 0；`rda_list_displays()` 在 Wayland 下只返回一项，共享过一次之后才有尺寸。
- 多数桌面第一次会弹窗让用户确认。同一进程里再次开始采集不再弹窗；设置 `RDACCEL_PORTAL_TOKEN_FILE=<文件>` 后，授权跨进程保留（portal 版本 4 起）。
  `RDACCEL_PORTAL_TIMEOUT_S` 是等用户确认的秒数，默认 120。
- 屏幕有变化才有新帧，带合成器给的脏区；静止画面由库自己补发关键帧，和 macOS 一样。
- 光标：portal 支持“光标元数据”（GNOME、KDE）时走光标通道；不支持时（wlroots 系）直接画进画面。
- 输入注入在 Wayland 下还不支持（XTest 只能注入到 XWayland 窗口）。

无头测试环境：`tools/wayland_headless.sh 命令…` 起一个 sway（软件渲染）+ PipeWire + xdg-desktop-portal-wlr 的桌面再运行命令，
需要 `sway pipewire wireplumber xdg-desktop-portal xdg-desktop-portal-wlr dbus-daemon`。Ubuntu 24.04 的 xdg-desktop-portal-wlr
在没有显卡时不肯共享屏幕，`tools/build_test_xdpw.sh` 编一个去掉这条限制的版本，用 `RDA_WL_XDPW=<路径>` 指定。

## 示例和工具

都在 `build/bin/`，只用公开 API：

| 程序 | 作用 |
| --- | --- |
| `rdaccel_serve` / `rdaccel_view` | 跨两台机器：被控端采集主屏（`--list-displays`、`--display=N`、`--inject`），控制端解码并打印统计 |
| `rdaccel_record` / `rdaccel_play` | 录制和回放码流（`.rdstream`），对比码率、跨平台互解 |
| `rdaccel_bench` | 离线场景评测（idle、打字、滚动、拖窗、视频、暂停、指针） |
| `rdaccel_netsim` | Host 和 Viewer 同进程，中间是模拟网络（限速、时延、丢包、带宽骤降） |
| `rdaccel_loopback` | macOS 窗口程序，本机回环 |
| `rdaccel_remote` | macOS 窗口程序，跨机器远程控制：显示对方屏幕，把本窗口的鼠标、滚轮、键盘发过去（对方 `rdaccel_serve --inject`） |
| `examples/minimal`、`examples/minimal_c` | 独立 CMake 工程，演示 `find_package` 集成 |
| `tools/make_desktop.py` | 生成评测用的 1920×1080 仿真桌面 |

跨两台机器试用：

```bash
KEY=$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')    # 32 字节密钥，两边相同（或两边都用 --plaintext）
./build/bin/rdaccel_serve --udp=7300 --key=$KEY      # 被控机
./build/bin/rdaccel_view 10.0.0.2 --udp=7300 --key=$KEY --seconds=30 --dump=last.ppm   # 控制端
```

要在窗口里看并直接操作对方（控制端是 Mac）：被控机加 `--inject`（macOS 被控机需要“辅助功能”权限），控制端换成

```bash
./build/bin/rdaccel_remote 10.0.0.2 --udp=7300 --key=$KEY
```

键盘按物理键发送，打什么字由被控机自己的键盘布局和输入法决定，Command 快捷键也发给对方；关窗口退出。
窗口里的指针是本机的系统指针（形状用对方的），鼠标一动立刻跟手，不等网络往返。
标题栏每秒显示显示/解码帧率、码率、解码时间和两个延迟（只用本机时钟）：`pointer` 是发出指针移动到对方光标到达那个位置的时间
（网络往返加对方的光标轮询）；`input→screen` 是在静止画面上按键、点击或滚动，到第一张变化的画面的时间
（往返、采集、编码、解码）。画面本来就在变时的输入不计入。

录一段码流看码率：

```bash
./build/bin/rdaccel_record --source=screen --frames=600 --out=screen.rdstream   # 10 秒真实屏幕
./build/bin/rdaccel_record --out=mac.rdstream --frames=300 --size=1920x1080     # 合成画面，各机器结果一致
./build/bin/rdaccel_play mac.rdstream --check-synthetic
```

## Mac 与 Linux 码流互解

改了编解码相关代码后，确认 Linux 上 x265 录的码流 macOS 上 VideoToolbox 能解，反过来也一样：在一端用 `rdaccel_record` 生成 `.rdstream`，拷到另一端运行 `rdaccel_play FILE --check-synthetic`，结果为 `OK` 且 PSNR 高于 30 dB 即互通。

## 其他检查

- **Sanitizer**：在 Linux 上用 ASan + UBSan 构建并跑全部测试：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" -DCMAKE_C_FLAGS="-fsanitize=address,undefined"`。
- **fuzz**：libFuzzer 目标覆盖所有解析对端数据的代码，用 `-DRDACCEL_BUILD_FUZZERS=ON` 构建，见 [fuzz/README.md](../fuzz/README.md)。
- **ABI**：`tools/abi_check.sh` 用 abidiff 对比上一版本的 C ABI，改了公开头文件就跑一次。
- **真机复测**：硬件编解码相关的项目要在真实 Mac 上确认，清单和静音脚本见 [mac_retest.md](mac_retest.md)。
