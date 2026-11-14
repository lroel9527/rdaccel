#!/bin/zsh
# Records one test clip per scenario on this Mac, by driving real apps
# (Terminal, TextEdit, Safari, QuickTime Player) with drive.swift while
# `rdaccel_clip record` captures the screen. See docs/clips.md.
#
#   tools/clip/scenarios/record.sh [OUT_DIR] [scenario ...]
#   scenarios: terminal typing ime idle scroll drag video switching (default: all)
#
# OUT_DIR defaults to clips/ in the repository, which git ignores: the clips
# show this screen, whatever is on it, and stay on this machine.
#
# Before running:
#   * cmake --build build --target rdaccel_clip
#   * tools/clip/scenarios/make_content.sh  (into OUT_DIR/content)
#   * the terminal needs the Screen Recording and Accessibility permissions
#   * nobody may use the screen meanwhile (about 6 minutes): input from
#     anyone else lands in the clips, and the driver stops when the app it
#     types into is no longer in front.
# Silent: the test video has no sound track, nothing else plays anything.
# Afterwards the windows it opened are closed (TextEdit and QuickTime Player
# quit without saving if they were not running before) and the input source
# is put back.
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${1:-$REPO/clips}
[[ $# -gt 0 ]] && shift
CONTENT=$OUT/content
CLIP=$REPO/build/bin/rdaccel_clip
DRIVE=$OUT/drive
mkdir -p "$OUT"
[[ -x $CLIP ]] || { echo "build rdaccel_clip first: cmake --build build --target rdaccel_clip" >&2; exit 1; }
[[ -f $CONTENT/scroll.html ]] || { echo "make the content first: tools/clip/scenarios/make_content.sh $CONTENT" >&2; exit 1; }
if [[ ! -x $DRIVE || $REPO/tools/clip/scenarios/drive.swift -nt $DRIVE ]]; then
  swiftc -O "$REPO/tools/clip/scenarios/drive.swift" -o "$DRIVE" || exit 1
fi

running() { osascript -e "application id \"$1\" is running" 2>/dev/null; }
IME_BEFORE=$(print "ime-get" | "$DRIVE")
TEXTEDIT_BEFORE=$(running com.apple.TextEdit)
QUICKTIME_BEFORE=$(running com.apple.QuickTimePlayerX)
FRONT_BEFORE=$(osascript -e 'tell application "System Events" to get bundle identifier of first process whose frontmost is true' 2>/dev/null)

win() {  # process x y w h : place the app's front window
  osascript -e "tell application \"System Events\" to tell process \"$1\" to set position of front window to {$2, $3}" \
            -e "tell application \"System Events\" to tell process \"$1\" to set size of front window to {$4, $5}" >/dev/null 2>&1
}
record() {  # name seconds driver-script
  echo "== $1 (${2}s)"
  "$CLIP" record "$OUT/$1.clip" --seconds=$2 > "$OUT/$1.log" 2>&1 &
  local rec=$!
  sleep 0.5
  "$DRIVE" "$3" || echo "   driver stopped (exit $?): this clip is not the scenario, record it again"
  wait $rec
  tail -1 "$OUT/$1.log"
}
D=$OUT/scripts
mkdir -p "$D"

terminal() {
  open -a Terminal "$CONTENT/terminal.command"
  sleep 2
  osascript -e 'tell application "Terminal" to activate' >/dev/null
  win Terminal 40 40 1200 780
  sleep 1
  print "sleep 40000" > "$D/terminal.txt"
  record terminal 40 "$D/terminal.txt"
  osascript -e 'tell application "Terminal" to close (every window whose name contains "terminal.command")' >/dev/null 2>&1
}

textedit_open() {
  osascript -e 'tell application "TextEdit" to make new document' -e 'tell application "TextEdit" to activate' >/dev/null
  sleep 1.5
  win TextEdit 120 60 1000 720
  sleep 1
}

typing() {
  textedit_open
  cat > "$D/typing.txt" <<'S'
ime com.apple.keylayout.ABC
activate com.apple.TextEdit
front com.apple.TextEdit
type 7 // Remote desktop typing test\n
sleep 800
type 7 int main(int argc, char** argv) {\n
type 7   const int frames = std::atoi(argv[1]);\n
sleep 1500
type 7   for (int i = 0; i < frames; ++i) {\n
type 7     render(scene, i, image);\n
type 7     if (i > 0 && image == last) continue;\n
sleep 2000
type 7     last = image;\n
type 7   }\n
type 7   return 0;\n
type 7 }\n
sleep 1500
type 7 The quick brown fox jumps over the lazy dog. 0123456789\n
sleep 3000
S
  record typing 40 "$D/typing.txt"
}

ime() {
  # Pinyin, with its candidate window (Simplified Chinese - Pinyin).
  cat > "$D/ime.txt" <<'S'
activate com.apple.TextEdit
front com.apple.TextEdit
ime com.apple.inputmethod.SCIM.ITABC
sleep 1000
type 5 yuancheng
key space
type 5 zhuomian
key space
sleep 400
type 5 ceshi
key space
sleep 1200
type 5 shurufa
key space
type 5 houxuan
key space
type 5 chuangkou
key space
sleep 1500
type 5 nihao
key space
type 5 shijie
key space
sleep 1000
key return
type 5 jintian
key space
type 5 tianqi
key space
type 5 henhao
key space
sleep 1500
type 5 women
key space
type 5 ceshi
key space
type 5 liuliang
key space
sleep 1200
type 5 wenzi
key space
type 5 qingxidu
key space
sleep 3000
ime com.apple.keylayout.ABC
S
  record ime 40 "$D/ime.txt"
}

idle() {
  print "activate com.apple.TextEdit\nsleep 40000" > "$D/idle.txt"
  record idle 40 "$D/idle.txt"
}

scroll() {
  osascript -e "tell application \"Safari\" to make new document with properties {URL:\"file://$CONTENT/scroll.html\"}" \
            -e 'tell application "Safari" to activate' >/dev/null
  sleep 3
  win Safari 30 30 1250 830
  sleep 1.5
  {
    echo "activate com.apple.Safari"; echo "front com.apple.Safari"; echo "move 650 450"
    for i in 1 2 3 4 5 6; do echo "scroll -12 180 16"; echo "sleep 2000"; done
    echo "scroll -40 60 16"; echo "sleep 2500"
    echo "scroll 30 120 16"; echo "sleep 3000"
  } > "$D/scroll.txt"
  record scroll 42 "$D/scroll.txt"
  osascript -e 'tell application "Safari" to close (every window whose name is "scroll test")' >/dev/null 2>&1
}

drag() {
  # A TextEdit window by its title bar (Safari's is all tabs and buttons).
  [[ $(osascript -e 'tell application "TextEdit" to count documents' 2>/dev/null) -gt 0 ]] || textedit_open
  osascript -e 'tell application "TextEdit" to activate' >/dev/null
  win TextEdit 200 120 900 600
  sleep 1.5
  {
    echo "activate com.apple.TextEdit"; echo "front com.apple.TextEdit"
    for i in 1 2 3; do
      echo "drag 3000 650 133 450 200 300 350 500 300 650 133"; echo "sleep 2000"
      echo "drag 2000 650 133 800 250 650 133"; echo "sleep 2500"
    done
  } > "$D/drag.txt"
  record drag 35 "$D/drag.txt"
}

video() {
  [[ -f $CONTENT/video.mp4 ]] || { echo "== video: no $CONTENT/video.mp4 (ffmpeg), skipped"; return; }
  osascript -e "tell application \"QuickTime Player\" to open POSIX file \"$CONTENT/video.mp4\"" \
            -e 'tell application "QuickTime Player" to activate' >/dev/null
  sleep 2.5
  win "QuickTime Player" 60 60 1280 760
  sleep 1
  osascript -e 'tell application "QuickTime Player" to play document 1' >/dev/null
  echo "== video (36s)"
  "$CLIP" record "$OUT/video.clip" --seconds=36 > "$OUT/video.log" 2>&1 &
  local rec=$!
  sleep 12; osascript -e 'tell application "QuickTime Player" to pause document 1' >/dev/null
  sleep 4;  osascript -e 'tell application "QuickTime Player" to play document 1' >/dev/null
  sleep 12; osascript -e 'tell application "QuickTime Player" to pause document 1' >/dev/null
  wait $rec
  tail -1 "$OUT/video.log"
  osascript -e 'tell application "QuickTime Player" to close every document saving no' >/dev/null 2>&1
}

switching() {
  [[ $(osascript -e 'tell application "TextEdit" to count documents' 2>/dev/null) -gt 0 ]] || textedit_open
  osascript -e "tell application \"Safari\" to make new document with properties {URL:\"file://$CONTENT/scroll.html\"}" >/dev/null
  sleep 2
  win Safari 30 30 1250 830
  cat > "$D/switching.txt" <<'S'
activate com.apple.Safari
sleep 2500
activate com.apple.TextEdit
sleep 2500
activate com.apple.Safari
sleep 2500
activate com.apple.TextEdit
sleep 2500
activate com.apple.Safari
front com.apple.Safari
key ctrl+cmd+f
sleep 5000
key ctrl+cmd+f
sleep 4000
activate com.apple.TextEdit
sleep 2500
activate com.apple.Safari
sleep 4000
S
  record switching 35 "$D/switching.txt"
  osascript -e 'tell application "Safari" to close (every window whose name is "scroll test")' >/dev/null 2>&1
}

cleanup() {
  [[ $TEXTEDIT_BEFORE == true ]] || osascript -e 'tell application "TextEdit" to quit saving no' >/dev/null 2>&1
  [[ $QUICKTIME_BEFORE == true ]] || osascript -e 'tell application "QuickTime Player" to quit' >/dev/null 2>&1
  osascript -e 'tell application "Safari" to close (every window whose name is "scroll test")' >/dev/null 2>&1
  osascript -e 'tell application "Terminal" to close (every window whose name contains "terminal.command")' >/dev/null 2>&1
  [[ -n $IME_BEFORE ]] && print "ime $IME_BEFORE" | "$DRIVE"
  [[ -n $FRONT_BEFORE ]] && osascript -e "tell application id \"$FRONT_BEFORE\" to activate" >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

for s in ${@:-terminal typing ime idle scroll drag video switching}; do $s; done
