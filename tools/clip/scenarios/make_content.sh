#!/bin/zsh
# Makes the content the macOS scenarios show, into DIR (default: clips/content
# in the repository, which git ignores). Nothing here is committed: a long
# page of text and pictures (the CHANGELOG, with the system's wallpapers), a
# 29 s test video (two wallpapers panned and zoomed, with film grain and a
# crossfade; no sound track), the same video with subtitles burned in, the
# terminal script, and four pages for the text scenarios: source code in a
# dark editor (code), small text with thin table rules (smalltext), text on
# blurred translucent panels over a photo (translucent) and small CSS
# animations among still text (animation). The pages are self-contained:
# no network, no web fonts, no sound.
#
#   tools/clip/scenarios/make_content.sh [DIR]
#
# Needs ffmpeg for the videos (brew install ffmpeg), and Python's Pillow for
# the subtitles (pip3 install pillow; without it there is no video_sub.mp4).
set -eu
cd "$(dirname "$0")/../../.."
DIR=${1:-clips/content}
mkdir -p "$DIR"
PICS="/System/Library/Desktop Pictures"
for n in "Sonoma" "iMac Blue" "Radial Sky Blue" "Mac Purple"; do
  sips -s format jpeg -Z 1400 "$PICS/$n.heic" --out "$DIR/${n// /_}.jpg" >/dev/null
done
python3 - "$DIR" CHANGELOG.md <<'PY'
import sys, html, re
d, src = sys.argv[1], sys.argv[2]
lines = [l.rstrip() for l in open(src, encoding='utf-8')][:700]
imgs = ['Sonoma.jpg', 'iMac_Blue.jpg', 'Radial_Sky_Blue.jpg', 'Mac_Purple.jpg']
out = ['<!doctype html><meta charset="utf-8"><title>scroll test</title><style>body{font:15px -apple-system,"PingFang SC",sans-serif;'
       'max-width:860px;margin:24px auto;line-height:1.55;color:#222;background:#fff}h2{border-bottom:1px solid #ddd}'
       'img{width:100%;border-radius:8px;margin:12px 0}code{background:#f3f3f3;padding:1px 4px;border-radius:3px}'
       'table{border-collapse:collapse}td{border:1px solid #ccc;padding:3px 8px}</style>']
k = 0
for i, l in enumerate(lines):
    if not l:
        continue
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', html.escape(l))
    if l.startswith('#'):
        out.append('<h2>' + t.lstrip('#').strip() + '</h2>')
    elif l.startswith('|'):
        out.append('<table><tr>' + ''.join('<td>' + c.strip() + '</td>' for c in t.strip('|').split('|')) + '</tr></table>')
    else:
        out.append('<p>' + t + '</p>')
    if i % 45 == 44:
        out.append('<img src="%s">' % imgs[k % 4])
        k += 1
open(d + '/scroll.html', 'w', encoding='utf-8').write('\n'.join(out))
PY
# The text scenarios' pages. Their text comes from the repository: source
# code, the English comments in core/, the Chinese paragraphs and tables of
# docs/.
python3 - "$DIR" <<'PY'
import sys, html, re, glob
d = sys.argv[1]
esc = html.escape
HEAD = '<!doctype html><meta charset="utf-8"><title>%s</title><style>%s</style>'

def inline(s):  # markdown to html, roughly
    s = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', s).replace('**', '')
    return re.sub(r'`([^`]+)`', r'<code>\1</code>', esc(s))

# English: runs of // comment lines in core/ of at least 120 characters.
english = []
for f in sorted(glob.glob('core/**/*.cpp', recursive=True)):
    run = []
    for l in open(f, encoding='utf-8', errors='replace').read().split('\n') + ['']:
        m = re.match(r'\s*//\s?(.*)', l)
        if m:
            run.append(m.group(1).strip())
            continue
        t = ' '.join(run).strip()
        if len(t) >= 120 and t.isascii():
            english.append(t)
        run = []
