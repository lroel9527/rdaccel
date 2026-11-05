# 更新记录

版本号遵循语义化版本。从 1.0 起，同一主版本内 C ABI 只加不改（新函数、结构体末尾加字段），
`find_package(rdaccel 1.0)` 接受任何 1.x。1.0 之前次版本号（0.x）变化时 C ABI 可能有不兼容改动。

## 未发布

- **画面静止后不再留下残块、也会变清楚**（ScreenCaptureKit 在画面不变时不再出帧）：编码器丢掉的那张画面里的变化、
  超出无损预算先走视频的块、还在渐进细化的块，以前要等下一次屏幕变化才补上，窗口关掉后可能一直留着旧色块或糊块。
  现在 Host 在没有新帧时每 100 ms 用自己保存的最后一帧补发，直到这些都补完。因为拥塞跳过的帧之后，下一帧整屏比较，
  不再只看采集端给的变化区域（那只相对于被跳过的那一帧）。
- **慢链路上欠下的无损块会发不出去**：一块细密文字的无损数据可能比无损桶的上限还大（200 kbps 时桶最多约 1.2 KB），
  以前要等桶里攒够一块才发，于是永远等不到，那块一直停在视频质量。现在桶满了就至少发一块。
- **`rdaccel_remote` 跟手**：窗口里用本机系统指针（形状取自对方光标），不再等对方光标传回来；画面写进两块 IOSurface，
  每帧只拷贝变化的区域，不再每帧在主线程上传整张图。标题栏的延迟改成 `pointer` 和 `input→screen` 两项，
  以前的 `input→update` 在画面持续变化时只是一帧间隔，不反映真实延迟。
- **macOS 最低版本**：从源码构建时默认的 `CMAKE_OSX_DEPLOYMENT_TARGET`（14.0）以前不生效，产物跟随构建机的系统版本
  （在 macOS 27 上构建的程序拷到 macOS 14–26 上跑不起来）。现在默认值在 `project()` 之前设好，已有的构建目录也会补上；
  `-DCMAKE_OSX_DEPLOYMENT_TARGET=` 或环境变量 `MACOSX_DEPLOYMENT_TARGET` 仍然优先。发布包本来就在 macOS 14 上构建，不受影响。

## 1.0.0-rc1

1.0 的候选版：C ABI 从这一版起冻结，1.x 之内只加不改。soname 改为 `librdaccel.so.1` / `librdaccel.1.dylib`，
`find_package(rdaccel 1.0)` 接受任何 1.x。和 0.4 相比 ABI 不兼容：去掉了两个阻塞的 C 函数，
`on_error` 的状态码有了约定，`rda_host_config` 末尾加了 `keep_serving`。在 Mac 和 Linux 上实测之后发 1.0.0。

- **`rdaccel_remote`**（macOS，#35）：带窗口的跨机器接收端，显示对方屏幕和光标，把鼠标、滚轮、键盘（含 Command 快捷键）发给
  `rdaccel_serve --inject`；窗口失去焦点时松开还按着的键。标题栏显示“input→update”：本机发出输入到下一张有变化的画面的时间。
  macOS 发布包（共享库版）的 `bin/` 里带这个程序。只是示例，库和 C ABI 没有变化。
- `rdaccel_serve`：接收端离开后在同一端口等下一个（`keep_serving`），不再退出；`--once` 恢复原来的行为。
- **0.4 Mbps 的链路**（#32）：无损块按真实字节扣一个按半个码率充值的桶，渐进细化也从桶里扣；
  估计带宽骤降时目标码率立刻跟到估计的 85%；区域列表纵向合并、一个块拆成很多视频碎片时只列外框；
  匹配碎成很多块的帧不做拷贝。netsim 4 → 0.4 Mbps 降速瞬间最差延迟约 200 ms（之前 0.5–1.2 s），
  4 → 1 Mbps 送达 91–94% 的帧。`netsim --trace` 和 `tools/netsim_trace_summary.py` 用来看每帧由什么组成。
- **慢链路上滚动先糊后清**（#33）：内容在动、上一帧已经用完无损预算时，桶只按 0.2 倍码率充值，放不下的先走视频；
  画面基本静止后（上一帧变化的块不超过 2%），所有停在视频质量的块排队，在桶内无损补发。
  netsim 1080p 0.4 Mbps 滚动时送达的帧 38% → 46%，p95 约 300 → 250 ms，停顿时最长无帧间隔 2 s → 0.5–1 s；
  20 Mbps 滚动后第一次停顿时精确像素 40% → 86%。`netsim --trace` 增加 `psnr`、`exact_pct` 两列。
