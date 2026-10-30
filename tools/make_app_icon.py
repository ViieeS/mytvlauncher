"""Draw the app icon: a dark glass tile with the "myTV" wordmark, at every size webOS and the
Homebrew Channel ask for (80 launcher icon, 130 largeIcon, 160 / 320 for the channel listing),
and splash.png, the full-screen picture webOS shows while the app starts (splashBackground).

usage: make_app_icon.py <package dir>      (reads tools/geist-700.ttf)
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1]
TTF = os.path.join(os.path.dirname(__file__), "geist-700.ttf")
M = 1024                                   # master size, downscaled for every output


def gradient(w, h):
    grad = Image.new("RGBA", (w, h))
    g = ImageDraw.Draw(grad)
    for y in range(h):                     # deep blue at the top to near-black at the bottom
        t = y / (h - 1)
        g.line([(0, y), (w, y)], fill=(int(38 - 26 * t), int(64 - 46 * t), int(120 - 88 * t), 255))
    return grad


def wordmark(img, size):
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(TTF, size)
    text = "myTV"
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    w, h = img.size
    d.text(((w - (r - l)) / 2 - l, (h - (b - t)) / 2 - t), text, font=font, fill=(255, 255, 255, 255))


def master():
    img = Image.new("RGBA", (M, M))
    grad = gradient(M, M)
    mask = Image.new("L", (M, M))
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, M - 1, M - 1], radius=M * 0.22, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([6, 6, M - 7, M - 7], radius=M * 0.22 - 6, outline=(255, 255, 255, 46), width=6)
    wordmark(img, int(M * 0.30))
    return img


m = master()
for name, px in (("icon.png", 80), ("largeIcon.png", 130), ("icon160.png", 160), ("icon320.png", 320)):
    m.resize((px, px), Image.LANCZOS).save(os.path.join(OUT, name), optimize=True)
    print("wrote", name)

splash = gradient(1920, 1080)
wordmark(splash, 168)
splash.convert("RGB").save(os.path.join(OUT, "splash.png"), optimize=True)
print("wrote splash.png")
