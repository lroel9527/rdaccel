# Mac 复测清单（M2）

M2 的功能都是在云端（Linux x265 + GitHub 的 macOS CI，VideoToolbox 软件编码）开发和验证的。这份清单列出必须在真实 Mac（Apple Silicon，VideoToolbox 硬件编解码）上再确认的项目：大部分由一键脚本自动跑，少数要人看一眼。

## 静音

整个过程不发声：rdaccel 没有任何音频代码（ScreenCaptureKit 配置了 `capturesAudio = NO`），脚本不调用 `say`、`afplay`、`osascript` 或其他会发声的命令，也不改音量。

## 一键脚本

```bash
cd rdaccel && git pull
tools/mac_retest.sh                 # 全部，大约 5 分钟
tools/mac_retest.sh --skip-screen   # 不采集真实屏幕（不需要屏幕录制权限）
```

- 第一次采集屏幕时 macOS 会要求给终端开“屏幕录制”权限（系统设置 → 隐私与安全性 → 屏幕与系统录音）。这是一个对话框，不发声。开好权限后重跑一次。
- 第 2 步里有 8 秒需要屏幕和鼠标保持不动（终端里会提示）。
- 结果写在 `retest-<时间>/summary.txt`，每一步的完整输出在同一目录。把 `summary.txt` 发回来就行。

脚本做的事情，以及每项要对照的数字：

| 步骤 | 检查什么 | 期望 |
| --- | --- | --- |
| `unit_tests`、`platform_tests` | 全部单元测试；VideoToolbox 编解码；公开 API（含两路编码器同时工作、运行中改码率、静止屏幕补发关键帧） | 全部 PASSED |
| `screen_native` | 真实屏幕经 UDP 回环；光标图像大小 | 光标图像是像素尺寸（Retina 上约为点尺寸的 2 倍，比如 32×32 以上） |
| `screen_scaled` | 抓屏缩到 1920×1200 时光标跟着缩 | 光标图像比 `screen_native` 小，比例和画面一致 |
| `screen_static_keyframe` | 屏幕静止时接收端要关键帧（ScreenCaptureKit 这时不出帧） | `frames after the keyframe request:` 至少 1 |
| `bench_all` | 各场景码率、编码耗时、PSNR，背景是 `screen_scaled` 那张真实截图 | 和 M1 的 Mac 结果同一量级（窗口拖动约 775 kbps / 41.5 dB，滚动约 736 kbps） |
| `bench_video_regions` / `_noregions` | 视频区域：同时开两路 VideoToolbox 会话 | 开区域时码率和 `enc ms` 都明显更低（Linux 上 4652 → 1764 kbps，16.5 → 6.0 ms），没有报错 |
| `bench_pointer` | 光标通道 | 稳态约 20 kbps，视频帧个位数 |
| `netsim_drop` | 运行中把 VideoToolbox 码率从 4 Mbps 降到 1 Mbps | 延迟 p95 在 150 ms 以内，没有编码错误 |
| `netsim_progressive_on` / `_off` | 渐进式首帧 | 开时 `first picture` 明显更早；`start:` 一行之后几秒内升到和关时相同的 PSNR |
| `netsim_cursor` | 指针延迟 | p50 约 20 ms（模拟的单向时延） |

## 需要人看一眼的

这些脚本做不了，有空时顺手看：

1. **回环窗口里的光标**：`open build-retest/bin/rdaccel_loopback.app`。确认窗口里的光标位置对、大小和真实光标一致（Retina），窗口缩放后还对得上。看完关掉窗口。
2. **多显示器**（如果有外接屏）：现在只按主屏算光标坐标。把鼠标移到副屏，回环窗口里的光标应该消失或停在边上，不能跳到奇怪的位置。
3. **两台机器**：一台跑 `build-retest/bin/rdaccel_serve`，另一台（Mac 或 Linux）跑 `rdaccel_view <ip> --seconds=20`。看每秒的 fps 和码率，`errors` 应该是 0。

## 记录结果

结果回来以后，把数字更新到 README 的“当前进度”，并去掉“还要在真实 Mac 上复测”那句。