- **接收端内存损坏**（#33，fuzz 找到）：光标通道在传输线程上解析，其他通道在解码线程上解析。对端把画面消息放到光标通道时，
  两个线程会同时改帧状态，导致堆损坏。现在每个线程只处理自己那一类消息。
- **UDP 传输**：收包时间改在加锁之后读取。以前伪造的确认可以让往返时间回绕成约 2^64 微秒（fuzz 找到）。

- **一个 Host 依次服务多个接收端**（`rda_host_config.keep_serving`，默认 1）：接收端离开、超时、被 `on_auth` 拒绝或密钥不对之后，
  `rda_udp_bind` / `rda_tcp_bind` 上的 Host 报 `RDA_STATE_DISCONNECTED`（带原因）再报 `RDA_STATE_CONNECTING`，在同一个端口等下一个；
  抓屏不停，下一个接收端从关键帧开始，统计跨接收端累计。以前一个密钥不对或被拒绝的接收端就会让 Host 永久断开，
  只能重建 Host 和抓屏。
- **杂包占不住位置**：UDP 上第一个包不像本协议的连接开头（端口扫描、上一个接收端迟到的包）时，Host 不把它的发送方当成对端；
  像开头但始终不握手的对端在 `peer_timeout_ms` 后断开。
- `examples/integration/support_host.c` 改成一个 Host 服务所有接收端。
- **去掉阻塞的 `rda_tcp_listen` / `rda_udp_listen`**（C++ 的 `ListenTcp` / `ListenUdp`）：它们一直阻塞到对端出现，
  期间既不能取消也收不到状态回调。改用 `rda_tcp_bind` / `rda_udp_bind`（`BindTcp` / `BindUdp`）：马上返回，
  马上创建 Host，用 `on_state` 等对方连上；Host 在对方连上之前保留最新一帧。
- **头文件**：模拟网络和合成桌面单独归到“testing and benchmarks”一节（仍属稳定 ABI）；用 `-fshort-enums`
  之类让枚举不是 4 字节的编译选项会直接编译失败；`rdaccel.hpp` 写明只承诺源码兼容。
- **安全**：设置了 `on_auth` 时，被拒绝的接收端以前在断开前仍能收到屏幕尺寸（`on_stream_info`）和光标，
  还会先报一次 `RDA_STATE_CONNECTED`。现在认证通过之前不发任何画面相关的数据：Host 只保留最新一帧，
  通过后再发尺寸、光标和关键帧。没有 `on_auth` 时行为不变（录制等没有接收端的传输照旧）。
- **回调时机**：`rda_host_create` / `rda_viewer_create` 返回之前不会调用任何回调，回调里可以直接用 create 写出的句柄。
  以前对端已经在等时（`rda_udp_bind`、管道），`on_state` 可能在句柄写出之前就执行。
- **集成示例**：`examples/integration/`，一个纯 C 的远程协助 host + viewer，按外部项目的方式（`find_package` 或 pkg-config）
  针对安装好的库构建；CI 在 Xvfb 里跑 `smoke.sh`。
- **Wayland 抓屏**：Linux 在 Wayland 会话下通过 xdg-desktop-portal 的 ScreenCast 接口和 PipeWire 采集屏幕
  （GNOME、KDE、sway 等）。用合成器给的脏区；光标优先走 PipeWire 光标元数据（进光标通道），没有时画进画面；
  授权在进程内保留，`RDACCEL_PORTAL_TOKEN_FILE` 可跨进程保留。依赖 `libpipewire-0.3`、`gio-2.0`，
  CMake 选项 `RDACCEL_PIPEWIRE=AUTO|ON|OFF`。CI 在无头 sway + PipeWire + portal 里测试，并用公开 API 录一段 Wayland 屏幕再解码。
- **错误回调的约定**（C ABI 语义改动）：`on_error` 只在 Host 停下或光标通道不可用时调用，状态码说明原因：
  `RDA_ERR_UNSUPPORTED` 表示没有光标通道，画面照常；`RDA_ERR_CODEC` 表示编码器建不起来；
  `RDA_ERR_PERMISSION` / `RDA_ERR_CLOSED` 表示抓屏停了（权限被收回，或 Wayland 共享被用户结束、屏幕没了）。
  单帧编码失败不再回调，只记日志并计入 `encode_errors`。抓屏中途停止以前没有任何通知，现在 PipeWire 和
  ScreenCaptureKit 会报告。`rdaccel.hpp` 的 `SetErrorHandler` 新增带 `rda_status` 的重载，旧的只收字符串的写法照旧可用。
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
