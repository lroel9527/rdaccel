# 集成指南

这份文档写给把 rdaccel 嵌进自己程序的人：两端各要做什么、回调在哪个线程、对象怎么销毁、怎么选传输、出错时会发生什么。头文件里的注释是权威说明，这里讲的是它们怎么配合。

## 0. 两种接口，一个 ABI

| 头文件 | 语言 | 说明 |
| --- | --- | --- |
| `<rdaccel/rdaccel.h>` | C99 起 | 库唯一的二进制接口。所有函数以 `rda_` 开头，对象是不透明指针，配置结构体第一个字段是 `struct_size` |
| `<rdaccel/rdaccel.hpp>` | C++20 | 只有头文件的包装层：`rdaccel::Host`、`rdaccel::Viewer`、`rdaccel::Transport` 等，RAII + `std::function` 回调，全部内联调用 `rda_*` |

两者可以混用（`Host::handle()` / `Viewer::handle()` 拿到 C 句柄）。因为跨越边界的只有 C 类型，应用可以用和库不同的编译器、不同的 C++ 标准库，Swift、Rust、Python 等语言也能直接绑定 `rdaccel.h`。动态库只导出 `rda_*` 符号。

C 的写法（完整程序见 `examples/minimal_c/main.c`）：

```c
#include <rdaccel/rdaccel.h>

rda_transport* t = NULL;
rda_host* host = NULL;
rda_host_config cfg;
rda_host_config_default(&cfg);          /* 先填默认值，再改要改的 */
cfg.user = my_state;
cfg.on_input = my_on_input;
if (rda_udp_listen(7300, &t) != RDA_OK ||
    rda_host_create(t, &cfg, &host) != RDA_OK) {   /* t 无论成败都归 host_create 管 */
  fprintf(stderr, "%s\n", rda_last_error());
}
rda_host_start_capture(host);
/* ... */
rda_host_destroy(host);
```

下面的例子用 C++ 包装层写，C 版本一一对应（`Host::SubmitFrame` ↔ `rda_host_submit_frame`，以此类推）。

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
auto transport = rdaccel::ListenUdp(7300, &err);   // #include <rdaccel/rdaccel.hpp>   // 阻塞到对端的第一个包到达
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

完整可运行的例子：`examples/serve.cpp`（发送端）和 `examples/view.cpp`（接收端）；同一进程里的最小例子是 `examples/minimal/main.cpp`，纯 C 版是 `examples/minimal_c/main.c`；带窗口的 macOS 例子是 `examples/loopback_mac/main.mm`。

## 1.1 连接状态、应用消息、输入

**连不上怎么办。** `ConnectUdp` 不阻塞，`Viewer` 创建后在 `peer_timeout_ms` 内等不到 Host 的回应就以 `RDA_ERR_TIMEOUT` 断开，这就是连接超时。`ListenTcp` / `ListenUdp` 会阻塞到对端出现；不想阻塞就用 `BindUdp` / `BindTcp`，马上创建 `Host`，用状态回调等对方连上。

**连接状态。** `Viewer` 创建后先发握手（协议版本、能力位、可选的认证令牌），`Host` 检查后回应，两边进入 `kConnected`。之后双方大约每秒互发心跳，`peer_timeout_ms`（默认 10 秒，0 = 不超时）内收不到对方任何数据就判定断开。主动关闭（析构 `Viewer` / `Host`）会给对方发 Goodbye，对方立刻收到断开而不是等超时。断开是终态，要重连就重建 `Host` / `Viewer` 和传输。

```cpp
host.SetStateHandler([](rdaccel::ConnectionState s, rda_status reason) {
  // kConnected：对方握手成功；kDisconnected：reason 是
  // RDA_ERR_CLOSED（对方关闭或传输断了）、RDA_ERR_TIMEOUT、
  // RDA_ERR_PROTOCOL（版本不兼容）、RDA_ERR_AUTH（认证被拒）
});
// Viewer 侧同样有 ViewerHandlers::on_state，也可以随时查 host.state() / viewer->state()。
```