# Chinese: plain paragraphs, headings and tables of docs/.
chinese, headings, tables = [], [], []
for f in sorted(glob.glob('docs/*.md')):
    rows, fence = [], False
    for l in open(f, encoding='utf-8').read().split('\n') + ['']:
        l = l.strip()
        if l.startswith('```'):
            fence = not fence
        if fence or l.startswith('```'):
            continue
        if l.startswith('|'):
            if not re.fullmatch(r'[|\s:-]+', l):
                rows.append([inline(c.strip()) for c in l.strip('|').split('|')])
            continue
        if len(rows) > 2:
            tables.append(rows)
        rows = []
        if l.startswith('## '):
            headings.append(inline(l[3:]))
        elif len(l) > 40 and re.search(r'[一-鿿]', l) and l[0] not in '#->*`!':
            chinese.append(inline(l))
if not english or not chinese:
    sys.exit('make_content.sh: no text found in core/ or docs/')
def pick(lst, i):
    return lst[i % len(lst)]

# code: the first 640 lines of a source file, coloured like a dark editor.
src = ''.join(open('core/session/host_session.cpp', encoding='utf-8').readlines()[:640])
KW = set('''alignas auto bool break case catch char class const constexpr const_cast continue decltype default
  delete do double else enum explicit extern false final float for friend if inline int long mutable namespace
  new noexcept nullptr operator override private protected public return short signed sizeof static
  static_assert static_cast reinterpret_cast struct switch template this throw true try typedef typename
  union unsigned using virtual void volatile while'''.split())
TOK = re.compile(r'''(?P<c>//[^\n]*|/\*.*?\*/)|(?P<s>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])+')|(?P<p>^[ \t]*#[^\n]*)'''
                 r'''|(?P<m>\b(?:0[xX][0-9a-fA-F']+|\d[\d']*(?:\.\d+)?(?:[eE][-+]?\d+)?)[uUlLfF]*\b)|(?P<i>[A-Za-z_]\w*)|(?P<o>\s+|.)''',
                 re.S | re.M)
lines, cur = [], []
for m in TOK.finditer(src):
    k, t = m.lastgroup, m.group()
    if k == 'i':
        if t in KW:
            k = 'k'
        elif re.match(r'\s*\(', src[m.end():m.end() + 8]):
            k = 'f'
        elif re.fullmatch(r'u?int\d+_t|size_t|[A-Z]\w*[a-z]\w*', t):
            k = 't'
        else:
            k = 'o'
    for j, part in enumerate(t.split('\n')):
        if j:
            lines.append(''.join(cur))
            cur = []
        if part:
            cur.append(esc(part) if k == 'o' else '<span class=%s>%s</span>' % (k, esc(part)))
lines.append(''.join(cur))
tree = []
for top in sorted(glob.glob('core/*')):
    tree.append('<div class=d>%s</div>' % esc(top.split('/')[-1]))
    if top == 'core/session':
        for f in sorted(glob.glob(top + '/*')):
            name = f.split('/')[-1]
            tree.append('<div class="fi%s">%s</div>' % (' on' if name == 'host_session.cpp' else '', esc(name)))
css = ('html,body{margin:0;background:#1e1f22;color:#bcbec4}'
       '.tabs{position:fixed;top:0;left:0;right:0;height:34px;background:#2b2d30;border-bottom:1px solid #393b40;'
       'font:12px -apple-system,sans-serif;display:flex;z-index:2}.tab{padding:10px 16px;color:#8c8f94}'
       '.tab.on{background:#1e1f22;color:#dfe1e5;box-shadow:inset 0 2px #3574f0}'
       '.tree{position:fixed;top:35px;bottom:24px;left:0;width:230px;background:#2b2d30;border-right:1px solid #393b40;'
       'font:12px/22px -apple-system,sans-serif;padding-top:6px;overflow:hidden}.tree .d{padding-left:14px;color:#ced0d6}'
       '.tree .d:before{content:"\\25B8  ";color:#6f737a}.tree .fi{padding-left:36px;color:#a8adb5}.tree .on{background:#2e436e;color:#fff}'
       '.status{position:fixed;bottom:0;left:0;right:0;height:24px;background:#2b2d30;border-top:1px solid #393b40;'
       'font:11px/24px -apple-system,sans-serif;color:#8c8f94;padding-left:14px;z-index:2}'
       '.code{margin-left:231px;padding:42px 0 40px;font:13px/20px "SF Mono",Menlo,monospace;tab-size:4}'
       '.l{display:flex;white-space:pre}.n{width:48px;padding-right:22px;text-align:right;color:#4b5059;flex:none}'
       '.l.cur{background:#26282e}.l.cur .n{color:#a1a3ab}'
       '.k{color:#cf8e6d}.s{color:#6aab73}.c{color:#7a7e85;font-style:italic}.m{color:#2aacb8}.p{color:#b3ae60}'
       '.f{color:#56a8f5}.t{color:#c77dbb}')
