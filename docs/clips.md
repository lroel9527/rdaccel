# 测试片段（clips）

改 Host 怎么编码以后，要知道它在真实屏幕上是更省还是更费、画面是更好还是更差。合成场景（`rdaccel_netsim`、`rdaccel_bench`）拿一张截图做动画，看不到真实桌面上的东西：窗口阴影、半透明面板、输入法候选窗、网页里的图文混排。测试片段把一段真实屏幕原样录下来，之后可以拿任意版本的 Host 反复重放，在完全相同的内容上比较。

工具在 `tools/clip/`：

| 文件 | 作用 |
| --- | --- |
| `rdaccel_clip`（`rdaccel_clip.cpp`） | 录制、把示例场景生成片段、测评一个片段、取出某一帧 |
| `clip.h` | 片段文件格式 |
| `compare.sh` | 当前代码和某个旧版本在一组片段上逐个对比 |
| `scenarios/record.sh` | 在 Mac 上自动操作真实应用，每个场景录一个片段 |
| `scenarios/drive.swift` | `record.sh` 用的键盘鼠标驱动 |
| `scenarios/make_content.sh` | 生成场景要显示的内容（长网页、测试视频、终端脚本） |

`rdaccel_clip` 用的是内部接口（`session::HostSession`），随 `RDACCEL_BUILD_TOOLS`（顶层构建时默认开）构建，不安装：

```sh
cmake --build build --target rdaccel_clip
```

## 隐私

**真实屏幕的片段显示的是录制时屏幕上的一切**（邮件、对话、文件名、侧栏）。片段、从片段取出的帧和截图只留在录制它的机器上：不提交到仓库，不推到任何远端，不附到发布或问题单，不发给任何外部服务。`*.clip` 和 `clips/` 在 `.gitignore` 里。仓库里只放工具的代码；测试内容由 `make_content.sh` 现场生成（系统自带的壁纸、仓库里的 CHANGELOG、ffmpeg 生成的视频），也不提交。

## 片段格式

只存屏幕变化了的帧（和屏幕采集一样：画面不变时采集器不送帧），无损。每帧：采集时间、宽高、系统给的脏矩形、这一帧和上一帧按字节异或后 deflate 的数据。文字界面一分钟几 MB 到几十 MB，视频和全屏滚动大得多（36 秒 1280 × 720 视频约 1.3 GB）。

## 录制

```sh
build/bin/rdaccel_clip record clips/mine.clip --seconds=60
```

需要终端有“屏幕录制”权限。录的时候照常使用，这就是一段“日常使用”片段。

### 按场景自动录制（macOS）

```sh
tools/clip/scenarios/make_content.sh            # 生成内容到 clips/content（视频需要 ffmpeg）
tools/clip/scenarios/record.sh                  # 8 个场景，约 6 分钟，片段在 clips/
tools/clip/scenarios/record.sh clips drag ime   # 只录其中几个
```

| 场景 | 内容 |
| --- | --- |
| `terminal` | 终端刷屏 6 秒、停 4 秒，四轮 |
| `typing` | 文本编辑里按人手速度打英文代码 |
| `ime` | 拼音输入（候选窗出现、选字） |
| `idle` | 静止桌面（时钟、光标） |
| `scroll` | Safari 里滚动一个图文长页（停停走走，再快速滚） |
| `drag` | 拖动文本编辑窗口绕圈，停一下再拖 |
| `video` | QuickTime 播放测试视频，中间暂停 4 秒 |
| `switching` | 在 Safari 和文本编辑之间切换，Safari 进出全屏 |

注意：

- 终端需要“屏幕录制”和“辅助功能”权限（驱动要发键盘鼠标事件）。
- 录制期间**不要有人用这台机器的屏幕**（包括远程连着的人）：别人的操作会录进去，而且驱动每次打字、点击前都检查前台应用是不是它要操作的那个，不是就停下（这一段片段就不是那个场景了，重录）。
- 没有声音：测试视频没有音轨，脚本不播放任何声音，也不碰音量。
- 结束后把它打开的窗口关掉（文本编辑和 QuickTime 原来没开就不保存退出），输入法恢复成原来的。

## 测评

```sh
build/bin/rdaccel_clip eval clips/drag.clip --text=1
```

把片段送进一对 HostSession / ViewerSession（本机的硬件编解码器，进程内管道），按 Host 在真实屏幕上的做法：屏幕变化时送这一帧；有东西欠着时每 100 ms 再送一次上一帧。快链路、固定码率（`--bitrate`，默认 8 Mbps），不模拟网络。输出一行：

| 项 | 含义 |
| --- | --- |
| sent / kbps | 发出的总字节和平均码率，其中视频和无损层各多少 |
| exact | 对端显示的画面里和原画面逐像素相同的比例，按时间加权（每个画面算到下一帧为止） |
| PSNR | 同样按时间加权 |
| pauses: exact | 片段里每段 0.5 秒以上没有变化的停顿，结束时逐像素相同的比例的平均 |

逐像素相同只是一个方面：视频细修后的块看着清楚，但不算在 exact 里。`--csv=FILE` 输出每一帧发了什么（视频、无损字节，无损块数，拷贝和缓存的块数，补发的块数），用来找流量花在哪一帧、哪一类块上。

个别帧在对端没有自己的画面（输出末尾会说有几帧），这时量的是对端当时显示的画面，也就是用户看到的。

## 对比两个版本

```sh
tools/clip/compare.sh v1.9.5 clips
```

把 `v1.9.5` 检出到临时目录单独构建（库，加上当前的 `tools/clip`），和当前代码一起把每个片段在普通模式和文字模式各跑一遍，打一张表：两边的字节数和变化、逐像素相同的比例。临时目录用完删掉。完整输出写在片段目录的 `compare-*.txt`。

硬件编码器每次的结果略有不同，同一个片段重跑差 1–2% 以内属正常；看差别要看明显的、方向一致的变化，并且用多个片段。

## 合成场景做成片段

```sh
build/bin/rdaccel_clip render clips/syn-scroll.clip --scenario=scroll --background=SCREEN.ppm --seconds=20
```

`examples/scenes.h` 的场景（idle typing scroll drag zoom video pause）按 60 Hz 渲染成片段，和真实片段用同一套测评。

## 例子：1.9.6 是怎么找到的

实际使用中感觉流量变大。9 段真实片段（这台 Mac 上录的）在 1.9.4、1.9.5 上对比，文字模式两版一样，不是升级造成的；但同一组数据显示文字模式拖窗口比普通模式多 69%。`--csv` 显示多出来的都在窗口移动的帧里，再按块的形状统计，发现是窗口阴影留下的几十万个 4 像素宽的小边角各带一个调色板。改完再跑同一组片段：拖窗口 −59%、滚动 −38%，其余场景不变。见 [CHANGELOG](../CHANGELOG.md) 1.9.6。