连接建立之前 `SendMessage` 返回失败（C：`RDA_ERR_BUSY`），`SendInput` 被丢弃，Host 也不接受对方的输入和消息（认证钩子不能被绕过）；所以要发东西请等 `kConnected`。断开后 `SubmitFrame` 和 `SendMessage` 返回失败（C：`RDA_ERR_CLOSED`），`SendInput` 被丢弃，库自己的屏幕采集不再编码。

**认证钩子。** 接收端在 `ViewerOptions::auth_token`（C：`rda_viewer_config.auth_token`，最多 4096 字节）里放一段令牌，发送端在 `HostConfig::auth`（C：`on_auth`）里决定是否接受，拒绝时两边都以 `RDA_ERR_AUTH` 断开。配上加密（见第 4 节）后令牌也是加密传输的；不加密时令牌是明文，只能当作区分用户的标签。

**版本协商。** 握手里双方各报“我说的版本”和“我最低能服务的对方版本”，新版本只要还支持旧版本就能互通；不兼容时以 `RDA_ERR_PROTOCOL` 断开，日志里写明双方版本。解析器会跳过不认识的消息类型、忽略消息末尾新增的字段，所以以后加功能不必升最低版本。

**应用消息。** 双向、可靠、有序，每条最多 16 MiB，走单独的通道，不会卡住画面：

```cpp
viewer->SendMessage(rdaccel::kClipboardTextMessage, std::string_view("要粘贴的文字"));  // 1 = UTF-8 剪贴板文本
host.SendMessage(rdaccel::kFirstUserMessage + 3, payload);   // 0x10000 起给应用自己用
host.SetMessageHandler([](uint32_t type, std::span<const uint8_t> p) { ... });
```

0x10000 以下的类型是库保留的（现在只定义了 1 = 剪贴板文本），发送保留类型会失败。库只负责传，剪贴板怎么读写由应用决定。

**输入事件。** 键盘事件的 `code` 是 USB HID 用法码（键盘页 0x07：0x04 = A，0x28 = 回车，0xE0 = 左 Ctrl……），跟操作系统无关，所以 Mac 控制端和 Linux 被控端对得上。和本机键码互转：`KeyCodeFromMac` / `KeyCodeToMac`（macOS `kVK_*`、`NSEvent.keyCode`）、`KeyCodeFromEvdev` / `KeyCodeToEvdev`（Linux `KEY_*`，X11 键码减 8）；C 里是 `rda_keycode_from_mac()` 等。鼠标按键是 `kButtonLeft = 1`、`kButtonRight = 2`、`kButtonMiddle = 3`、`kButtonBack = 4`、`kButtonForward = 5`；`modifiers` 是 `kModShift | kModCtrl | kModAlt | kModMeta | kModCaps` 的组合（Mac 的 Command 是 `kModMeta`，Option 是 `kModAlt`）。把事件注入系统仍由应用自己做。

**文字输入。** 按键码只描述“按了哪个物理键”，两边键盘布局不同、或者用输入法打中文时就不够了。这时发 `kText`（C：`RDA_INPUT_TEXT`）事件，`code` 是一个 Unicode 字符（码点 1..0x10FFFF，不含代理区），被控端照字面输入，与它自己的布局无关。C++ 里 `viewer->SendText("你好")` 会把 UTF-8 字符串拆成一个字符一个事件；无效的 UTF-8 整串不发。对方是 0.3 及更早的版本、不支持文字输入时（握手里没有这个能力位），发送返回失败（C：`RDA_ERR_UNSUPPORTED`），什么也不发。被控端遇到以后版本新增的输入类型会跳过而不是断开。

## 2. 线程

