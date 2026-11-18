#!/usr/bin/env python3
"""Runs `rdaccel_clip eval` on a set of clips with several configurations and
prints comparison tables (docs/clips.md, "测评").

    tools/clip/compare_runs.py [--clips DIR] [--bin PATH] [--only a,b,c] [--out DIR]
                               [--force] [--list] [--markdown] NAME='ARGS' [NAME='ARGS' ...]

    e.g. tools/clip/compare_runs.py base='--text=1' \\
             exp='--text=1 --split-cells=16 --split-hot=0 --refine=0 --still-refine=0'

Each NAME='ARGS' is one configuration: ARGS are passed to `eval` as they are
(add --text=1 for the text mode); --label is added unless ARGS has one. The
first configuration is the reference the others are compared with.

Clips: every *.clip in --clips (default: clips/ in the repo), sorted by name,
or only those named by --only (names without .clip). Runs are sequential:
the hardware encoder is shared. The full output of each run is kept in --out
(default clips/baseline/runs/) as <clip>.<name>.txt; its first line records
the ARGS. An existing file is reused instead of running again when it was made
with the same ARGS and is newer than both the clip and the binary, unless
--force is given.

The tables: traffic; text; what the video pictures were coded for and why
rects went to video; and the regressions against the reference. A value eval
did not print (an older binary, another mode) shows as "-", and stderr says
which were missing.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NUM = r"(-?[\d.]+)"

# Every value eval prints, as (key, line, pattern). `line` picks the line by
# the label it starts with ("" = the first line, which starts with the clip's
# label and has "| sent" in it); `pattern` finds the value by the words around
# it, so the order of fields and the spacing do not matter.
FIELDS = [
    ("seconds", "", NUM + r" s\b"),
    ("frames", "", r"(\d+) frames"),
    ("resent", "", r"\+\s*(\d+) re-sent"),
    ("total_kb", "", r"sent\s+" + NUM + r" kB"),
    ("kbps", "", NUM + r" kbps"),
    ("video_kb", "", r"\(video\s+" + NUM),
    ("lossless_kb", "", r"lossless\s+" + NUM + r" kB\)"),
    ("exact", "", r"\| exact\s+" + NUM + "%"),
    ("psnr", "", r"PSNR\s+" + NUM),
    ("pauses", "", r"(\d+) pauses"),
    ("pauses_exact", "", r"pauses: exact\s+" + NUM + "%"),
    ("text_share", "text:", r"text:\s+" + NUM + r"% of the screen"),
    ("t_exact", "text:", r"by time: exact\s+" + NUM + "%"),
    ("t_sharp", "text:", r"sharp\s+" + NUM + "%"),
    ("t_sharp_psnr", "text:", r"sharp\s+[\d.]+% \(PSNR " + NUM),
    ("t_before", "text:", r"as before the change\s+" + NUM + "%"),
    ("t_soft", "text:", r"soft\s+" + NUM + "%"),
    ("t_soft_psnr", "text:", r"soft\s+[\d.]+% \(PSNR " + NUM),
    ("t_soft16", "text:", r"up to 16 colours\s+" + NUM + "%"),
    ("t_soft64", "text:", r"up to 64\s+" + NUM + "%"),
    ("t_wrong", "text:", r"wrong\s+" + NUM + "%"),
    ("t_pixels", "text:", r"pixels exact\s+" + NUM + "%"),
    ("changes", "changed text cells", r"exact or sharp:\s+(\d+)"),
    ("median_ms", "changed text cells", r"median\s+" + NUM + " ms"),
    ("p95_ms", "changed text cells", r"95%\s+" + NUM + " ms"),
    ("longest_ms", "changed text cells", r"longest\s+" + NUM + " ms"),
    ("changed_again", "changed text cells", r"changed again first:\s+(\d+)"),
    ("changed_again_ms", "changed text cells", r"\(after\s+" + NUM + " ms"),
    ("not_exact_end", "changed text cells", r"not exact at the end:\s+(\d+)"),
    ("pictures", "pictures:", r"pictures:\s+(\d+)"),
    ("pictures_no_new", "pictures:", r"\((\d+) with nothing new\)"),
    ("key_kb", "pictures:", r"keyframes\s+" + NUM),
    ("new_kb", "pictures:", r"new content\s+" + NUM),
    ("refine_kb", "pictures:", r"refinement\s+" + NUM),
    ("polish_kb", "pictures:", r"still polish\s+" + NUM),
    ("other_kb", "pictures:", r"other\s+" + NUM),
    ("r_lossless", "rects:", r"rects:\s+(\d+) lossless"),
    ("r_colours", "rects:", r"(\d+) too many colours"),
    ("r_changing", "rects:", r"(\d+) too many for content that keeps changing"),
    ("r_budget", "rects:", r"(\d+) over the lossless budget"),
    ("r_motion", "rects:", r"(\d+) in motion"),
    ("r_keyframe", "rects:", r"(\d+) video first with a keyframe"),
    ("areas_opened", "video areas:", r"(\d+) opened"),
    ("areas_closed", "video areas:", r"(\d+) closed"),
    ("areas_kb", "video areas:", NUM + r" kB in their streams"),
]
# Printed only in some cases (--focus, frames the viewer skipped): not
# reported as missing when absent.
OPTIONAL = [
    ("focus_in", "PSNR inside the focus", r"focus\s+" + NUM),
    ("focus_out", "PSNR inside the focus", r"outside\s+" + NUM),
    ("missed", "(", r"\((\d+) frames without a picture"),
]


def parse(text):
    """The values in one run's output: {key: (string as printed, float)}."""
    lines = [l for l in text.splitlines() if l.strip() and not l.startswith("#")]
    first = next((l for l in lines if not l[0].isspace() and "| sent" in l), None)
    out = {}
    for key, start, pat in FIELDS + OPTIONAL:
        if start == "":
            line = first
        else:
            line = next((l for l in lines if l.lstrip().startswith(start)), None)
        m = re.search(pat, line) if line else None
        if m:
            out[key] = (m.group(1), float(m.group(1)))
    return out


