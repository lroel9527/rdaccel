# 测试片段（clips）

改 Host 怎么编码以后，要知道它在真实屏幕上是更省还是更费、画面是更好还是更差。合成场景（`rdaccel_netsim`、`rdaccel_bench`）拿一张截图做动画，看不到真实桌面上的东西：窗口阴影、半透明面板、输入法候选窗、网页里的图文混排。测试片段把一段真实屏幕原样录下来，之后可以拿任意版本的 Host 反复重放，在完全相同的内容上比较。

工具在 `tools/clip/`：

| 文件 | 作用 |
| --- | --- |
| `rdaccel_clip`（`rdaccel_clip.cpp`） | 录制、把示例场景生成片段、测评一个片段、取出某一帧 |
| `clip.h` | 片段文件格式 |
| `text_cells.h` | 测评用的文字格判断（见“怎么找文字格”） |
| `compare.sh` | 当前代码和某个旧版本在一组片段上逐个对比 |
| `compare_runs.py` | 同一个 `rdaccel_clip` 在一组片段上按几种设置对比 |
| `scenarios/record.sh` | 在 Mac 上自动操作真实应用，每个场景录一个片段 |
| `scenarios/drive.swift` | `record.sh` 用的键盘鼠标驱动 |
| `scenarios/make_content.sh` | 生成场景要显示的内容（长网页、测试视频和带字幕的版本、终端脚本、四个文字场景的页面；字幕要 Python 的 Pillow） |

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
tools/clip/scenarios/record.sh                  # 13 个场景，约 10 分钟，片段在 clips/
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
| `code` | Safari 里深色编辑器样式的源代码，一次约 18 行地滚、停，再长滚一段、滚回 |
| `smalltext` | 10–11 像素的小字、浅灰字、1 像素表格线，大多静止，滚三次 |
| `translucent` | 模糊的半透明面板上的文字，下面的页面自己滚动（停 4 秒、动 6 秒） |
| `animation` | 静止文字旁边几个 CSS 小动画，不滚动 |
| `subtitles` | 烧进字幕的测试视频，先在窗口里、再全屏播放，中间各暂停一次 |

注意：

- 终端需要“屏幕录制”和“辅助功能”权限（驱动要发键盘鼠标事件）。
- 录制期间**不要有人用这台机器的屏幕**（包括远程连着的人）：别人的操作会录进去，而且驱动每次打字、点击前都检查前台应用是不是它要操作的那个，不是就停下（这一段片段就不是那个场景了，重录）。
- 没有声音：两个测试视频都没有音轨，网页不放声音，脚本不播放任何声音，也不碰音量。
- 结束后把它打开的窗口关掉（文本编辑和 QuickTime 原来没开就不保存退出），输入法恢复成原来的。

## 测评

```sh
build/bin/rdaccel_clip eval clips/drag.clip --text=1
```

把片段送进一对 HostSession / ViewerSession（本机的硬件编解码器，进程内管道），按 Host 在真实屏幕上的做法：屏幕变化时送这一帧；有东西欠着时每 100 ms 再送一次上一帧。快链路、固定码率（`--bitrate`，默认 8 Mbps），不模拟网络。第一行是总的结果：

| 项 | 含义 |
| --- | --- |
| sent / kbps | 发出的总字节和平均码率，其中视频和无损层各多少 |
| exact | 对端显示的画面里和原画面逐像素相同的比例，按时间加权（每个画面算到下一帧为止） |
| PSNR | 同样按时间加权 |
| pauses: exact | 片段里每段 0.5 秒以上没有变化的停顿，结束时逐像素相同的比例的平均 |

下面几行依次是：`text:`、`text by pixels alone:`、`changed text cells until exact or sharp:`、`pictures:`、`rects:`（见下面几节），`--focus=x,y,w,h` 时这块区域（比如视频窗口）里面和外面各自的 PSNR（同样按时间加权），最后是视频区域开了几次、关了几次、区域流共多少字节。

逐像素相同只是一个方面：视频细修后的块看着清楚，但不算在 exact 里。所以另外单看文字（`text:` 开头的三行）。`--csv=FILE` 输出每一帧发了什么，用来找流量花在哪一帧、哪一类块上。列：