| 回调 / 调用 | 在哪个线程 | 注意 |
| --- | --- | --- |
| `on_stream_info`、`on_frame`、`on_cursor` | 传输的接收线程，同一个线程依次调用 | `ImageView` 和 `CursorState::image` 只在回调期间有效，要在别的线程用就先拷贝。回调里不要做耗时的事，否则后面的数据会积压 |
| `Host` 的 `SetInputHandler`、`SetMessageHandler`、认证钩子，`Viewer` 的 `on_message` | 传输的接收线程 | 同上 |
| 连接状态回调（`SetStateHandler` / `on_state`） | 传输的接收线程，或库的心跳线程（超时） | 可能和画面回调同时执行。回调里可以调用 `SendMessage`，但不要析构 `Host` / `Viewer` |
| `Host` 的 `SetErrorHandler` 回调（C：`on_error`） | 出错的那个后台线程（采集线程、光标线程） | 不要在回调里析构 `Host` 或调用 `StopScreenCapture` |
| 日志回调（`rda_set_log` / `SetLogHandler`） | 任意库线程，可能并发 | 自己加锁；消息里没有画面、按键或消息内容 |
| `Host::SubmitFrame` | 调用者的线程，同步编码 | 不能并发调用；返回时这一帧已经发给传输 |
| `Host::SetCursor`、`SetCursorImage`、`RequestKeyframe`、`stats` | 任意线程 | 线程安全 |
| `Viewer::SendInput`、`RequestKeyframe`、`stats` | 任意线程 | 线程安全 |
| `SendMessage`、`state()`（两端） | 任意线程 | 线程安全；大消息会分块，多线程同时发也不会交错 |

## 3. 生命周期

- `Host` 和 `Viewer` 拥有传给它们的 `Transport`，析构时先停接收线程，再释放会话。析构返回后不会再有任何回调。C 接口里 `rda_host_create` / `rda_viewer_create` 无论成功失败都接管传入的 `rda_transport*`，调用方不要再释放它；没交出去的传输用 `rda_transport_destroy` 释放。
- 回调里捕获的对象必须活得比 `Host` / `Viewer` 长，或者在它们析构之前不再被用到。最简单的做法是先声明回调用到的状态，再声明 `Host` / `Viewer`，让它们先析构。
- 分辨率可以随时变：送进一帧新尺寸的画面（或采集到的屏幕分辨率变了），Host 就以新尺寸重开编码器、发关键帧，新的尺寸信息跟画面走同一个有序通道，接收端在新旧画面之间准确切换，再收到一次 `on_stream_info`。`HostStats::resolution_changes` 记录次数，帧号连续。

## 4. 传输怎么选

| 传输 | 适合 | 带宽自适应 |
| --- | --- | --- |
| `ListenUdp` / `ConnectUdp` | 真实网络，尤其是带宽不稳定或会丢包的链路 | 有：带宽估计、按估计调码率、队列满时跳帧；光标和输入先于视频发送 |
| `BindUdp` | 同上，发送端不想阻塞：立刻返回，第一个发来数据的接收端成为对端；之前送进的帧只保留最新一张，连上后从关键帧开始 | 同上 |
| `UdpFromFd` | 你自己建好的 UDP socket（NAT 打洞之后、由启动器传进来）；成功后归库所有 | 同上 |
| `ListenTcp` / `ConnectTcp` | 局域网、只能走 TCP 的环境 | 没有：按 `bitrate_bps` 固定码率，网络慢时延迟会越积越大 |
| `BindTcp` | 同上，发送端不想阻塞：立刻返回，第一个连上来的接收端成为对端；之前送进的帧只保留最新一张 | 同上 |
| `CreateLocalPipe` | 同一进程（测试、回环演示） | 不需要 |
| `CreateRecordingTransport` | 录制成 `.rdstream` 文件，之后用 `Viewer::FeedRecording` 回放 | 不需要 |
| `CreateSimulatedNetwork` | 测试：进程内模拟限速、时延、丢包 | 有（跑的就是 UDP 协议） |
| 自己实现 `Transport` | QUIC、WebRTC 数据通道、中继服务器 | 实现 `GetStats` 就有 |

自己实现 `Transport` 的要求（C 接口见 `rdaccel.h` 里的 `rda_transport_vtable`，C++ 里继承 `rdaccel::Transport`）：`Send` 线程安全；同一通道内字节有序，可以拆分或合并；接收回调设置之前到达的数据要先存着；析构时停掉自己的线程并等正在执行的回调返回。通道之间最好互相独立（光标、输入不排在视频后面）。通道号要原样传递：现在有 6 个（`RDA_CHANNEL_COUNT`，第 6 个是应用消息），以后可能增加，256 以下的通道号都应该能传。如果能测出带宽和排队量，实现 `GetStats`，`Host` 会自动做自适应码率。

两端必须用同一种传输。

### 加密

两端在配置里放同一个 32 字节密钥（C：`encryption_key`；C++：`HostConfig::encryption_key` / `ViewerOptions::encryption_key`），之后所有通道上的数据都用 XChaCha20-Poly1305 加密并认证：

