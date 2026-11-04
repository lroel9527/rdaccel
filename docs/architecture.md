# 架构

rdaccel 是一个“分层混合编码”的远程桌面库：先判断哪些块根本不用发、哪些只是挪了位置或以前出现过，剩下的文字和界面块走 CPU 上的无损调色板层，只有像视频的区域才交给硬件 HEVC 编码器。为什么这样分层，见 [设计背景](design.md)。

## 一帧怎么走

```text
 被控端（Host）                                                控制端（Viewer）
 ┌──────────┐   ┌──────────┐   ┌──────────────────────────┐        ┌──────────────────────┐
 │ 采集     │──▶│ 分块比对 │──▶│ 分流（每个变化块选一条路）│        │ 解码线程             │
 │ SCK/X11  │   │ 64×64 块 │   │  1 平移  → 拷贝指令       │ surface│  拷贝 / 缓存 / 无损块 │
 │ 或应用喂 │   │ 哈希     │   │  2 缓存  → 贴回槽位       │──────▶│  → BGRA 画布          │
 └──────────┘   └──────────┘   │  3 少色  → 调色板无损     │        │  HEVC 硬解 → 视频块   │
                               │  4 其余  → HEVC（整屏或   │ video  │  按帧号合成后         │
 ┌──────────┐                  │           视频区域）      │──────▶│  on_frame(BGRA, 变化) │
 │ 光标采样 │──────────────────┴──────────────────────────┘ cursor │                      │
 │ 120 Hz   │─────────────────────────────────────────────────────▶│ on_cursor（另一线程） │
 └──────────┘         码率控制 ◀── 传输统计（带宽、RTT、队列）       └──────────────────────┘
```

1. **采集**：macOS 用 ScreenCaptureKit（带系统脏区，不采集声音），Linux 用 X11 MIT-SHM，Wayland 下用 xdg-desktop-portal + PipeWire（带合成器脏区）；应用也可以自己调用 `SubmitFrame` 喂 BGRA 画面。光标不画进画面，单独采样。
2. **分块比对**（`core/tile/tile_diff`）：画面切成 64×64 块，逐块算内容哈希，和上一帧比较。没有块变化的帧整帧跳过。
3. **分流**：变化块按代价从低到高试四条路。
   - **平移**（`core/tile/motion`）：变化块多时，取每块最有细节的一行做滚动哈希，在上一帧里找位置投票出位移，逐块精确比对后发拷贝指令。滚动区和窗口边缘的块按 4 像素宽的条和像素行再拆，只把新露出的像素送去编码。
   - **块缓存**（`core/tile/tile_cache`）：接收端有 1024 个槽，Host 维护一份索引决定存什么、贴什么，接收端只照做，所以两边状态永远一致。即将被盖住的、停留过一阵的块，以及反复出现的块（转圈动画）先存起来，内容再出现时直接贴回。拖窗露出的背景即使只占块的一部分，也按条贴回。
   - **调色板无损层**（`core/codec/lossless`）：颜色少的块（文字、界面）编成调色板 + 索引。所有块共用一条跨帧的 deflate 流，前面发过的字形和工具栏下次大多只是回溯引用。拷贝和缓存指令也走这条流，几乎不占码率。送进硬件编码器的画面里这些块被抹成底色，打字时往往一帧视频都不用编。
   - **HEVC**：剩下的块交给硬件编码器。macOS 用 VideoToolbox（优先低延迟模式），Linux 用 FFmpeg（NVENC 优先，否则 x265，可选 VAAPI）。只支持 HEVC，没有 H.264 回退。
4. **视频区域**（`core/tile/video_region`）：相邻多块在大部分帧里都要走视频编码时（播放视频、游戏），这块区域单独开一路 HEVC 码流，只编裁剪画面，有自己的码率和 QP 下限，整屏编码器在这里不再花码率。视频暂停约 0.5 秒后区域关闭，区域内的块再走一次普通路径，暂停画面会变清晰。
5. **发送**：同一帧的片段带同一个帧号，接收端收齐后一起合成，不会出现撕裂。

接收端维护一张全分辨率 BGRA 画布，按发送端生成的顺序应用拷贝、缓存、无损块和视频块，然后通过 `on_frame` 把画布和本帧变化的矩形交给应用绘制。

## 首帧与补全

- **渐进式首帧**（`progressive`）：首帧和每次全屏刷新先只发视频版本，比无损版小好几倍。文字和界面块随后按码率的约 80% 逐帧补成无损，补完后再给剩下的视频块发一张清晰的帧内画面。纯色块直接走无损。
- **静止区精修**（`refine_frames`）：视频块变化后，接收端在之后 N 个视频帧里继续采用编码器对它的精修。
- **关键帧请求**：接收端解码失败或中途接入时自动要一次全屏刷新（最多每 250 ms 一次），应用也可以调用 `RequestKeyframe()`。画面静止时，Host 100 ms 后用最后一帧补发（拥塞时跳过的帧也一样）。

## 传输

传输是可插拔的（`rda_transport_vtbl` / `rdaccel::Transport`）。库自带进程内管道、TCP 和 UDP，应用也可以接自己的 QUIC、WebRTC 或中继。每条连接分 6 个通道：

| 通道 | 内容 |
| --- | --- |
| control | 握手和版本协商、心跳、关键帧请求、断开通知 |
| input | 键盘、鼠标、滚轮、文字输入（USB HID 键码） |
| surface | 拷贝、缓存、无损块（那条 deflate 流），必须有序 |
| video | HEVC 码流和本帧的区域列表 |
| cursor | 指针位置和光标图像，高优先级 |
| message | 应用消息（剪贴板文本或自定义类型），可靠有序 |

**UDP 可靠传输**（`core/net/reliable_transport`）：