body = ['<div class=tabs><div class="tab on">host_session.cpp</div><div class=tab>host_session.h</div>'
        '<div class=tab>viewer_session.cpp</div><div class=tab>rate_control.cpp</div></div>',
        '<div class=tree>' + ''.join(tree) + '</div>', '<div class=code>']
for i, l in enumerate(lines[:640]):
    body.append('<div class="l%s"><span class=n>%d</span>%s</div>' % (' cur' if i == 23 else '', i + 1, l))
body.append('</div><div class=status>host_session.cpp &nbsp;&nbsp; 24:17 &nbsp;&nbsp; LF &nbsp;&nbsp; UTF-8 &nbsp;&nbsp; C++ &nbsp;&nbsp; 4 spaces</div>')
open(d + '/code.html', 'w', encoding='utf-8').write(HEAD % ('code test', css) + '\n'.join(body))

# smalltext: 10-11 px text in three columns, 9 px footnotes, tables with
# 1 px rules, some light grey text.
css = ('body{margin:0;background:#fff;color:#222;font:10.5px/1.45 -apple-system,"PingFang SC","Helvetica Neue",sans-serif}'
       'header{padding:10px 22px 8px;border-bottom:1px solid #e2e2e2;font-size:11px;color:#555}header b{font-size:13px;color:#111}'
       'section{padding:6px 22px 4px}h3{font-size:12px;margin:10px 0 2px}'
       '.meta{font-size:9px;color:#b4b4b4;margin-bottom:6px}.cols{columns:3 280px;column-gap:26px;column-rule:1px solid #ededed}'
       'p{margin:0 0 6px}p.zh{font-size:11px}p.low{color:#a3a3a3}sup{font-size:7px;color:#888}'
       '.notes{font-size:9px;color:#666;border-top:1px solid #e6e6e6;margin-top:4px;padding-top:3px}.notes p{margin:0 0 2px}'
       'table{border-collapse:collapse;font-size:10px;margin:6px 0 8px}td,th{border:1px solid #cfcfcf;padding:1px 6px}'
       'th{background:#f6f6f6;font-weight:600;text-align:left}td.low{color:#a0a0a0}'
       'code{font:9.5px Menlo,monospace;background:#f4f4f4;padding:0 2px}')
body = ['<header><b>rdaccel</b> &nbsp; notes, measurements and comments &nbsp; '
        '<span style="color:#aaa">· generated from the repository · 10.5 px</span></header>']
e = z = n = 0
for s in range(10):
    body.append('<section><h3>%s</h3><div class=meta>section %d · updated 2026-11-18 · %d paragraphs</div><div class=cols>'
                % (pick(headings, s) if headings else 'Section %d' % (s + 1), s + 1, 10))
    notes = []
    for p in range(10):
        if p % 2:
            t, cls = pick(chinese, z), 'zh'; z += 1
        else:
            t, cls = esc(pick(english, e)), ''; e += 1
        if p % 4 == 3:
            cls += ' low'
        if p % 3 == 2:
            n += 1
            t += '<sup>%d</sup>' % n
            notes.append('<p><sup>%d</sup> %s</p>' % (n, (pick(chinese, z + 7) if n % 2 else esc(pick(english, e + 5)))[:160]))
        body.append('<p class="%s">%s</p>' % (cls.strip(), t))
    body.append('</div><div class=notes>' + ''.join(notes) + '</div>')
    if tables:
        rows = pick(tables, s)
        body.append('<table><tr>' + ''.join('<th>%s</th>' % c for c in rows[0]) + '</tr>' +
                    ''.join('<tr>' + ''.join('<td%s>%s</td>' % (' class=low' if j == len(r) - 1 else '', c)
                                             for j, c in enumerate(r)) + '</tr>' for r in rows[1:]) + '</table>')
    body.append('</section>')