- 每次连接双方各出 32 字节随机数，用 BLAKE2b 从应用密钥派生出两个方向各自的会话密钥，所以同一个密钥长期使用也不会重复用 nonce。
- 每条记录带通道号和递增计数，篡改、重放、乱序、跨通道搬运都会被发现；发现后这条连接以 `RDA_ERR_AUTH` 断开，对方也会立刻收到（不用等超时）。
- 一端有密钥、另一端没有时以 `RDA_ERR_PROTOCOL` 断开；密钥不同时两端都是 `RDA_ERR_AUTH`。
- TCP 和 UDP 传输上不给密钥会直接创建失败，除非显式设置 `allow_plaintext`。管道、录制、模拟网络和自定义传输不受此限制（自定义传输可能自带加密，比如 QUIC、WebRTC）。
- 密钥怎么生成、怎么交给两端是应用的事（比如扫码配对、账号服务器下发）。用 `od -An -tx1 -N32 /dev/urandom` 就能生成一个；示例程序用 `--key=<64 位十六进制>`。
- 加密用的是仓库里自带的 [Monocypher](../third_party/monocypher/README.md) 4.0.2（BSD-2 / CC0），已经和 libsodium 逐字节对照过。

## 5. 常用配置

`HostConfig` 的默认值就是推荐值，下面只列出最常需要改的：

| 字段 | 默认 | 什么时候改 |
| --- | --- | --- |
| `fps` | 60 | 链路很慢或想省 CPU 时降到 30 |
| `bitrate_bps` | 8 Mbps | 码率上限。UDP 下实际码率跟着带宽估计走，不会超过它 |
| `min_bitrate_bps` | 300 kbps | 自适应码率的下限 |
| `display_index` | 0（主显示器） | 多显示器时选要采集的那块屏。`ListDisplays()`（C：`rda_list_displays`）列出可选的显示器，0 永远是主显示器 |
| `capture_width` / `capture_height` | 0（原生） | 4K / Retina 屏想降分辨率传输时（macOS 和 Linux 都支持，光标图像跟着缩放） |
| `cursor_channel` | 开 | 一般不关；关掉且 `capture_cursor` 也关时接收端看不到光标 |
| `video_regions` | 开 | 对比测试时关 |
| `progressive` | 开 | 首帧先发视频版、随后补成无损。要求第一张画面就逐像素准确（截图、自动化测试）时关 |
| `encoder` | 自动 | Linux 上强制 `libx265` 或 `hevc_nvenc` |

每个模块的开关和评测数据见 [README](../README.md)。

## 6. 统计、出错和日志

- 能失败的 C 函数都返回 `rda_status`：`RDA_OK` 是 0，错误是负数。常见的有 `RDA_ERR_INVALID_ARG`（参数或配置不对）、`RDA_ERR_UNSUPPORTED`（这台机器没有 HEVC 编解码器）、`RDA_ERR_PERMISSION`（macOS 没给屏幕录制权限）、`RDA_ERR_CODEC`、`RDA_ERR_TRANSPORT`、`RDA_ERR_CLOSED`、`RDA_ERR_IO`。`rda_status_str()` 给出名字，`rda_last_error()` 给出本线程上一次失败的详细原因。C++ 包装层把详细原因写进 `std::string* error`。
- 库自己从不往终端打印（FFmpeg 的输出也接到了库的日志里）。要看日志就用 `rda_set_log(fn, user, RDA_LOG_INFO)` 或 `rdaccel::SetLogHandler(...)` 装一个接收函数。
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

- `rda_version()` 返回 `RDA_MAKE_VERSION(major, minor, patch)`，头文件里的 `RDA_VERSION` 是编译时的版本；`rda_version_string()` 返回文字版。
- 1.0 之前每个次版本可能有不兼容的改动（CMake 包按 `SameMinorVersion` 匹配）。1.0 之后同一主版本内只在结构体末尾加字段、只加函数：库按 `struct_size` 识别旧程序传来的较短结构体，没传的字段用默认值，所以用旧头文件编译的程序可以直接换新库。
- 线上协议有版本号，连接时协商（见 1.1）；当前是 4，旧（版本 3）录制文件仍然能回放。
