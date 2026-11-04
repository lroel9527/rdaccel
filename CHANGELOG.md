# 更新记录

版本号遵循语义化版本。1.0 之前，次版本号（0.x）变化时 C ABI 可能有不兼容改动；
补丁版本（0.x.y）保持兼容。`find_package(rdaccel 0.4)` 只接受同一个 0.4.x。

## 未发布

- **Wayland 抓屏**：Linux 在 Wayland 会话下通过 xdg-desktop-portal 的 ScreenCast 接口和 PipeWire 采集屏幕
  （GNOME、KDE、sway 等）。用合成器给的脏区；光标优先走 PipeWire 光标元数据（进光标通道），没有时画进画面；
  授权在进程内保留，`RDACCEL_PORTAL_TOKEN_FILE` 可跨进程保留。依赖 `libpipewire-0.3`、`gio-2.0`，
  CMake 选项 `RDACCEL_PIPEWIRE=AUTO|ON|OFF`。CI 在无头 sway + PipeWire + portal 里测试，并用公开 API 录一段 Wayland 屏幕再解码。
- 光标在画面里（Wayland 不支持光标元数据时）不再通过错误回调报“cursor channel unavailable”。

## 0.4.0

第三阶段：显示器选择、文字输入、输入注入，以及弱网（低到 0.4 Mbps）下的延迟。Mac（M 系列，VideoToolbox）复测：
光标延迟 p50 约 23 ms；4 → 1 Mbps 时 99% 的帧送达，p95 约 165 ms；4 → 0.4 Mbps 时实际发送约 350 kbps（低于链路），延迟中位数约 100–140 ms。

- **弱网**（#21–#23）：
  - 带宽估计：链路降速后约 0.4 秒跟上（之前 1 到 4 秒）。排队开始后的前两个 RTT 只看最近一个 RTT 的样本，之后保留排队期间的样本；
    裁剪时至少留 4 个样本。只有“确认比发送慢”或网络明显排队时，样本才能拉低估计；窗口受限的样本不能拉低。
    探测超时只重传最早一个包。路径排队时仍保留每 8 轮一次的 1.25 倍探测。
  - 拥塞窗口：按 2×估计×(最小 RTT + 平滑 RTT)/2 计算，排队期间平滑 RTT 固定为排队开始时的值，避免 RTT 和窗口互相推高。
  - 码率控制：没有排队时目标码率也降到估计的两倍以下（每步最多降 10%）。编码器拿到的码率到了下限以后，
    会话总发送量限制在估计的 90% 以内，超出就跳帧（VideoToolbox 在 1080p 繁忙画面时每帧开销几乎固定，
    300 kbps 下实发约 490 kbps，只有少发帧才降得下来）；网络里有常驻队列时还会隔帧跳过。
  - 区域编码器丢帧后不再强制关键帧，丢帧计入 `encoder_drops`。
  - netsim 新增 `--min-bitrate`，“首帧”从会话开始计时，`--verbose` 增加 area pictures 列。
- **选择显示器**：新增 `rda_list_displays()` / `rdaccel::ListDisplays()` 和 `rda_host_config.display_index`（追加在结构体末尾，旧调用方不受影响）。
  Linux 用 RandR 1.5 的 monitor 列表（没有时整个 X screen 算一块），macOS 用 ScreenCaptureKit 的 SCDisplay；
  0 永远是主显示器。光标坐标相对所选显示器。`rdaccel_serve --list-displays` / `--display=N`。
- **文字输入**：新增输入类型 `RDA_INPUT_TEXT` / `InputKind::kText`（`code` = Unicode 字符）和 C++ `Viewer::SendText(utf8)`。
  握手里新增能力位，对 0.3 的被控端发文字返回 `RDA_ERR_UNSUPPORTED`；被控端遇到不认识的输入类型改为跳过而不是断开。
  C++ `Viewer::SendInput` 现在返回 `bool`。
- **输入注入（可选）**：新增 `rda_injector_*` / `rdaccel::Injector`，把收到的输入事件注入被控系统。Linux 用 XTest
  （CI 在 Xvfb 里验证移动、缩放坐标、点击、滚轮、按键和任意字符输入），macOS 用 CGEvent（需要辅助功能权限）。
  `rdaccel_serve --inject`。滚轮单位明确为“行”，y > 0 向上、x > 0 向右。
- **低带宽下延迟**（Mac 复测 4 → 1 Mbps 时 p95 约 1 秒）：链路降速后带宽估计要 2.7 秒才跟上，码率因此不降。
  带宽估计改为最多看最近 1 秒（网络里排起队时只看最近一个往返）；码率控制看到往返时延明显高于最低值也立刻降码率；
  码率控制按编码器实际产出的视频字节校正超发（连续两秒比要的多，就按比例少要，每步最多 30%，之后每秒放回 20%；
  单个关键帧不算）；修复队列不消退时“每 250 ms 降 5%”从未生效的问题（5% 低于重新配置编码器的 10% 门槛，被丢掉了）。
  编码器自己丢掉一帧（VideoToolbox 低码率时会）不再触发关键帧和全屏重发，这一帧的视频区域并到下一帧里
  （以前每丢一帧就强制关键帧，关键帧更大又更容易被丢，1 Mbps 时大部分帧都这样没了）。新增统计 `rda_host_stats.encoder_drops` / `HostStats::encoder_drops`（追加在结构体末尾）。