open(d + '/smalltext.html', 'w', encoding='utf-8').write(HEAD % ('small text test', css) + '\n'.join(body))

# translucent: a fixed photo behind blurred panels (toolbar, sidebar, a
# notification), and content that scrolls by itself under the toolbar: 4 s
# still, 6 s moving, and so on, at 40-70 px/s.
css = ('html{background:#000 url(Sonoma.jpg) center/cover fixed}'
       'body{margin:0;font:14px/1.5 -apple-system,"PingFang SC",sans-serif;color:#fff}'
       '.g{-webkit-backdrop-filter:blur(24px) saturate(170%);backdrop-filter:blur(24px) saturate(170%)}'
       '.bar{position:fixed;top:0;left:0;right:0;height:52px;background:rgba(255,255,255,.3);border-bottom:1px solid rgba(255,255,255,.4);'
       'display:flex;align-items:center;gap:10px;padding:0 18px;color:#1d1d1f;z-index:3;font-size:13px}'
       '.bar b{font-size:15px;margin-right:12px}.bar span{padding:4px 11px;border-radius:7px;background:rgba(255,255,255,.35)}'
       '.bar .q{margin-left:auto;width:230px;background:rgba(0,0,0,.1);color:#55555a}'
       '.side{position:fixed;top:53px;bottom:0;left:0;width:220px;background:rgba(28,28,32,.36);padding:12px 10px;z-index:2;'
       'border-right:1px solid rgba(255,255,255,.18)}.side h4{font-size:11px;color:rgba(255,255,255,.62);margin:12px 8px 4px;font-weight:600}'
       '.side div{padding:4px 8px;border-radius:6px;font-size:13px}.side .on{background:rgba(255,255,255,.22)}'
       '.side i{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:8px}'
       '.note{position:fixed;top:66px;right:18px;width:310px;border-radius:14px;background:rgba(246,246,246,.6);color:#1d1d1f;'
       'padding:11px 14px;box-shadow:0 8px 30px rgba(0,0,0,.25);z-index:3;font-size:13px;line-height:1.4}'
       '.note small{float:right;color:#6e6e73}.note p{margin:3px 0 0;color:#3a3a3c}'
       'main{margin-left:220px;padding:90px 40px 600px 44px}'
       'h1{font-size:44px;margin:30px 0 6px;text-shadow:0 2px 14px rgba(0,0,0,.45)}'
       '.on-photo{font-size:16px;max-width:620px;text-shadow:0 1px 6px rgba(0,0,0,.7)}'
       '.card{border-radius:16px;background:rgba(255,255,255,.2);border:1px solid rgba(255,255,255,.32);padding:14px 20px;'
       'margin:24px 0;max-width:640px}.card.dark{background:rgba(18,18,22,.42)}.card h3{margin:0 0 4px;font-size:17px}'
       'img{display:block;width:640px;border-radius:14px;margin:24px 0;box-shadow:0 10px 40px rgba(0,0,0,.35)}')
dots = ['#ff5f57', '#febc2e', '#28c840', '#2f7cf6', '#a259ff', '#ff6ac1']
side = ['<div class="side g"><h4>FAVOURITES</h4>']
for i, (t, on) in enumerate([('Recents', 0), ('Remote desktops', 1), ('Clips', 0), ('Downloads', 0), ('Documents', 0)]):
    side.append('<div%s><i style="background:%s"></i>%s</div>' % (' class=on' if on else '', dots[i], t))