def header_for(args):
    return "# rdaccel_clip eval ARGS: " + args


def usable(path, args, clip, binary):
    """Whether an earlier run's output can be reused."""
    try:
        st = path.stat()
        with path.open(encoding="utf-8", errors="replace") as f:
            first = f.readline().rstrip("\n")
            rest = f.read()
    except OSError:
        return False
    if first != header_for(args) or "# exit status" in rest:
        return False
    return st.st_mtime > clip.stat().st_mtime and st.st_mtime > binary.stat().st_mtime


def run(binary, clip, name, args, path):
    """Runs eval once and writes its output (stdout and stderr) to `path`."""
    argv = shlex.split(args)
    if not any(a.startswith("--label") for a in argv):
        argv.append("--label=%s-%s" % (clip.stem, name))
    cmd = [str(binary), "eval", str(clip)] + argv
    t0 = time.monotonic()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    body = header_for(args) + "\n" + p.stdout
    if p.returncode != 0:
        body += "# exit status %d\n" % p.returncode
    tmp = path.with_suffix(".tmp")  # written whole, then renamed: no half files after Ctrl-C
    tmp.write_text(body, encoding="utf-8")
    os.replace(tmp, path)
    return p.returncode, time.monotonic() - t0, body


# --- tables -----------------------------------------------------------------

def fmt(v, digits=None):
    """A value for a table: as eval printed it, or with `digits` decimals."""
    if v is None:
        return "-"
    if isinstance(v, tuple):
        return v[0] if digits is None else "%.*f" % (digits, v[1])
    return "%.*f" % (digits if digits is not None else 1, v)


def table(title, head, rows, markdown):
    """Prints rows of strings; the first two columns (clip, configuration)
    are aligned left, the rest right."""
    print()
    if markdown:
        print("### " + title)
        print()
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join(" --- " if i < 2 else " ---: " for i in range(len(head))) + "|")
        for r in rows:
            print("| " + " | ".join(r) + " |")
        return
    print(title)
    widths = [max(len(h), *(len(r[i]) for r in rows)) if rows else len(h) for i, h in enumerate(head)]
    def line(cells):
        return "  ".join(c.ljust(w) if i < 2 else c.rjust(w) for i, (c, w) in enumerate(zip(cells, widths))).rstrip()
    print(line(head))
    print(line(["-" * w for w in widths]))
    prev = None
    for r in rows:
        # The clip's name only on its first row, so each block stands out.
        cells = list(r)
        if cells[0] == prev:
            cells[0] = ""
        else:
            prev = cells[0]
        print(line(cells))


def get(v, key):
    x = v.get(key)
    return x[1] if x else None


def exact_sharp(v):
    e, s = get(v, "t_exact"), get(v, "t_sharp")
    return None if e is None or s is None else e + s


def change_pct(new, ref):
    if new is None or ref is None:
        return None
    if ref == 0:
        return 0.0 if new == 0 else None
    return 100.0 * (new - ref) / ref