| 列 | 含义 |
| --- | --- |
| `t_ms` | 从片段开始的时间 |
| `resent` | 1 = 没有新画面，是有东西欠着时补送的上一帧 |
| `dirty_px` | 系统给的脏矩形共多少像素 |
| `video_b` / `lossless_b` | 这一帧的视频、无损字节 |
| `lossless_rects` | 无损块数 |
| `moved_tiles` / `cached_tiles` | 拷贝、缓存的块数 |
| `owed_tiles` | 补发的块数 |
| `video_frame` | 这一帧编了几幅视频画面 |
| `exact_pct` | 这一帧之后对端画面逐像素相同的比例 |
| `text_exact_pct` `text_sharp_pct` `text_before_pct` `text_soft_pct` `text_wrong_pct` | 这一帧之后文字格五种状态各占文字格的比例（见下） |
| `text_px40_pct` `text_px30_pct` | 每行最后两列（在 `video_rects` 之后，前面的列和顺序不变）：这一帧之后只看像素、和原画面至少 40 dB、30 dB 的文字格的比例（见“只看像素”） |

个别帧在对端没有自己的画面（输出末尾会说有几帧），这时量的是对端当时显示的画面，也就是用户看到的。

### 文字：`text:` 一行

屏幕先按 16 × 16 切成格，哪些格是文字由 `text_cells.h` 判断（见下面“怎么找文字格”）。每个新的片段帧都重新判断。对端每显示一个画面，每个文字格属于下面五种之一：

| 状态 | 含义 |
| --- | --- |
| exact | 每个像素和原画面相同 |
| sharp | 不完全相同，但整格都来自无损层（或无损内容的拷贝，`HostSession::ShownQuality` 为 0），并且和原画面接近（整格 PSNR 至少 25 dB）：文字模式减色后的样子，边缘在原来的位置，相近的颜色合并了 |
| as before the change | 对端还显示着这一格最近一次变化之前的内容 |
| soft | 接近原画面（至少 25 dB），但有一部分来自视频编码器 |
| wrong | 以上都不是：占位内容，或者离原画面太远 |

一行里依次是：文字格占屏幕的比例；五种状态各占文字格的比例，都按时间加权；sharp 格和 soft 格各自的 PSNR（所有这种格的误差合在一起算，不按时间加权）；文字像素逐像素相同的比例。

soft 格再按这一格在原画面上有几种颜色分开：最多 16 种、17–64 种（其余是更多的）。这一项要紧，因为无损层按 64 × 64 的块决定：颜色少的格本来可以无损发，变成 soft 是因为同一个块里别的东西（图标、照片、渐变）颜色太多，整块送进了视频编码器。少颜色的 soft 格多，说明值得把块分得更细，而不是文字本身没法无损。

### 只看像素：`text by pixels alone:` 一行

```
  text by pixels alone: within 40 dB  93.1% (exact  16.2%), within 30 dB  99.2%, others' mean PSNR 33.4
```

同样的文字格，只比对端显示的像素和原画面，不管 Host 以为自己发了什么：整格 PSNR 至少 40 dB 的格占文字格的比例（exact 的格算在内），括号里是其中 exact 的比例（和 `text:` 一行的 exact 是同一个数），至少 30 dB 的比例，以及不是 exact 的格的平均 PSNR（每格最多算 60 dB，免得几乎相同的格把其余的盖住；每个画面里所有不 exact 的格的 PSNR 相加，按格数和时间加权平均；文字格一直都 exact 时是“-”）。比例都按时间加权，和 `text:` 一行一样。低于 25 dB 的已经算在 `text:` 一行的 wrong（或 as before）里，这里不再重复。

为什么要这一行：sharp 靠的是 Host 自己的记账（`HostSession::ShownQuality` 为 0，即整格来自无损层），记账错了，exact + sharp 就会偏高或偏低。以前就有过：缓存槽给整块只记一个质量，和原画面相差不到 45 dB 的格也因此被算成不是 sharp。这一行和记账无关，可以拿来核对。

两个数一起看：

- exact + sharp 高，40 dB 以内低：Host 认为文字是无损的，对端显示的却是别的东西；或者文字模式减色后的颜色离原画面比平常远。
- 40 dB 以内高，exact + sharp 低：记账把文字说得比实际差（比如上面缓存槽的例子）。
- 两个数差不多：记账和像素一致，exact + sharp 可信。