side.append('<h4>TAGS</h4>' + ''.join('<div><i style="background:%s"></i>%s</div>' % (c, t)
            for c, t in zip(dots, ['红色 · 待办', 'Orange', '绿色 · 完成', 'Blue', '紫色', 'Pink'])) + '</div>')
main = ['<main><h1>Text over glass</h1><p class=on-photo>%s</p>' % esc(english[0])]
imgs = ['iMac_Blue.jpg', 'Radial_Sky_Blue.jpg', 'Mac_Purple.jpg']
for i in range(12):
    main.append('<div class="card g%s"><h3>%s</h3>%s</div>' % (' dark' if i % 2 else '', pick(headings, i) if headings else 'Card',
                pick(chinese, i) if i % 3 == 1 else esc(pick(english, i + 1))))
    main.append('<p class=on-photo>%s</p>' % (pick(chinese, i + 20) if i % 2 else esc(pick(english, i + 20))))
    if i % 3 == 2:
        main.append('<img src="%s">' % imgs[(i // 3) % 3])
main.append('</main>')
js = ('<script>const P=[[4000,0],[6000,50],[4000,0],[6000,70],[4000,0],[6000,-60],[4000,0],[6000,40]];'
      'let i=0,t0=0,last=0,pos=0;function step(now){if(now-t0>=P[i][0]){i=(i+1)%P.length;t0=now}'
      'const max=document.documentElement.scrollHeight-innerHeight;pos=Math.min(max,Math.max(0,pos+P[i][1]*(now-last)/1000));'
      'last=now;scrollTo(0,Math.round(pos));requestAnimationFrame(step)}'
      'addEventListener("load",()=>{pos=scrollY;t0=last=performance.now();requestAnimationFrame(step)})</script>')
open(d + '/translucent.html', 'w', encoding='utf-8').write(HEAD % ('translucent test', css) +
    '<div class="bar g"><b>Glass</b><span>Back</span><span>Share</span><span>View</span><span class=q>搜索 Search</span></div>' +
    ''.join(side) + '<div class="note g"><b>RDMachine</b><small>now</small><p>Connected to the Mac mini · 已连接，文字清晰模式</p></div>' +
    '\n'.join(main) + js)

# animation: still paragraphs with small CSS animations among them (a
# spinner, a progress bar, a caret, a badge, a typing indicator, a
# colour-cycling icon). Fits in the window: nothing scrolls.
css = ('html,body{margin:0;background:#fff;overflow:hidden}'
       'body{font:15px/1.55 -apple-system,"PingFang SC",sans-serif;color:#1d1d1f}main{max-width:900px;margin:22px auto;padding:0 20px}'
       'h2{font-size:20px;margin:0 0 10px;display:flex;align-items:center;gap:10px}p{margin:0 0 12px}.row{display:flex;align-items:center;gap:14px;margin:0 0 14px}'
       '.spin{display:inline-block;box-sizing:border-box;width:24px;height:24px;border:3px solid #dde3ea;border-top-color:#2f7cf6;'
       'border-radius:50%;animation:spin .8s linear infinite;vertical-align:middle}@keyframes spin{to{transform:rotate(360deg)}}'
       '.prog{position:relative;width:320px;height:4px;background:#e4e8ee;border-radius:2px;overflow:hidden}'
       '.prog:before{content:"";position:absolute;top:0;left:0;height:100%;width:35%;background:#2f7cf6;border-radius:2px;'
       'animation:prog 1.6s ease-in-out infinite}@keyframes prog{from{transform:translateX(-100%)}to{transform:translateX(300%)}}'
       '.field{display:inline-block;width:320px;border:1px solid #c9ced6;border-radius:6px;padding:5px 10px;font-size:14px;background:#fff}'
       '.caret{display:inline-block;width:1px;height:17px;background:#111;vertical-align:text-bottom;animation:blink 1.06s steps(1) infinite}'
       '@keyframes blink{50%{opacity:0}}'
       '.badge{display:inline-block;min-width:18px;height:18px;border-radius:9px;background:#ff3b30;color:#fff;font:600 11px/18px -apple-system,sans-serif;'
       'text-align:center;animation:pulse 1.4s ease-in-out infinite}'
       '@keyframes pulse{0%,100%{transform:scale(1);box-shadow:0 0 0 0 rgba(255,59,48,.5)}50%{transform:scale(1.15);box-shadow:0 0 0 6px rgba(255,59,48,0)}}'
       '.typing{display:inline-flex;gap:4px;padding:9px 12px;background:#e9e9eb;border-radius:14px}'
       '.typing i{width:7px;height:7px;border-radius:50%;background:#8e8e93;animation:dot 1.2s ease-in-out infinite}'
       '.typing i:nth-child(2){animation-delay:.2s}.typing i:nth-child(3){animation-delay:.4s}'
       '@keyframes dot{0%,60%,100%{transform:translateY(0);opacity:.4}30%{transform:translateY(-4px);opacity:1}}'
       '.sprite{position:relative;display:inline-block;width:32px;height:32px;border-radius:7px;'
       'background:linear-gradient(90deg,#ff5f57 0 16.67%,#febc2e 0 33.33%,#28c840 0 50%,#2f7cf6 0 66.67%,#a259ff 0 83.33%,#ff6ac1 0);'
       'background-size:600% 100%;animation:sprite .9s steps(6) infinite}'
       '.sprite:after{content:"";position:absolute;left:10px;top:10px;width:12px;height:12px;border-radius:3px;background:#fff}'
       '@keyframes sprite{from{background-position:0 0}to{background-position:120% 0}}'
       '.muted{color:#6e6e73;font-size:13px}code{font:13px Menlo,monospace;background:#f3f3f3;padding:1px 4px;border-radius:3px}')
body = ['<main><h2>Session status <span class=spin></span></h2>',
        '<p>%s</p>' % esc(english[1]),
        '<div class=row><span class=muted>Uploading clip</span><div class=prog></div><span class=muted>about a minute left</span></div>',
        '<p>%s</p>' % chinese[1],
        '<div class=row><span class=field>Search the clips<span class=caret></span></span><span class=muted>Messages</span>'
        '<span class=badge>3</span></div>',
        '<p>%s</p>' % esc(english[2]),
        '<div class=row><span class=typing><i></i><i></i><i></i></span><span class=muted>对方正在输入…</span>'
        '<span class=sprite></span><span class=muted>Recording</span></div>',
        '<p>%s</p>' % chinese[2],
        '<p class=muted>%s</p></main>' % esc(english[3])]
open(d + '/animation.html', 'w', encoding='utf-8').write(HEAD % ('animation test', css) + '\n'.join(body))
PY
if command -v ffmpeg >/dev/null; then
  # The picture of both videos: two wallpapers panned and zoomed, a
  # crossfade, film grain.
  base="[0]scale=2560:-2,zoompan=z='1.0+0.0015*on':x='iw/2-(iw/zoom/2)+200*sin(on/90)':y='ih/2-(ih/zoom/2)':d=450:s=1280x720:fps=30,setsar=1[a];[1]scale=2560:-2,zoompan=z='1.3-0.001*on':x='100*on/30':y='ih/2-(ih/zoom/2)+80*sin(on/60)':d=450:s=1280x720:fps=30,setsar=1[b];[a][b]xfade=transition=fade:duration=1:offset=14,noise=alls=6:allf=t"
  enc=(-t 29 -an -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p)
  # Subtitles: ffmpeg here has no drawtext, so Pillow draws each line into a
  # PNG and ffmpeg lays it over the picture for its 2.0-2.2 s.
  rm -rf "$DIR/subs"
  mkdir -p "$DIR/subs"
  if python3 - "$DIR/subs" <<'PY'
import sys, glob
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit('Pillow not found (pip3 install pillow): no video_sub.mp4')
d = sys.argv[1]
def font():
    # PingFang SC Semibold (a downloaded system font, in a folder whose name
    # changes), else Hiragino Sans GB W6 or STHeiti.
    for f in sorted(glob.glob('/System/Library/AssetsV2/com_apple_MobileAsset_Font*/*.asset/AssetData/PingFang.ttc')):
        for i in range(40):
            try:
                ft = ImageFont.truetype(f, 40, index=i)
            except OSError:
                break
            if ft.getname() == ('PingFang SC', 'Semibold'):
                return ft
    for f, i in [('/System/Library/Fonts/Hiragino Sans GB.ttc', 2), ('/System/Library/Fonts/STHeiti Medium.ttc', 0)]:
        try:
            return ImageFont.truetype(f, 40, index=i)
        except OSError:
            pass
    sys.exit('no Chinese font found: no video_sub.mp4')
ft = font()
lines = ['Are you sure the connection is still up?', '我这边看得很清楚，就是有一点延迟。',
         'Try scrolling the page again, slowly.', '字幕要一直清晰，不能糊成一片。',
         'The picture behind us keeps moving,\nbut the words should stay sharp.', '等一下，我把窗口挪到另一边。',
         'Okay. Now pause it for a moment.', '暂停的时候，\n画面应该马上变清楚。',
         'It looks fine from here. Play it again.', '最后一段，我们切到全屏看看。',
         'Full screen: the same text, bigger.', '好的，测试结束，谢谢大家。']
chain, t, prev = [], 0.4, 's0'
for i, text in enumerate(lines):
    box = ImageDraw.Draw(Image.new('RGBA', (1, 1))).multiline_textbbox((0, 0), text, font=ft, spacing=8, align='center', stroke_width=3)
    box = [int(v) for v in box]
    im = Image.new('RGBA', (box[2] - box[0] + 16, box[3] - box[1] + 16), (0, 0, 0, 0))
    ImageDraw.Draw(im).multiline_text((8 - box[0], 8 - box[1]), text, font=ft, fill=(255, 255, 255, 255), spacing=8,
                                      align='center', stroke_width=3, stroke_fill=(0, 0, 0, 235))
    im.save('%s/%02d.png' % (d, i))
    dur = 2.2 if i % 2 == 0 else 2.0
    out = 'sub' if i == len(lines) - 1 else 's%d' % (i + 1)
    chain.append("[%s][%d]overlay=x=(W-w)/2:y=H-h-46:enable='between(t,%.1f,%.1f)'[%s]" % (prev, i + 2, t, t + dur, out))
    prev, t = out, t + dur + 0.2
open(d + '/overlay.txt', 'w').write(';'.join(chain))
PY
  then
    pngs=("$DIR"/subs/*.png)
    inputs=()
    for p in $pngs; do inputs+=(-i "$p"); done
    ffmpeg -hide_banner -loglevel error -y -loop 1 -i "$DIR/Sonoma.jpg" -loop 1 -i "$DIR/iMac_Blue.jpg" $inputs \
      -filter_complex "$base,split=2[v][s0];$(<"$DIR/subs/overlay.txt")" \
      -map "[v]" $enc "$DIR/video.mp4" -map "[sub]" $enc "$DIR/video_sub.mp4"
  else
    rm -rf "$DIR/video_sub.mp4" "$DIR/subs"
    ffmpeg -hide_banner -loglevel error -y -loop 1 -i "$DIR/Sonoma.jpg" -loop 1 -i "$DIR/iMac_Blue.jpg" -filter_complex "$base" \
      $enc "$DIR/video.mp4"
  fi
else
  echo "ffmpeg not found: no video.mp4 or video_sub.mp4 (the video and subtitles scenarios are skipped)" >&2
fi
cat > "$DIR/terminal.command" <<'SH'
#!/bin/zsh
# Output in bursts of 6 s, then 4 s still, four times.
printf '\e[8;45;150t'
for round in 1 2 3 4; do
  end=$((SECONDS + 6))
  while [ $SECONDS -lt $end ]; do ls -la /usr/bin | head -40; done
  echo "--- pause $round ---"
  sleep 4
done
echo "terminal scenario done"
sleep 2
exit 0
SH
chmod +x "$DIR/terminal.command"
ls "$DIR"
