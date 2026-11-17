# 构建与测试

## macOS

rdaccel 只在 macOS 上构建和运行（Linux 支持在 2.0.0 删除了，别的系统上 CMake 配置时直接报错）。需要 macOS 14.2+（Apple Silicon 或 Intel）、Xcode 命令行工具和 CMake（`brew install cmake`）。默认部署目标是 14.2（声音采集用的 Process Tap 从 14.2 起才有），在新系统上构建的产物也能在 14.2 上运行；用到更新系统才有的接口时必须加 `@available` 分支，否则编译报错（`-Werror=unguarded-availability-new`）。

第一次配置时 CMake 从 `downloads.xiph.org` 下载 Opus 1.6.1（声音的编解码，核对 SHA256 后静态编进库里）。离线构建：`-DFETCHCONTENT_SOURCE_DIR_OPUS=<解开的 opus-1.6.1 目录>`；不要声音：`-DRDACCEL_AUDIO=OFF`。

```bash
git clone https://github.com/lroel9527/rdaccel && cd rdaccel
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

ctest --test-dir build -j8           # 全部测试（单元测试、公开 API、平台测试）
# 或者直接跑，这时自己设 RDACCEL_AUDIO_NULL_OUTPUT=1（声音播放器只用空设备）：
RDACCEL_AUDIO_NULL_OUTPUT=1 ./build/bin/rdaccel_tests
RDACCEL_AUDIO_NULL_OUTPUT=1 ./build/bin/rdaccel_platform_tests

# 本机回环：采集主屏 -> 硬编 -> 硬解 -> 窗口显示
open build/bin/rdaccel_loopback.app --args --fps=60 --bitrate=8000000
```

第一次采集屏幕时 macOS 会要求“屏幕录制”权限：系统设置 → 隐私与安全性 → 屏幕与系统录音，给程序（或运行命令的终端）打开后重启程序。输入注入（`rdaccel_serve --inject`）还需要“辅助功能”权限。采集系统声音（`RDA_AUDIO_SOURCE_SYSTEM`，`rdaccel_serve --audio`）要同一页里的“仅系统音频录制”，它和屏幕录制是两项：应用的 Info.plist 必须有 `NSAudioCaptureUsageDescription`，否则系统直接拒绝、不弹询问；询问在第一次开始读声音时出现，建 tap 本身不问；没有权限时读到的全是零。

**测试不出声**：CTest 给每个测试设 `RDACCEL_AUDIO_NULL_OUTPUT=1`，这时播放器不打开任何输出设备；测试信号都是算出来的，没有测试放声音或改音量。读系统声音的测试默认跳过，`RDACCEL_TEST_AUDIO_TAP=1` 才跑，它建的 tap 不让本机静音（`CATapUnmuted`），读到的样本只数个数、看时间戳，不保存。

## 示例和工具

都在 `build/bin/`，只用公开 API：

| 程序 | 作用 |
| --- | --- |
| `rdaccel_serve` / `rdaccel_view` | 跨两台机器：被控端采集主屏（`--list-displays`、`--display=N`、`--inject`），控制端解码并打印统计 |
| `rdaccel_record` / `rdaccel_play` | 录制和回放码流（`.rdstream`），对比码率 |
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

要在窗口里看并直接操作对方：被控机加 `--inject`（需要“辅助功能”权限），控制端换成

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

## 其他检查

- **Sanitizer**：用 ASan + UBSan 构建并跑全部测试：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_OBJCXX_FLAGS="-fsanitize=address,undefined"`（`.mm` 文件用的是 `CMAKE_OBJCXX_FLAGS`）。
- **fuzz**：libFuzzer 目标覆盖所有解析对端数据的代码，用 `-DRDACCEL_BUILD_FUZZERS=ON` 构建（Xcode 自带的 clang 没有 libFuzzer 运行库，要另装 LLVM），见 [fuzz/README.md](../fuzz/README.md)。
- **ABI**：没有自动检查。1.x 的 `tools/abi_check.sh`（libabigail 的 abidiff，只读 Linux 的 ELF）在 3.0.0 删掉了；改 `include/rdaccel/rdaccel.h` 时自己对照“同一主版本内只加不改”的约定（见 [releasing.md](releasing.md)）。