- **丢包恢复**：每个包有新的包号，接收端按区间确认，发送端判定丢失后重传那段字节。判定条件是之后三个包已确认、明显早于一个往返，或探测超时。每个通道是独立的有序字节流，丢一个视频包不会卡住输入、光标和控制。
- **拥塞控制**：简化的 BBR。从确认里测投递速率和最小 RTT，按测得的瓶颈带宽发包，在途字节不超过约两个带宽时延积，所以不会把路由器缓冲塞满，也不会因为随机丢包就退让。弱网下的几处细节（排队时冻结平滑 RTT、只让“确认比发送慢”的样本拉低估计、裁剪时至少留 4 个样本）见 [CHANGELOG](../CHANGELOG.md) 0.4.0。

**加密**（`core/transport/secure_transport`）：TCP 和 UDP 上的数据用应用提供的 32 字节密钥做 XChaCha20-Poly1305（内置 Monocypher）。不加密必须显式 `allow_plaintext`。密钥由应用的配对流程产生，库不做证书和配对。

**连接层**（`core/session/link`）：握手和版本协商（能力位）、心跳和断线检测、关闭时通知对方、认证钩子、应用消息。

## 自适应码率

`core/session/rate_control` 由传输统计（带宽估计、RTT、发送队列）驱动：

- 发送队列排空时间超过 `max_queue_ms`（默认 50）时跳过这一帧，下一帧带上所有变化，所以延迟有上限。
- 排队或跳帧时，目标码率立刻降到估计带宽的 85%；畅通时每 250 ms 最多升 25%，最高到估计的两倍。
- RTT 明显高于最低值时，说明网络里已经排起队，即使带宽估计还没反应过来，也按排队处理。
- 编码器实际产出比要求多时（VideoToolbox 低码率下会超发），按比例少要。
- 到了码率下限 `min_bitrate_bps` 以后，会话总发送量限制在估计的 90% 以内，超出就跳帧；网络里有常驻队列时还会隔帧跳过。因为硬件编码器每帧有近乎固定的开销，到了下限只能靠少发帧来降。

## 光标

光标走独立通道，不进画面。Host 发指针位置（每次约 17 字节）和光标图像（每种只发一次，deflate 压缩，两端各记最近 64 种），Viewer 通过 `on_cursor` 拿到后自己画（可用 `DrawCursor()`，或交给系统硬件光标）。`on_cursor` 在传输的接收线程调用，不排在画面解码后面，大帧还在路上时指针照样跟手。按 `capture_width` / `capture_height` 缩放时，光标图像和热点按同样比例缩放。

## 输入

Viewer 用 `SendInput` / `SendText` 发输入事件，键码统一用 USB HID 用法码，坐标是被控端像素。能出字的键一律发文字事件，只有不出字的键发按键事件。Host 通过 `on_input` 收到事件，可以交给可选的 `rdaccel::Injector` 注入系统：macOS 用 CGEvent（需要辅助功能权限），Linux 用 XTest。

## 代码布局

```text
include/rdaccel/   公开 API：rdaccel.h（C ABI，唯一的二进制接口）、rdaccel.hpp（只有头文件的 C++ 包装）
src/               公开 API 的实现（src/capi/ 是 C ABI 层）
core/              平台无关的内部实现（命名空间 rdacore），可在任何机器上跑单元测试
  capture/         采集接口、合成画面
  tile/            分块比对、平移检测、块缓存索引、视频区域
  codec/           调色板无损层、HEVC 码流格式、编解码器接口
  session/         Host / Viewer 会话、连接层、码率控制、光标通道
  net/             UDP、可靠传输、网络模拟
  transport/       进程内管道、TCP、加密层
  proto/ io/ log/  线上消息、录制文件、日志
platform/mac/      ScreenCaptureKit、VideoToolbox、光标、显示器列表、CGEvent 注入
platform/linux/    X11 / XFixes / RandR、Wayland（portal + PipeWire）、FFmpeg（NVENC、x265、VAAPI）、XTest 注入
third_party/       Monocypher
examples/          只用公开 API 的示例和工具
tests/ fuzz/       GoogleTest 单元测试、libFuzzer 目标
```

平台相关代码只在 `platform/` 里，`core/` 只通过抽象接口访问采集、编解码和网络，所以单元测试可以用假实现在 Linux CI 上跑。

## 配置开关

每项优化都能在 `HostConfig` / `rda_host_config` 里单独关掉，方便对比：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `lossless` | 开 | 调色板无损层 |
| `lossless_max_colors` / `lossless_max_colors_hot` | 256 / 4 | 静止块、每帧都在变的块走无损的颜色上限 |
| `detect_moves` | 开 | 滚动和拖窗检测 |
| `tile_cache` | 开 | 块缓存 |
| `refine_frames` | 8 | 静止区精修帧数 |
| `video_regions` | 开 | 视频区域单独编码 |
| `video_region_min_qp` / `video_region_bitrate_bps` | 27 / 0 | 视频区域 QP 下限、码率（0 = 按面积分总码率，至少 1/4） |
| `min_qp` | 24 | 整屏编码器 QP 下限（0 = 编码器自定） |
| `adaptive_bitrate` / `min_bitrate_bps` / `max_queue_ms` | 开 / — / 50 | 自适应码率 |
| `cursor_channel` / `cursor_poll_hz` / `capture_cursor` | 开 / 120 / 关 | 光标通道；`capture_cursor` 改回画进画面 |
| `progressive` | 开 | 渐进式首帧 |
| `display_index` | 0 | 采集哪块显示器（0 = 主显示器） |
| `encoder` | 自动 | 指定编码器，比如 `hevc_vaapi` |

完整字段和含义见 [`rdaccel.h`](../include/rdaccel/rdaccel.h) 和 [集成指南](integration.md)。