- **光标延迟**：光标消息不再排在画面解码后面。Viewer 的 `on_stream_info` / `on_frame` 改在库自己的解码线程调用，
  `on_cursor` 仍在传输的接收线程，**两者可能同时执行**，共用的状态要加锁。云端 1.5 Mbps 模拟里光标 p95 从 57 ms 降到 29 ms。
  另外视频占满链路时，发送端看到往返时延高出最低值就以 0.9 倍估计带宽发送，把网络里的排队清掉，
  带宽样本只有在“确认比发送慢”（真是链路在限速）时才能拉低估计，自己发得慢（排空队列、线程唤醒晚）的样本只能抬高，
  避免估计在负载重的 macOS 上一路掉到 100 kbps 量级。
  光标不再排在一整个往返的视频后面（1.5 Mbps 占满时光标 p50 68 → 27 ms，吞吐不变）。
- **输入法吞字**：macOS 注入文字时若当前输入源是输入法（如拼音），临时切到 ASCII 键盘布局，停打 1 秒后的按键或销毁时切回；
  需要主线程跑 run loop（`rdaccel_serve` 已处理）。文档写明：能出字的键一律发 `kText`，只有不出字的键发按键事件。
- **fuzz**：新增 `fuzz_recording`（录制文件回放，走真实 HEVC 解码器）和 `fuzz/corpus/recording/` 种子语料。
- **修复**：`rda_test_pattern_render()` / `RenderTestPattern` 在宽或高小于约 120 像素时越界写内存；
  文字绕回页首后整帧渲染与增量渲染不一致。
- **VAAPI 编码（实验性）**：Linux 上 `encoder = "hevc_vaapi"` 用 Intel / AMD 显卡编码（BGRA → NV12 → GPU 表面），
  设备用 `RDACCEL_VAAPI_DEVICE` 指定；不会自动选用，打不开时退回 NVENC / x265。CI 没有显卡，只验证了退回路径。
- **发布包**：macOS 改为通用二进制（arm64 + x86_64）；Linux 和 macOS 都多一个静态库包（`...-static.tar.gz`）。
- **CI**：每个 PR 用 abidiff 检查 C ABI（`tools/abi_check.sh`）。
- Linux 新增依赖 libxrandr、libxtst（`libxrandr-dev libxtst-dev`）。
- CI 在 Xvfb 里跑 X11 采集、区域选择和光标测试。

## 0.3.0

第二阶段：安全与健壮性，外加打包。

- **加密**：TCP / UDP 上的数据用应用提供的 32 字节密钥做 XChaCha20-Poly1305 加密认证（Monocypher 4.0.2，内置）。
  网络传输必须给 `encryption_key`，或者显式 `allow_plaintext`；密钥不对时以 `RDA_ERR_AUTH` 断开。（#14）
- **fuzz**：7 个 libFuzzer 目标覆盖所有解析网络或录制数据的地方，CI 每次运行。修复了它找到的两个问题：
  恶意 Hello 的超大分辨率让接收端崩溃（现在拒绝 0 和边长 > 16384），加密层空缓冲上的未定义行为。（#15）
- **`rda_tcp_bind`**：TCP 也能不阻塞地监听，第一个连上来的接收端成为对端。（#16）
- **打包**：安装 pkg-config 文件 `rdaccel.pc`（可重定位）；`cpack` 生成 `rdaccel-<版本>-<系统>-<架构>.tar.gz`；
  `RDACCEL_INSTALL_TOOLS` 一起安装 serve / view 等命令行工具；打 `v*` 标签时 GitHub Actions 生成 Linux 和 macOS 的
  发布包（草稿 release）。共享库安装后，`find_package` 不再要求应用装 FFmpeg / X11 开发包。
- **不兼容**：`rdaccel_serve` / `rdaccel_view` 必须带 `--key=<64 位十六进制>` 或 `--plaintext`。

## 0.2.0

第一阶段：作为库的稳定接口。

- 纯 C 接口 `rdaccel.h`（不透明句柄、`struct_size` 版本化配置、错误码），只含头文件的 C++ 包装 `rdaccel.hpp`；
  共享库只导出 `rda_*`。（#11）
- 连接层：握手和版本协商、心跳和断线检测（`on_state`）、主动关闭时通知对方、认证钩子（`on_auth`）、
  双向应用消息通道（剪贴板文本或自定义消息）；键盘事件统一用 USB HID 键码。（#12）
- 分辨率可以中途改变；`rda_udp_bind` 不阻塞；`rda_udp_from_fd` 接管应用自己的 UDP socket。（#13）

## 0.1.x（M0–M2）

- M0：VideoToolbox / FFmpeg HEVC 编解码、TCP 传输、录制和回放。（#1）
- M1：分层混合编码：静止区域跳过、移动区域复制、缓存、文字和界面无损编码。（#2）
- M2：视频区域单独编码（#3）、带拥塞控制和码率自适应的 UDP 传输（#4）、光标通道（#5）、集成指南和
  serve / view 工具（#6）、渐进式首帧（#7）、光标和抓屏缩放（#8）、静止画面补发（#9）、Mac 复测脚本（#10）。