两个数不会完全相等：减色后的 sharp 格合起来的 PSNR 在 45 dB 左右，但其中有一部分单格低于 40 dB；反过来，来自视频的 soft 格有的也在 40 dB 以上。`idle`、`typing` 两个片段在文字模式下，exact + sharp 是 99.5%、97.5%，40 dB 以内是 90.9%、93.7%；按 3.0.1 的规则（`--split-cells=0 --text-refine=1`）是 78.0%、76.4% 和 81.2%、84.7%。要看的是同一个片段在两种设置之间两个数是不是往同一个方向变，而不是两个数的绝对差。

### 文字变化后多久清楚：`changed text cells until exact or sharp:` 一行

文字格的内容一变就开始计时，直到对端第一次显示它为 exact 或 sharp。给出次数、中位数、95% 和最长的时间（ms）。时间在每次送帧后量（片段的帧，或欠着东西时 100 ms 一次的补送），精度就是这个间隔。还给出：没清楚之前又变了的次数（重新计时）和平均变化后多久又变；片段结束（加上最多 3 秒补送）时还没清楚的格数（输出里叫 `not exact at the end`，但数的是既不是 exact 也不是 sharp 的格）。第一帧的文字格也算一次变化。不再是文字的格不计。

### 视频画面花在哪里：`pictures:` 一行

全屏编码器编了几幅画面，其中几幅没有新内容（只是细修或静止细修）。字节按画面编的目的分：

| 项 | 含义 |
| --- | --- |
| keyframes | 关键帧，以及结束渐进关键帧的那一幅 |
| new content | 新内容：对端从这幅画面取的新矩形 |
| refinement | 普通细修：视频块编过以后，对端从后面几幅画面继续取（`HostOptions::refine_frames`） |
| still polish | 屏幕快静止时，以更低的 QP 下限再取几幅（`HostOptions::still_refine_frames`） |
| other | 没有记到任何块的画面 |

一幅画面同时有几种目的时，字节按块数比例分（新内容按矩形数，细修按块数，两者直接相加，是近似）。数据来自 `HostOptions::trace`（`session::FrameTrace`）。

### 为什么送视频：`rects:` 一行

无损发了多少矩形，送进视频编码器的又各是什么原因：

| 项 | 含义 |
| --- | --- |
| too many colours | 颜色太多，做不了调色板 |
| too many for content that keeps changing | 颜色够少能做调色板，但对每帧都变的内容来说太多 |
| over the lossless budget | 能做调色板，但这一帧的无损预算用完了 |
| in motion (saver) | 文字模式：属于运动的部分，颜色对每帧都变的矩形来说太多 |
| video first with a keyframe | 能做调色板，但随渐进关键帧先以视频发出 |

都是逐帧的矩形数累加。

### 实验选项

`--refine=N` 和 `--still-refine=N` 改 `HostOptions::refine_frames`、`still_refine_frames`（默认 8、60），用来看细修和静止细修花多少、换来多少。`refine` 为 0 时不细修，静止细修也一起关掉。文字模式默认不细修（`text_refine`），这两项要和 `--text-refine=1` 一起才对文字模式起作用。

文字模式的几项决定（`HostOptions`，见 [架构](architecture.md) 的“文字模式”）：

| 选项 | 默认 | 改的是 |
| --- | --- | --- |
| `--split-cells=N` | 16 | `split_cells`：整块放不进调色板的块按 16 × 16 的格拆，非文字格最多 N 色才无损；0 = 不拆 |
| `--split-hot=1` | 0 | `split_hot`：上一帧也变了的块也立刻拆 |
| `--text-refine=1` | 0 | `text_refine`：文字模式也照普通模式细修和打磨 |
| `--linger-ms=N` | 100 | `text_linger_us`：文字模式下视频区域不再变化多久后关闭（毫秒；0 = 照普通模式的 250 ms） |

`--split-cells=0 --text-refine=1` 按 3.0.1 的规则决定（视频区域的改动除外，见 [CHANGELOG](../CHANGELOG.md) 3.1.0）。

`--cells=1` 在最后列出片段结束时既不是 exact 也不是 sharp 的文字格，用来查原因：每格一行，左上角坐标、PSNR、对端这一格来自哪里（`lossless`、`polished video`、`video`，即 `HostSession::ShownQuality`）、这一格在原画面上有几种颜色，低于 25 dB 的标 `WRONG`。

### 怎么找文字格

`tools/clip/text_cells.h`，只给测量用，Host 不用。屏幕切成 16 × 16 的格（右边和下边不满的格照样算），每格在两个方向上每隔一个像素取样，算亮度 Y = (77R + 150G + 29B) >> 8。一格是文字，当且仅当：