def regressions(results, clips, configs):
    """(clip, configuration, what) for each value worse than the reference."""
    ref_name = configs[0][0]
    found = []
    for clip in clips:
        ref = results.get((clip.stem, ref_name))
        if ref is None:
            continue
        for name, _ in configs[1:]:
            v = results.get((clip.stem, name))
            if v is None:
                continue
            if not v:
                found.append((clip.stem, name, "no result (eval failed or printed nothing)"))
                continue
            checks = []
            a, b = exact_sharp(v), exact_sharp(ref)
            if a is not None and b is not None and a < b - 0.5:
                checks.append("exact+sharp %.1f%% -> %.1f%% (%+.1f points)" % (b, a, a - b))
            a, b = get(v, "t_wrong"), get(ref, "t_wrong")
            if a is not None and b is not None and a > b + 0.05:
                checks.append("wrong %.2f%% -> %.2f%% (%+.2f points)" % (b, a, a - b))
            a, b = get(v, "p95_ms"), get(ref, "p95_ms")
            if a is not None and b is not None and a > b + 50:
                checks.append("95%% until exact or sharp %.0f ms -> %.0f ms" % (b, a))
            a, b = get(v, "total_kb"), get(ref, "total_kb")
            d = change_pct(a, b)
            if d is not None and d > 2:
                checks.append("traffic %.0f kB -> %.0f kB (%+.1f%%)" % (b, a, d))
            for c in checks:
                found.append((clip.stem, name, c))
    return found


def report(results, clips, configs, markdown):
    ref_name = configs[0][0]
    rows1, rows2, rows3 = [], [], []
    for clip in clips:
        ref = results.get((clip.stem, ref_name)) or {}
        for name, _ in configs:
            v = results.get((clip.stem, name)) or {}
            if name == ref_name:
                delta = "ref"
            else:
                d = change_pct(get(v, "total_kb"), get(ref, "total_kb"))
                delta = "-" if d is None else "%+.1f%%" % d
            rows1.append([clip.stem, name, fmt(v.get("total_kb")), fmt(v.get("video_kb")), fmt(v.get("lossless_kb")),
                          fmt(v.get("kbps")), delta])
            rows2.append([clip.stem, name, fmt(v.get("text_share")), fmt(exact_sharp(v), 1), fmt(v.get("t_exact")),
                          fmt(v.get("t_sharp")), fmt(v.get("t_soft")), fmt(v.get("t_soft_psnr")), fmt(v.get("t_wrong")),
                          fmt(v.get("t_before")), fmt(v.get("median_ms")), fmt(v.get("p95_ms")),
                          fmt(v.get("longest_ms")), fmt(v.get("not_exact_end"))])
            rows3.append([clip.stem, name, fmt(v.get("pictures")), fmt(v.get("key_kb")), fmt(v.get("new_kb")),
                          fmt(v.get("refine_kb")), fmt(v.get("polish_kb")), fmt(v.get("r_lossless")),
                          fmt(v.get("r_colours")), fmt(v.get("r_changing")), fmt(v.get("r_budget")),
                          fmt(v.get("r_motion")), fmt(v.get("r_keyframe"))])
    table("Traffic (kB; change of the total against %s)" % ref_name,
          ["clip", "config", "total kB", "video kB", "lossless kB", "kbps", "total vs ref"], rows1, markdown)
    table("Text (% of text cells by time; times until exact or sharp in ms)",
          ["clip", "config", "text %", "exact+sharp", "exact", "sharp", "soft", "soft PSNR", "wrong", "as before",
           "median", "95%", "longest", "not exact at end"], rows2, markdown)
    table("Video pictures coded for (kB), and rects sent lossless / to video and why",
          ["clip", "config", "pictures", "keyframes", "new content", "refinement", "still polish", "lossless rects",
           "too many colours", "keeps changing", "over budget", "in motion", "keyframe first"], rows3, markdown)

    found = regressions(results, clips, configs)
    print()
    title = "Regressions against %s" % ref_name
    rule = ("exact+sharp lower by more than 0.5 points, wrong higher by more than 0.05 points, "
            "95% time higher by more than 50 ms, or total traffic higher by more than 2%")
    if markdown:
        print("### " + title)
        print()
        print("(" + rule + ")")
        print()
        if not found:
            print("none")
        for clip, name, what in found:
            print("- %s, %s: %s" % (clip, name, what))
    else:
        print(title + " (" + rule + ")")
        if not found:
            print("  none")
        for clip, name, what in found:
            print("  %-12s %-10s %s" % (clip, name, what))


