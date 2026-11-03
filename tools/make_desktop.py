#!/usr/bin/env python3
"""Draws a deterministic, desktop-like screenshot for rdaccel_bench.

    python3 tools/make_desktop.py desktop.ppm [--size=1920x1080]
    ./build/bin/rdaccel_bench --background=desktop.ppm

The built-in test pattern is mostly flat colour, so its "video" scene is
nearly free to code. This picture has what real screens have: anti-aliased
text in several sizes and colours, a photo-like wallpaper and image, UI
chrome and gradients. It needs Pillow and the DejaVu fonts (fonts-dejavu on
Debian/Ubuntu); with other fonts the numbers shift a little.
"""

import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_DIRS = ["/usr/share/fonts/truetype/dejavu/", "/Library/Fonts/", "/System/Library/Fonts/Supplemental/"]


def font(name, size):
    for d in FONT_DIRS:
        try:
            return ImageFont.truetype(d + name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def fractal(w, h, rnd, octaves=6):
    """Grey value noise with a 1/f spectrum, like clouds or foliage."""
    out = Image.new("L", (w, h), 128)
    amp = 0.5
    for o in range(octaves):
        cw, ch = max(2, w >> (octaves - o)), max(2, h >> (octaves - o))
        layer = Image.new("L", (cw, ch))
        layer.putdata([rnd.randrange(256) for _ in range(cw * ch)])
        layer = layer.resize((w, h), Image.BICUBIC)
        out = Image.blend(out, layer, amp)
        amp *= 0.6
    return out


def colorize(grey, stops):
    lut = []
    for c in range(3):
        for v in range(256):
            t = v / 255 * (len(stops) - 1)
            i = min(int(t), len(stops) - 2)
            f = t - i
            lut.append(int(stops[i][c] * (1 - f) + stops[i + 1][c] * f))
    return Image.merge("RGB", [grey.point(lut[c * 256:(c + 1) * 256]) for c in range(3)])


def photo(w, h, rnd):
    sky = colorize(fractal(w, h, rnd), [(40, 70, 140), (120, 160, 210), (230, 230, 240)])
    land = colorize(fractal(w, h, rnd, 7), [(20, 40, 15), (70, 110, 40), (160, 150, 90), (90, 70, 50)])
    mask = Image.linear_gradient("L").resize((w, h)).point(lambda v: 255 if v > 120 else 0)
    mask = mask.filter(ImageFilter.GaussianBlur(3))
    img = Image.composite(land, sky, mask)
    return img.filter(ImageFilter.UnsharpMask(2, 80))


WORDS = ("the of and to in is that for it as was with be by on not he this are or his from at "
         "which but have an they you were her she there been one all we their has would when "
         "encoder region tile bitrate latency frame video screen cursor packet stream lossless "
         "network window desktop library quality palette motion scroll keyframe").split()


def paragraph(rnd, n):
    return " ".join(rnd.choice(WORDS) for _ in range(n))


def window(d, box, title, dark=False):
    x0, y0, x1, y1 = box
    d.rectangle((x0 + 4, y0 + 6, x1 + 4, y1 + 8), fill=(0, 0, 0, 40))
    d.rectangle(box, fill=(30, 30, 34) if dark else (252, 252, 252), outline=(180, 180, 185))
    d.rectangle((x0, y0, x1, y0 + 32), fill=(48, 48, 54) if dark else (232, 232, 236))
    for i, c in enumerate([(237, 95, 87), (246, 190, 79), (98, 197, 84)]):
        d.ellipse((x0 + 12 + i * 20, y0 + 10, x0 + 24 + i * 20, y0 + 22), fill=c)
    d.text((x0 + 80, y0 + 8), title, font=font("DejaVuSans.ttf", 14),
           fill=(220, 220, 220) if dark else (60, 60, 60))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "desktop.ppm"
    w, h = 1920, 1080
    for a in sys.argv[2:]:
        if a.startswith("--size="):
            w, h = (int(v) for v in a[7:].split("x"))
    rnd = random.Random(42)
    img = photo(w, h, rnd).convert("RGBA")
    d = ImageDraw.Draw(img, "RGBA")
    s = w / 1920

    # Menu bar and dock.
    d.rectangle((0, 0, w, int(28 * s)), fill=(245, 245, 247, 230))
    menu_font = font("DejaVuSans.ttf", int(14 * s))
    x = 16
    for item in ["Finder", "File", "Edit", "View", "Go", "Window", "Help"]:
        d.text((x, 5 * s), item, font=menu_font, fill=(20, 20, 20))
        x += menu_font.getlength(item) + 22
    d.text((w - 190 * s, 5 * s), "Tue 3 Nov  10:42", font=menu_font, fill=(20, 20, 20))
    d.rounded_rectangle((w * 0.3, h - 76 * s, w * 0.7, h - 8 * s), 18, fill=(240, 240, 245, 170))
    for i in range(10):
        cx = w * 0.3 + 24 * s + i * (w * 0.4 - 48 * s) / 9
        c = tuple(rnd.randrange(40, 230) for _ in range(3))
        d.rounded_rectangle((cx - 24 * s, h - 66 * s, cx + 24 * s, h - 18 * s), 10, fill=c)

    # Code editor (dark).
    eb = (int(40 * s), int(60 * s), int(900 * s), int(760 * s))
    window(d, eb, "host_session.cpp — rdaccel", dark=True)
    mono = font("DejaVuSansMono.ttf", int(14 * s))
    colours = [(197, 134, 192), (86, 156, 214), (206, 145, 120), (220, 220, 170), (156, 220, 254),
               (106, 153, 85), (212, 212, 212)]
    y = eb[1] + 44 * s
    line = 1
    while y < eb[3] - 20 * s:
        d.text((eb[0] + 12 * s, y), f"{line:4d}", font=mono, fill=(110, 110, 110))
        x = eb[0] + 70 * s + rnd.choice([0, 0, 1, 2, 3]) * 28 * s
        for _ in range(rnd.randrange(1, 7)):
            tok = rnd.choice(WORDS) + rnd.choice(["", "(", ");", " =", "_", "::", ","])
            d.text((x, y), tok, font=mono, fill=rnd.choice(colours))
            x += mono.getlength(tok + " ")
            if x > eb[2] - 40 * s:
                break
        y += 20 * s
        line += 1

    # Browser with an article and a photo.
    bb = (int(700 * s), int(120 * s), int(1860 * s), int(980 * s))
    window(d, bb, "Low-latency screen coding — Browser")
    d.rounded_rectangle((bb[0] + 80 * s, bb[1] + 40 * s, bb[2] - 80 * s, bb[1] + 66 * s), 12,
                        fill=(240, 240, 243))
    d.text((bb[0] + 96 * s, bb[1] + 44 * s), "https://example.org/articles/screen-coding",
           font=font("DejaVuSans.ttf", int(13 * s)), fill=(90, 90, 90))
    d.text((bb[0] + 60 * s, bb[1] + 90 * s), "Coding screens for remote desktops",
           font=font("DejaVuSerif-Bold.ttf", int(30 * s)), fill=(20, 20, 20))
    body = font("DejaVuSerif.ttf", int(16 * s))
    pw, ph = int(460 * s), int(300 * s)
    px, py = int(bb[2] - 60 * s - pw), int(bb[1] + 150 * s)
    img.paste(photo(pw, ph, rnd), (px, py))
    y = bb[1] + 150 * s
    while y < bb[3] - 30 * s:
        right = px - 30 * s if y < py + ph + 10 * s else bb[2] - 60 * s
        x = bb[0] + 60 * s
        while True:
            word = rnd.choice(WORDS)
            wl = body.getlength(word + " ")
            if x + wl > right:
                break
            d.text((x, y), word, font=body, fill=(40, 40, 40))
            x += wl
        y += 26 * s
        if rnd.random() < 0.12:
            y += 18 * s

    # A small chart window with gradients.
    cb = (int(120 * s), int(800 * s), int(620 * s), int(1000 * s))
    window(d, cb, "Activity")
    for i in range(24):
        bh = rnd.randrange(10, 130) * s
        x0 = cb[0] + 20 * s + i * 19 * s
        for k in range(int(bh)):
            t = k / max(1, bh)
            d.line((x0, cb[3] - 14 * s - k, x0 + 13 * s, cb[3] - 14 * s - k),
                   fill=(int(60 + 120 * t), int(130 + 60 * t), 230))

    rgb = img.convert("RGB")
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(rgb.tobytes())


if __name__ == "__main__":
    main()
