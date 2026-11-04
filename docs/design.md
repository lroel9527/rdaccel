# 设计背景与范围

## 目标

| 项 | 目标 |
| --- | --- |
| 场景 | 以办公为主：代码、文档、网页、IDE，兼顾偶尔出现的视频窗口和动画 |
| 带宽 | 静态画面低于 100 kbps；打字和滚动 300 kbps 到 1 Mbps；全屏视频按可用带宽自适应 |
| 延迟 | 局域网端到端低于 50 ms；公网在网络 RTT 之上再加不超过 40 ms |
| 文字 | 必须清晰；可以先给模糊预览，停下后很快补成无损 |
| 平台 | macOS（Apple Silicon，VideoToolbox 硬件编解码）和 Linux（X11 / Wayland，FFmpeg） |
| 编码 | 只用 HEVC，没有 H.264 回退 |

实测数字见 [评测](benchmarks.md)。

## 为什么是分层混合编码

只靠硬件编码器做不到上面的目标：

- **硬件编码器没有屏幕内容工具。** VideoToolbox 是通用视频编码器，没有帧内块拷贝（IBC）和调色板模式，也没有逐块 QP 或 ROI。而这两类工具正是文字压缩的主要收益来源：HEVC SCC 里关掉 IBC，文字图形序列的码率在低延迟配置下增加 13%，全帧内增加 56%；关掉调色板分别增加 7.5% 和 19%（Liu 等 2015）。
- **最大的带宽收益不在编码器里。** 办公画面大部分帧要么没变，要么只是平移（滚动、拖窗），要么是以前出现过的内容（切回标签页、弹出菜单）。脏区、平移检测和块缓存能把这些帧压到接近零，工作量也最小。NX 在极低带宽下好用，主要靠的就是差分加缓存。
- **文字要单独处理。** 4:2:0 色度下采样会让彩色字和亚像素边缘发糊，低码率下的量化还会在锋利边缘周围产生振铃。文字块走无损调色板层，就不受这两点影响。

所以 rdaccel 照 RDP 图形通道（MS-RDPEGFX）的思路，在一条连接里混用几种编码：没变的块不发，平移的块只发拷贝指令，以前出现过的块从缓存贴回，少色块走 CPU 上的无损调色板层（对应 SCC 的调色板模式，跨帧的 deflate 流起到便宜的“跨帧 IBC”作用），只有剩下像视频的部分交给硬件 HEVC。首帧先模糊、再补成无损的做法来自 RemoteFX Progressive Codec。具体实现见 [架构](architecture.md)。

### 没有采用的方向

| 方向 | 原因 |
| --- | --- |
| 整帧硬编（Sunshine / Moonlight 式） | 简单，但静止和打字时浪费码率，文字在低码率下发糊；在 rdaccel 里它是 M0 基线，用来对比 |
| CPU 软件 AV1 + SCC 工具 | AV1 主档次有 IntraBC 和调色板，适合小面积文字，但全屏 4K 实时软编太重；Apple 没有 AV1 硬编 |
| AVC444 式拆帧拿全色度 | 文字已经走无损层，拆帧只影响图像和视频里的细色边，收益小于两路码流同步的复杂度，暂不做 |
| 学习型编码（DCVC-RT 等） | 压缩率已超过 H.266，但在 Mac 上实时跑 4K 还做不到；“文字感知”的解码端增强没有成熟工作，模型还可能改字 |
| libwebrtc | 太重，也不需要浏览器兼容；带宽估计和拥塞控制在库里自己做（简化的 BBR） |

## 库的范围

rdaccel 是给其他程序集成的库，不是应用。划分原则是：做错了会拖慢画面或浪费带宽的事，库自己做；和产品形态或系统权限相关的事，留给应用。

| 功能 | 归属 |
| --- | --- |
| 屏幕采集（或应用喂帧）、选择显示器、分辨率中途变化 | 库 |
| 分区编码、HEVC 硬编解、光标通道、自适应码率 | 库 |
| 内置传输（UDP、TCP、进程内管道）、从应用已打通的 UDP socket 创建、非阻塞连接 | 库 |
| 包加密（应用给 32 字节密钥）、认证回调 | 库 |
| 输入事件和应用消息（剪贴板等）的可靠传送 | 库 |
| 输入注入（CGEvent、XTest） | 可选模块 `rdaccel::Injector` |
| 界面、配对、账号、信令、NAT 穿透、中继、文件传输 | 应用 |
| 多个观看者、Windows、H.264 / AV1、音频 | 暂不做 |

