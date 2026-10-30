"""Rasterise lumen's line icons into LVGL ARGB8888 image descriptors (white, alpha = stroke).

The paths are lumen's own 24x24 glyphs (its `tile__glyph` set); the launcher tints them at draw
time (img_opa for the 62 % white idle state, recolor for the dark focused state), so one white
mask per size serves every state.

usage: make_icons.py <out .c> <out .h>     (needs rsvg-convert on PATH)
"""
import io
import subprocess
import sys
from PIL import Image

OUT_C, OUT_H = sys.argv[1], sys.argv[2]

P = {
    "house": "M3 11.5 12 4l9 7.5M5.5 9.8V20h13V9.8M10 20v-6h4v6",
    "wifi": "M2.5 9a14 14 0 0 1 19 0M5.6 12.5a9.5 9.5 0 0 1 12.8 0M8.8 16a5 5 0 0 1 6.4 0M12 19.4h.01",
    "ethernet": "M4 6h16v8h-3v3.5h-3V14h-4v3.5H7V14H4zM8 9.2v2.2M12 9.2v2.2M16 9.2v2.2",
    "muted": "M4 9.5h3.5L12 6v12l-4.5-3.5H4zM16 9.5l5 5M21 9.5l-5 5",
    "timer": "M12 8v4.5l3 1.8M12 4.5a8 8 0 1 0 0 16 8 8 0 0 0 0-16zM9.5 2.5h5",
    "speaker": "M4 9.5h3.5L12 6v12l-4.5-3.5H4zM15.5 9a4 4 0 0 1 0 6M18 6.5a8 8 0 0 1 0 11",
    "picture": "M3.5 5.5h17v11h-17zM8 20h8M12 16.5V20",
    "power": "M12 3.5v8M7 6.5a7.5 7.5 0 1 0 10 0",
    "restart": "M19 12a7 7 0 1 1-2.2-5.1M19 4.5V8h-3.5",
    "folder": "M3.5 7.5a2 2 0 0 1 2-2h4l2 2.5h7a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z",
    "sliders": "M4 7h8M16 7h4M4 17h4M12 17h8M12 4.5v5M8 14.5v5",
    "screensaver": "M20.5 14.2A8.5 8.5 0 0 1 9.8 3.5a8.5 8.5 0 1 0 10.7 10.7z",
    "inputs": "M3.5 7.5h17v7.2a2 2 0 0 1-2 2h-3.2l-1 2.3H9.7l-1-2.3H5.5a2 2 0 0 1-2-2zM8 10.5v2.4M12 10.5v2.4M16 10.5v2.4",
    "gear": ("M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32"
             "c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84"
             "c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87"
             "c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61"
             "l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84"
             "c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32"
             "c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"),
    "check": "M5 12.5l4.5 4.5L19 7.5",
    # lumen 1.1.4 additions
    "sun": "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4",
    "eye": "M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12zM12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6z",
    "cloud": "M7.5 19h10a4 4 0 0 0 .4-8 6 6 0 0 0-11.6 1.6A3.3 3.3 0 0 0 7.5 19z",
    "grid": "M4 4h6.5v6.5H4zM13.5 4H20v6.5h-6.5zM4 13.5h6.5V20H4zM13.5 13.5H20V20h-6.5z",
    "image": "M3.5 5h17v14h-17zM3.5 15.5l5-5 4 4 3-3 5 5M15.5 9.3a.8.8 0 1 0 0-.1",
    "contrast": "M12 3.5a8.5 8.5 0 1 0 0 17 8.5 8.5 0 0 0 0-17zM12 3.5v17M12 7.5h4.5M12 11.5h6.5M12 15.5h5",
}

# (c name, glyph, px, stroke width in 24-unit space). lumen: tile__glyph 1.6, pill 1.8, badge tick 3.
ICONS = [(f"ic_{g}_32", g, 32, 1.6) for g in
         ("house", "muted", "timer", "speaker", "picture", "power", "restart", "screensaver", "gear")]
ICONS += [("ic_inputs_28", "inputs", 28, 1.8), ("ic_sliders_28", "sliders", 28, 1.8),
          ("ic_wifi_26", "wifi", 26, 1.6), ("ic_ethernet_26", "ethernet", 26, 1.6),
          ("ic_inputs_76", "inputs", 76, 1.6), ("ic_check_20", "check", 20, 3.0)]
# Control Center 1.1.4: .cc__mod svg stroke 1.8; tile glyphs 40px, circles 44px, list/slider 34px
ICONS += [(f"ic_{g}_40", g, 40, 1.8) for g in ("speaker", "picture", "screensaver", "power")]
ICONS += [(f"ic_{g}_44", g, 44, 1.8) for g in ("screensaver", "house", "power", "gear", "eye")]
ICONS += [(f"ic_{g}_34", g, 34, 1.8) for g in ("sun", "speaker", "picture", "screensaver", "power")]


