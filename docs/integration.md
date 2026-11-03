# 集成指南

这份文档写给把 rdaccel 嵌进自己程序的人：两端各要做什么、回调在哪个线程、对象怎么销毁、怎么选传输、出错时会发生什么。所有类型都在 `include/rdaccel/` 下，头文件里的注释是权威说明，这里讲的是它们怎么配合。

## 1. 两端各做什么

```text
发送端（被控机）                                   接收端（控制端）
Host ── 采集或 SubmitFrame ── 变化检测 / 编码 ──┐    ┌── Viewer ── on_frame  画面
     ── 光标（系统光标或 SetCursor） ───────────┼────┼──────────── on_cursor 光标
     <─ SetInputHandler  输入事件 ─────────────┘    └── SendInput  输入
                       Transport（UDP / TCP / 管道 / 自定义）
```

发送端：

```cpp
std::string err;
auto transport = rdaccel::ListenUdp(7300, &err);   // 阻塞到对端的第一个包到达
rdaccel::HostConfig cfg;                           // 默认值即可，见第 5 节
rdaccel::Host host(std::move(transport), cfg);
host.SetErrorHandler([](const std::string& e) { /* 记日志；编码器起不来时会到这里 */ });
host.SetInputHandler([](const rdaccel::InputEvent& ev) { /* 注入到系统（库不做） */ });
if (!host.StartScreenCapture(&err)) { /* macOS 上通常是没有屏幕录制权限 */ }
```

自己有画面来源（虚拟显示器、游戏引擎、别的采集方式）时，不调用 `StartScreenCapture`，而是每帧调用 `SubmitFrame`，光标用 `SetCursor` / `SetCursorImage` 报告。

接收端：

```cpp
rdaccel::ViewerHandlers h;
h.on_stream_info = [](int32_t w, int32_t h) { /* 建纹理 */ };
h.on_frame = [](const rdaccel::ImageView& img, std::span<const rdaccel::Rect> changed,
                const rdaccel::FrameInfo& info) { /* 拷贝 changed 区域到纹理 */ };
h.on_cursor = [](const rdaccel::CursorState& c) { /* 更新光标层 */ };
auto viewer = rdaccel::Viewer::Create(rdaccel::ConnectUdp("10.0.0.2", 7300), std::move(h), &err);
viewer->SendInput({rdaccel::InputKind::kMouseMove, 100, 200});
```

完整可运行的例子：`examples/serve.cpp`（发送端）和 `examples/view.cpp`（接收端）；同一进程里的最小例子是 `examples/minimal/main.cpp`；带窗口的 macOS 例子是 `examples/loopback_mac/main.mm`。

## 2. 线程

| 回调 / 调用 | 在哪个线程 | 注意 |
| --- | --- | --- |
| `on_stream_info`、`on_frame`、`on_cursor` | 传输的接收线程，同一个线程依次调用 | `ImageView` 和 `CursorState::image` 只在回调期间有效，要在别的线程用就先拷贝。回调里不要做耗时的事，否则后面的数据会积压 |
| `Host` 的 `SetInputHandler` 回调 | 传输的接收线程 | 同上 |
| `Host` 的 `SetErrorHandler` 回调 | 出错的那个后台线程（采集线程、光标线程） | 不要在回调里析构 `Host` 或调用 `StopScreenCapture` |
| `Host::SubmitFrame` | 调用者的线程，同步编码 | 不能并发调用；返回时这一帧已经发给传输 |
| `Host::SetCursor`、`SetCursorImage`、`RequestKeyframe`、`stats` | 任意线程 | 线程安全 |
| `Viewer::SendInput`、`RequestKeyframe`、`stats` | 任意线程 | 线程安全 |

## 3. 生命周期

- `Host` 和 `Viewer` 拥有传给它们的 `Transport`，析构时先停接收线程，再释放会话。析构返回后不会再有任何回调。
- 回调里捕获的对象必须活得比 `Host` / `Viewer` 长，或者在它们析构之前不再被用到。最简单的做法是先声明回调用到的状态，再声明 `Host` / `Viewer`，让它们先析构。
- 第一帧决定分辨率。之后分辨率变化的帧会被拒绝（`SubmitFrame` 返回 false），要换分辨率就重建 `Host`。

## 4. 传输怎么选

