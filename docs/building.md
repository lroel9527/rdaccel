# 构建与测试

## macOS

需要 macOS 14+、Apple Silicon、Xcode 命令行工具和 CMake（`brew install cmake`）。

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
    libx11-dev libxext-dev libxfixes-dev libxrandr-dev libxtst-dev zlib1g-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
```

编码器按 NVENC、x265 的顺序自动选择；`encoder = "hevc_vaapi"` 可以用 Intel / AMD 显卡（实验性，设备用 `RDACCEL_VAAPI_DEVICE` 指定）。没有显示器时可以用 Xvfb：

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 ./build/bin/rdaccel_record --source=screen --out=x.rdstream
```

## 示例和工具

都在 `build/bin/`，只用公开 API：

| 程序 | 作用 |
| --- | --- |
| `rdaccel_serve` / `rdaccel_view` | 跨两台机器：被控端采集主屏（`--list-displays`、`--display=N`、`--inject`），控制端解码并打印统计 |
| `rdaccel_record` / `rdaccel_play` | 录制和回放码流（`.rdstream`），对比码率、跨平台互解 |
| `rdaccel_bench` | 离线场景评测（idle、打字、滚动、拖窗、视频、暂停、指针） |
| `rdaccel_netsim` | Host 和 Viewer 同进程，中间是模拟网络（限速、时延、丢包、带宽骤降） |
| `rdaccel_loopback` | macOS 窗口程序，本机回环 |
| `examples/minimal`、`examples/minimal_c` | 独立 CMake 工程，演示 `find_package` 集成 |
| `tools/make_desktop.py` | 生成评测用的 1920×1080 仿真桌面 |

跨两台机器试用：

```bash
KEY=$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')    # 32 字节密钥，两边相同（或两边都用 --plaintext）
./build/bin/rdaccel_serve --udp=7300 --key=$KEY      # 被控机
./build/bin/rdaccel_view 10.0.0.2 --udp=7300 --key=$KEY --seconds=30 --dump=last.ppm   # 控制端
```

录一段码流看码率：

```bash
./build/bin/rdaccel_record --source=screen --frames=600 --out=screen.rdstream   # 10 秒真实屏幕
./build/bin/rdaccel_record --out=mac.rdstream --frames=300 --size=1920x1080     # 合成画面，各机器结果一致
./build/bin/rdaccel_play mac.rdstream --check-synthetic
```

## Mac 与 Linux 码流互解

CI 每次在 Linux 上用 x265 录一段合成码流、在 macOS 上用 VideoToolbox 解，再反过来。手动做法：在一端用 `rdaccel_record` 生成 `.rdstream`，拷到另一端运行 `rdaccel_play FILE --check-synthetic`，结果为 `OK` 且 PSNR 高于 30 dB 即互通。

## 其他检查

- **Sanitizer**：CI 的 Linux 任务用 ASan + UBSan 构建并跑全部测试：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" -DCMAKE_C_FLAGS="-fsanitize=address,undefined"`。
- **fuzz**：libFuzzer 目标覆盖所有解析对端数据的代码，用 `-DRDACCEL_BUILD_FUZZERS=ON` 构建，见 [fuzz/README.md](../fuzz/README.md)。
- **ABI**：`tools/abi_check.sh` 用 abidiff 对比上一版本的 C ABI，CI 每个 PR 都跑。
- **真机复测**：云端 CI 的 macOS 跑的是 VideoToolbox 软件编码，硬件相关的项目要在真实 Mac 上确认，清单和静音脚本见 [mac_retest.md](mac_retest.md)。