# --- main -------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Runs rdaccel_clip eval on clips with several configurations "
                                             "and compares them (the first is the reference).")
    ap.add_argument("--clips", default=str(REPO / "clips"), help="directory of *.clip files (default: clips/ in the repo)")
    ap.add_argument("--bin", default=str(REPO / "build" / "bin" / "rdaccel_clip"), help="the rdaccel_clip binary")
    ap.add_argument("--only", help="comma-separated clip names (without .clip)")
    ap.add_argument("--out", default=str(REPO / "clips" / "baseline" / "runs"), help="where each run's output is kept")
    ap.add_argument("--force", action="store_true", help="run again even when a kept output could be reused")
    ap.add_argument("--list", action="store_true", help="only list the clips and configurations that would run")
    ap.add_argument("--markdown", action="store_true", help="print Markdown tables")
    ap.add_argument("configs", nargs="+", metavar="NAME='ARGS'", help="a configuration: a name and the eval options")
    opt = ap.parse_args()

    configs = []
    for c in opt.configs:
        name, sep, args = c.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            ap.error("configuration %r: expected NAME='ARGS' with a NAME of letters, digits, _ or -" % c)
        if any(name == n for n, _ in configs):
            ap.error("configuration name %r given twice" % name)
        try:
            shlex.split(args)
        except ValueError as e:
            ap.error("configuration %s: cannot split its ARGS: %s" % (name, e))
        configs.append((name, " ".join(args.split())))

    binary = Path(opt.bin)
    clip_dir = Path(opt.clips)
    if not clip_dir.is_dir():
        sys.exit("no clip directory %s (record or render clips first: docs/clips.md)" % clip_dir)
    if opt.only:
        names = [n.strip() for n in opt.only.split(",") if n.strip()]
        clips = [clip_dir / (n if n.endswith(".clip") else n + ".clip") for n in names]
        missing = [c.name for c in clips if not c.is_file()]
        if missing:
            sys.exit("not in %s: %s" % (clip_dir, ", ".join(missing)))
    else:
        clips = sorted(clip_dir.glob("*.clip"))
        if not clips:
            sys.exit("no *.clip files in %s" % clip_dir)
    out_dir = Path(opt.out)
    have_bin = binary.is_file() and os.access(binary, os.X_OK)

    if opt.list:
        print("binary: %s%s" % (binary, "" if have_bin else "  (missing: cmake --build build --target rdaccel_clip)"))
        print("output: %s" % out_dir)
        print("configurations (the first is the reference):")
        for name, args in configs:
            print("  %-12s %s" % (name, args))
        print("clips:")
        for clip in clips:
            states = []
            for name, args in configs:
                p = out_dir / ("%s.%s.txt" % (clip.stem, name))
                states.append("%s %s" % (name, "kept" if have_bin and not opt.force and usable(p, args, clip, binary)
                                         else "run"))
            print("  %-14s %7.1f MB  %s" % (clip.stem, clip.stat().st_size / 1e6, ", ".join(states)))
        return 0

    if not have_bin:
        sys.exit("no rdaccel_clip at %s (build it: cmake --build build --target rdaccel_clip)" % binary)
    out_dir.mkdir(parents=True, exist_ok=True)

    results = {}
    missing_report = []
    for clip in clips:
        for name, args in configs:
            path = out_dir / ("%s.%s.txt" % (clip.stem, name))
            if not opt.force and usable(path, args, clip, binary):
                print("%s %s: kept output %s" % (clip.stem, name, path), file=sys.stderr)
                body = path.read_text(encoding="utf-8", errors="replace")
                status = 0
            else:
                print("%s %s: running eval %s" % (clip.stem, name, args), file=sys.stderr, end="", flush=True)
                status, secs, body = run(binary, clip, name, args, path)
                print(" (%.0f s)" % secs, file=sys.stderr)
            v = parse(body)
            if status != 0 or not v:
                tail = [l for l in body.splitlines()[1:] if not l.startswith("#")][-3:]
                print("%s %s: eval failed with status %d: %s" % (clip.stem, name, status, " / ".join(tail) or "no output"),
                      file=sys.stderr)
            results[(clip.stem, name)] = v
            lost = [k for k, _, _ in FIELDS if k not in v]
            if lost and v:  # a run with no values at all was reported above
                missing_report.append("%s %s: %s" % (clip.stem, name, ", ".join(lost)))

    if missing_report:
        print("values eval did not print (shown as -; an older binary or a different mode):", file=sys.stderr)
        for m in missing_report:
            print("  " + m, file=sys.stderr)
    report(results, clips, configs, opt.markdown)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
