# 更新记录

版本号遵循语义化版本。1.0 之前，次版本号（0.x）变化时 C ABI 可能有不兼容改动；
补丁版本（0.x.y）保持兼容。`find_package(rdaccel 0.3)` 只接受同一个 0.3.x。

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