- 恰好有一个亮度值占至少三分之一的样本（背景），
- 并且至少有两个样本和它相差 16 级或以上（笔画）。

所以：纯色格（背景占满，没有笔画）、两块纯色的分界（两个值都占三分之一以上）、照片和纹理（没有占优的值）、慢渐变（占优的值周围只有相近的亮度）都不算。只看哪个值占优，不看谁更亮，深色、浅色主题和彩色文字一样算。

已知的局限：

- 渐变或半透明面板上的文字没有占优的背景值，算不到。
- 图表和图标的边缘有一部分会算成文字（一边不到三分之一的分界，细线）。
- 只取偶数坐标的像素，落在奇数行或奇数列上的 1 像素细线看不到。
- 只在生成的桌面画面上核对过，没有在真实片段上逐格核对。

`rdaccel_clip frame CLIP --index=N --out=FRAME.ppm --textmap=1` 取出一帧时把不是文字的格调暗到四分之一，文字格保持原色，并打印有多少格是文字，用来肉眼检查判断得对不对。

## 对比两个版本

```sh
tools/clip/compare.sh v1.9.5 clips
```

把 `v1.9.5` 检出到临时目录单独构建（库，加上当前的 `tools/clip`），和当前代码一起把每个片段在普通模式和文字模式各跑一遍，打一张表：两边的字节数和变化、逐像素相同的比例。临时目录用完删掉。完整输出写在片段目录的 `compare-*.txt`。

硬件编码器每次的结果略有不同，同一个片段重跑差 1–2% 以内属正常；看差别要看明显的、方向一致的变化，并且用多个片段。

## 对比几种设置

```sh
tools/clip/compare_runs.py base='--text=1' exp='--text=1 --split-cells=0 --text-refine=1'
tools/clip/compare_runs.py --only idle,typing --markdown base='--text=1' old='--text=1 --split-cells=0 --text-refine=1'
```

和 `compare.sh` 不同，这里只用当前构建的 `rdaccel_clip`，比的是同一份代码在不同选项下的结果。每个 `NAME='ARGS'` 是一种设置，ARGS 原样交给 `eval`（文字模式要自己加 `--text=1`），第一种是参照。片段默认是 `clips/` 里所有的 `*.clip`，`--only` 只跑列出的几个（不带 `.clip`）；`--clips`、`--bin` 改片段目录和程序。一个接一个跑（硬件编码器是共用的）。

每次运行的完整输出存在 `--out`（默认 `clips/baseline/runs/`）的 `<片段>.<设置>.txt`，第一行记着 ARGS；同样的 ARGS、比片段和程序都新的输出直接拿来用，不重跑，`--force` 才重跑。`--list` 只列出会跑什么。

打出的表：流量；文字（exact+sharp 旁边是只看像素 40 dB 以内的比例，旧的输出里没有这一行时显示“-”，也不算缺）；视频画面编的目的和矩形送进视频的原因；最后是比参照差的地方（regressions：文字格 exact 加 sharp 少 0.5 个百分点以上、只看像素 40 dB 以内的少 1.0 个百分点以上、wrong 多 0.05 个百分点以上、变化后到 exact 或 sharp 的 95% 时间多 50 ms 以上、流量多 2% 以上，或者没有结果）。`--markdown` 打 Markdown 表。程序没有输出某一项（旧版本、另一种模式）时显示“-”，stderr 说缺了哪些。

## 合成场景做成片段

```sh
build/bin/rdaccel_clip render clips/syn-scroll.clip --scenario=scroll --background=SCREEN.ppm --seconds=20
```

`examples/scenes.h` 的场景（idle typing scroll drag zoom video pause terminal）按 60 Hz 渲染成片段，和真实片段用同一套测评。

## 例子：1.9.6 是怎么找到的

实际使用中感觉流量变大。9 段真实片段（这台 Mac 上录的）在 1.9.4、1.9.5 上对比，文字模式两版一样，不是升级造成的；但同一组数据显示文字模式拖窗口比普通模式多 69%。`--csv` 显示多出来的都在窗口移动的帧里，再按块的形状统计，发现是窗口阴影留下的几十万个 4 像素宽的小边角各带一个调色板。改完再跑同一组片段：拖窗口 −59%、滚动 −38%，其余场景不变。见 [CHANGELOG](../CHANGELOG.md) 1.9.6。