| 传输 | 适合 | 带宽自适应 |
| --- | --- | --- |
| `ListenUdp` / `ConnectUdp` | 真实网络，尤其是带宽不稳定或会丢包的链路 | 有：带宽估计、按估计调码率、队列满时跳帧；光标和输入先于视频发送 |
| `ListenTcp` / `ConnectTcp` | 局域网、只能走 TCP 的环境 | 没有：按 `bitrate_bps` 固定码率，网络慢时延迟会越积越大 |
| `CreateLocalPipe` | 同一进程（测试、回环演示） | 不需要 |
| `CreateRecordingTransport` | 录制成 `.rdstream` 文件，之后用 `Viewer::FeedRecording` 回放 | 不需要 |
| `CreateSimulatedNetwork` | 测试：进程内模拟限速、时延、丢包 | 有（跑的就是 UDP 协议） |
| 自己实现 `Transport` | QUIC、WebRTC 数据通道、中继服务器 | 实现 `GetStats` 就有 |

自己实现 `Transport` 的要求（详见 `transport.h`）：`Send` 线程安全；同一通道内字节有序，可以拆分或合并；接收回调设置之前到达的数据要先存着；析构时停掉自己的线程并等正在执行的回调返回。通道之间最好互相独立（光标、输入不排在视频后面）。如果能测出带宽和排队量，实现 `GetStats`，`Host` 会自动做自适应码率。

两端必须用同一种传输。UDP 和 TCP 目前都没有加密和认证，跨公网使用时请放在 VPN、SSH 隧道或自己的加密传输里面。

## 5. 常用配置

`HostConfig` 的默认值就是推荐值，下面只列出最常需要改的：

| 字段 | 默认 | 什么时候改 |
| --- | --- | --- |
| `fps` | 60 | 链路很慢或想省 CPU 时降到 30 |
| `bitrate_bps` | 8 Mbps | 码率上限。UDP 下实际码率跟着带宽估计走，不会超过它 |
| `min_bitrate_bps` | 300 kbps | 自适应码率的下限 |
| `capture_width` / `capture_height` | 0（原生） | 4K / Retina 屏想降分辨率传输时 |
| `cursor_channel` | 开 | 一般不关；关掉且 `capture_cursor` 也关时接收端看不到光标 |
| `video_regions` | 开 | 对比测试时关 |
| `encoder` | 自动 | Linux 上强制 `libx265` 或 `hevc_nvenc` |

每个模块的开关和评测数据见 [README](../README.md)。

## 6. 统计和出错

- `Host::stats()` 和 `Viewer::stats()` 随时可以调用，返回的都是累计值，每秒取一次求差就是速率（`examples/serve.cpp` 就是这么做的）。
- 编码器创建失败：`SubmitFrame` 返回 false，错误处理回调收到原因，之后的帧都会被拒绝。
- 编码偶尔失败：返回 false，下一帧自动变成全屏刷新。
- 接收端解码失败（或者从中途接入）：Viewer 自动向 Host 请求关键帧（最多每 250 ms 一次），Host 在下一帧发全屏刷新。`ViewerStats::keyframe_requests` 和 `HostStats::keyframe_requests` 记录次数。屏幕完全静止、没有新帧进来时（macOS 的 ScreenCaptureKit 只在画面变化时出帧，自己喂帧的程序也可能停喂），Host 在 100 ms 后用记住的最后一帧补发；因为拥塞跳过的那一帧也是这样补上，`HostStats::frames_resubmitted` 记录次数。
- 光标读不到（比如 Linux 上没有 XFixes）：错误回调收到一条以 `cursor` 开头的消息，画面照常传输。

## 7. 平台要求

| | macOS | Linux |
| --- | --- | --- |
| 编码 / 解码 | VideoToolbox（HEVC 硬件） | FFmpeg：NVENC，否则 libx265；解码 FFmpeg |
| 屏幕采集 | ScreenCaptureKit，需要"屏幕录制"权限，不采集声音 | X11 MIT-SHM（Wayland 下需要 XWayland） |
| 光标 | CoreGraphics + NSCursor | XFixes |
| 依赖 | 系统框架 | `libavcodec libavutil libswscale libx11 libxext libxfixes zlib` |

## 8. 版本

`rdaccel::Version()` 返回库版本。1.0 之前 API 还会调整，但到目前为止每次改动都只增加字段和函数，没有改已有的签名；线上协议有版本号（`Hello`），旧录制文件仍然能回放。