def svg(d, px, sw, vb="0 0 24 24"):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{px}" height="{px}" viewBox="{vb}">'
            f'<path d="{d}" fill="none" stroke="#fff" stroke-width="{sw}" stroke-linecap="round" '
            f'stroke-linejoin="round"/></svg>')


def raster(src_svg, w, h):
    png = subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h)], input=src_svg.encode(),
                         capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")


def emit(name, img):
    w, h = img.size
    data = bytearray()
    for a in img.getchannel("A").tobytes():
        data += bytes((255, 255, 255, a))     # BGRA, white
    rows = [", ".join(f"0x{v:02x}" for v in data[i:i + 32]) for i in range(0, len(data), 32)]
    return (f"static const uint8_t {name}_px[] = {{\n    " + ",\n    ".join(rows) + "\n};\n"
            f"const lv_img_dsc_t {name} = {{ .header.always_zero = 0, .header.w = {w}, .header.h = {h}, "
            f".header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .data_size = {len(data)}, .data = {name}_px }};\n")


# Settings 1.1.4: the side panel's 480px icon draws its glyph at 40 % (192px), stroke 1.4
ICONS += [(f"ic_{g}_192", g, 192, 1.4) for g in
          ("grid", "inputs", "image", "house", "screensaver", "cloud", "contrast", "timer", "gear")]
ICONS += [("ic_tick_30", "check", 30, 2.6)]

out_c = ["/* Generated by tools/make_icons.py from lumen's glyph paths. Do not edit. */",
         '#include "lvgl/lvgl.h"']
names = []
for name, g, px, sw in ICONS:
    out_c.append(emit(name, raster(svg(P[g], px, sw), px, px)))
    names.append(name)
# Weather icons (.wicon): viewBox 32, stroke 2, drawn at 1.15em of 22px = 25px
CLOUD = '<path d="M9 23h14a5 5 0 0 0 .6-9.96A7 7 0 0 0 10 12.5 5.2 5.2 0 0 0 9 23z"/>'
SUN = '<circle cx="16" cy="16" r="5"/>' + "".join(
    f'<path d="M16 4.5V7.5" transform="rotate({k * 45} 16 16)"/>' for k in range(8))
MOON = '<path d="M22 20.5A9 9 0 0 1 11.5 10 9 9 0 1 0 22 20.5z"/>'
WEATHER = {
    "sun": SUN, "moon": MOON, "cloud": CLOUD,
    "storm": CLOUD + '<path d="M16 23l-2.5 4.5h4L15 31"/>',
    "snow": CLOUD + '<path d="M11 27h.01M16 28h.01M21 27h.01"/>',
    "rain": CLOUD + '<path d="M12 26.5l-1 2.5M17 26.5l-1 2.5M22 26.5l-1 2.5"/>',
    "fog": '<path d="M6 12h20M4 17h24M8 22h16"/>',
    "partly_sun": f'<g transform="translate(-3 -4) scale(.72)">{SUN}</g><g transform="translate(5 4) scale(.8)">{CLOUD}</g>',
    "partly_moon": f'<g transform="translate(-3 -4) scale(.72)">{MOON}</g><g transform="translate(5 4) scale(.8)">{CLOUD}</g>',
}
for name, body in WEATHER.items():
    doc = (f'<svg xmlns="http://www.w3.org/2000/svg" width="25" height="25" viewBox="0 0 32 32" fill="none" '
           f'stroke="#fff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">{body}</svg>')
    out_c.append(emit(f"wx_{name}", raster(doc, 25, 25)))
    names.append(f"wx_{name}")
# Dock hint chevron: lumen's own 48x28 viewBox, stroke 3.2, drawn at 52x30.
out_c.append(emit("ic_chevron_up", raster(svg("M6 22 24 6l18 16", 52, 3.2, "0 0 48 28").replace(
    'width="52" height="52"', 'width="52" height="30"'), 52, 30)))
names.append("ic_chevron_up")
# .slist__chevron: 14x24, path M2 2l8 8-8 8 in a 12x20 box, stroke 2.6
out_c.append(emit("ic_chevron_right", raster(svg("M2 2l8 8-8 8", 14, 2.6, "0 0 12 20").replace(
    'width="14" height="14"', 'width="14" height="24"'), 14, 24)))
names.append("ic_chevron_right")

open(OUT_C, "w").write("\n".join(out_c))
open(OUT_H, "w").write("/* Generated by tools/make_icons.py. */\n#pragma once\n#include \"lvgl/lvgl.h\"\n" +
                       "".join(f"extern const lv_img_dsc_t {n};\n" for n in names))
print("icons:", len(names))
