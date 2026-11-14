#!/bin/zsh
# Makes the content the macOS scenarios show, into DIR (default: clips/content
# in the repository, which git ignores). Nothing here is committed: a long
# page of text and pictures (the CHANGELOG, with the system's wallpapers), a
# 29 s test video (two wallpapers panned and zoomed, with film grain and a
# crossfade; no sound track) and the terminal script.
#
#   tools/clip/scenarios/make_content.sh [DIR]
#
# Needs ffmpeg for the video (brew install ffmpeg).
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
if command -v ffmpeg >/dev/null; then
  ffmpeg -hide_banner -loglevel error -y -loop 1 -i "$DIR/Sonoma.jpg" -loop 1 -i "$DIR/iMac_Blue.jpg" -filter_complex \
    "[0]scale=2560:-2,zoompan=z='1.0+0.0015*on':x='iw/2-(iw/zoom/2)+200*sin(on/90)':y='ih/2-(ih/zoom/2)':d=450:s=1280x720:fps=30,setsar=1[a];[1]scale=2560:-2,zoompan=z='1.3-0.001*on':x='100*on/30':y='ih/2-(ih/zoom/2)+80*sin(on/60)':d=450:s=1280x720:fps=30,setsar=1[b];[a][b]xfade=transition=fade:duration=1:offset=14,noise=alls=6:allf=t" \
    -t 29 -an -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p "$DIR/video.mp4"
else
  echo "ffmpeg not found: no video.mp4 (the video scenario is skipped)" >&2
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