### 接口约定

- **稳定 C ABI**：`<rdaccel/rdaccel.h>` 是唯一的二进制接口。所有符号用 `rda_` 前缀，对象是不透明指针，配置结构体第一个字段是 `struct_size`，用 `*_default()` 填默认值再改，回调都带 `void* user`。动态库只导出 `rda_*`。这样换编译器或标准库不受影响，也能直接给 Swift、Rust、Python 绑定。`<rdaccel/rdaccel.hpp>` 是只有头文件的 C++ 包装。
- **传输所有权**：传输一旦交给 `rda_host_create` / `rda_viewer_create` 就归库管，创建失败也由库释放。
- **线程**：库自己开线程，应用不用驱动事件循环。`destroy` 返回后保证不再回调。细节见 [集成指南](integration.md)。
- **错误和日志**：函数返回 `rda_status`；详细文字用 `rda_*_last_error()` 取。后台错误通过 `on_state` 上报。日志是一个全局回调，不设就不输出，不记录画面、按键和消息内容。
- **版本**：语义化版本。1.0 之前次版本之间可以不兼容；1.0 之后同一主版本只在结构体末尾加字段、加函数。线上协议在握手里协商版本和能力位，录制文件保持向后可读。CI 用 abidiff 检查 C ABI。
- **消息通道**：`type` 小于 `0x10000` 的保留给库定义的标准格式（第一个是 UTF-8 纯文本剪贴板），其余归应用。可靠有序，和视频分开排队。
- **加密**：密钥由应用的配对流程产生，库只管加密（Moonlight、Parsec 都是这样），不在库里做 TLS 和证书。密钥不对时以 `RDA_ERR_AUTH` 断开，一端加密一端不加密直接拒绝。

### 参考过的库

| 参考 | 借鉴 |
| --- | --- |
| Parsec SDK | 纯 C 头文件、不透明句柄、Host / Client 两套函数、应用自定义消息通道 |
| x265 / libvpx | 参数结构体 + `*_default()`、头文件里的版本宏、主版本号作 SOVERSION |
| moonlight-common-c | 核心不碰平台；连接状态和断开原因通过回调上报；密钥由外部配对产生 |
| FreeRDP | 虚拟通道：编号 + 可靠有序 + 应用注册处理函数 |
| libvncserver | 一个文件能跑的最小例子；应用自己喂帧再标脏区 |

## 参考文献

- Liu, Xu, Lei, Jou. [Overview of HEVC extensions on screen content coding](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/E12DCF69B0A59DD269EFE14CD1AFBFBE/S2048770315000116a.pdf/overview-of-hevc-extensions-on-screen-content-coding.pdf). APSIPA Trans. SIP, 2015.
- Nguyen 等. [Overview of the Screen Content Support in VVC](https://www.microsoft.com/en-us/research/?p=798274). IEEE TCSVT 31(10), 2021.
- Reddy 等. [Subband Decomposition for High-Resolution Color in HEVC and AVC 4:2:0 Video Coding Systems](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/MSR-TR-2014-31.pdf). MSR-TR-2014-31.
- Microsoft. [MS-RDPEGFX: RFX_AVC444V2_BITMAP_STREAM](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/3b337b87-f478-4786-a63b-97794aa72075)、[RemoteFX Progressive Codec](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/f8c3c134-bf99-45eb-be7e-d22c8178255f).
- Och 等. [Improved Screen Content Coding in VVC Using Soft Context Formation](https://arxiv.org/html/2305.05440v2). arXiv:2305.05440.
- Jia 等. [Towards Practical Real-Time Neural Video Compression (DCVC-RT)](https://arxiv.org/abs/2502.20762v2). CVPR 2025.
- Cheng 等. [LSCD: A Large-Scale Screen Content Dataset for Video Compression](https://arxiv.org/html/2308.09332v1). arXiv:2308.09332.
- Zoe Liu. [AV1 for RTC: Current and Future](https://atscaleconference.com/?p=74622). @Scale 2022.
- Apple. [WWDC21 10158: Explore low-latency video encoding with VideoToolbox](https://developer.apple.com/wwdc21/10158)、[WWDC22 10155: Take ScreenCaptureKit to the next level](https://developer.apple.com/videos/play/wwdc2022/10155/)、[SCStreamFrameInfo.dirtyRects](https://developer.apple.com/documentation/screencapturekit/scstreamframeinfo/dirtyrects).
- [NoMachine / NX technology](https://en.wikipedia.org/wiki/NoMachine)（Wikipedia）。
