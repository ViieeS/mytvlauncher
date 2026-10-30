"""Rasterise the UI fonts into LVGL v8 `lv_font_fmt_txt` C sources (4 bpp, plain bitmaps).

Stands in for lv_font_conv: FreeType (via Pillow) renders each glyph, this script packs the
bitmaps and metrics. No kerning table -- lumen's tracking (letter-spacing) is applied by
trimming every advance instead, which is what the CSS does too.

usage: make_fonts.py <dir with geist-500.ttf / geist-600.ttf> <out .c> <out .h>
"""
import sys
from PIL import Image, ImageDraw, ImageFont

TTF_DIR, OUT_C, OUT_H = sys.argv[1], sys.argv[2], sys.argv[3]

ASCII = [chr(c) for c in range(0x20, 0x7F)]
EXTRA = ["°", "·", "–", "—", "…"]
LATIN1 = [chr(c) for c in range(0xC0, 0x100)]   # city names: Zürich, São Paulo
CAPS = [" "] + [chr(c) for c in range(ord("0"), ord("9") + 1)] + [chr(c) for c in range(ord("A"), ord("Z") + 1)]

# (name, weight, px, tracking in em, charset). Sizes/weights are lumen's CSS values.
FONTS = [
    ("geist_19m", 500, 19, -0.011, ASCII + EXTRA),   # cc__label
    ("geist_19s", 600, 19, -0.015, ASCII + EXTRA),   # tile__label
    ("geist_20m", 500, 20, -0.011, ASCII + EXTRA),   # pill__label
    ("geist_21m", 500, 21, -0.011, ASCII + EXTRA),   # cc__sub
    ("geist_21s", 600, 21, -0.011, ASCII + EXTRA),   # 1.1.4 cc__name, cc__slidertext
    ("geist_20s", 600, 20, -0.011, ASCII + EXTRA),   # 1.1.4 cc__caption
    ("geist_22m", 500, 22, -0.005, ASCII + EXTRA + LATIN1 + list("\u25c0\u25b6\u25b2\u25bc")),   # status__weather, cc__net, hints, toast
    ("geist_24m", 500, 24, -0.011, ASCII + EXTRA),   # toast
    ("geist_24s", 600, 24, -0.015, ASCII + EXTRA),   # panel tile label
    ("geist_26s", 600, 26, -0.011, ASCII + EXTRA),   # cc__name, cc__slidertext
    ("geist_26m", 500, 26, -0.011, ASCII + EXTRA + LATIN1),   # movehint, option values
    ("geist_28m", 500, 28, -0.011, ASCII + EXTRA),   # menu__row
    ("geist_30m", 500, 30, -0.012, ASCII + EXTRA),   # status date + clock
    ("geist_30s", 600, 30, -0.020, ASCII + EXTRA),   # cc__title, panel__title, tabs
    ("geist_32m", 500, 32, -0.012, ASCII + EXTRA + LATIN1),   # 1.1.4 .slist__row
    ("geist_52s", 600, 52, -0.020, ASCII + EXTRA),   # 1.1.4 .slist__title
    ("geist_44b", 700, 44, -0.020, ASCII + EXTRA),   # 1.1.4 .switcher__title
    ("geist_34m", 500, 34, -0.011, ASCII + EXTRA),   # 1.1.4 .switcher__empty
    ("geist_96b", 700, 96, 0.0, CAPS),               # 1.1.4 .switcher__art monogram
    # StandBy / photo saver (lumen 1.1.4 .sb__*, .ambient__*): only the glyphs each one shows
    ("sb_time", 800, 460, -0.045, list("0123456789:")),          # .sb__time 460/800
    ("sb_time_light", 300, 440, -0.040, list("0123456789:")),    # solar .sb__time--light 440/300
    ("sb_temp", 700, 420, -0.040, list("0123456789-\u00b0\u2014")),  # .sb__temp 420/700
    ("sb_small", 650, 120, -0.020, list("0123456789: AMP")),    # .sb__time--small, .ambient clock 120/650
    ("sb_period", 500, 80, 0.0, list(" AMP")),                   # .sb__period 80/500
    ("sb_numeral", 500, 80, 0.0, list("0123456789")),            # analog numerals
    ("sb_cond", 500, 84, -0.005, ASCII),                         # .sb__cond 84/500
    ("sb_month", 600, 56, 0.02, CAPS),                           # .sb__month 56/600 uppercase
    ("sb_day", 500, 50, 0.0, list(" 0123456789")),               # .sb__day 50/500
    ("sb_wd", 600, 42, 0.0, list(" SMTWF")),                     # .sb__wd 42/600
    ("sb_place", 600, 48, -0.005, ASCII + LATIN1),               # .sb__place 48/600
    ("sb_sub", 500, 40, -0.005, ASCII + EXTRA + LATIN1),         # .sb__sub 40/500
    ("geist_32n", 400, 32, 0.0, list(" AMP")),                   # .ambient clock__period 32/400
    ("geist_64s", 600, 64, 0.0, CAPS),               # tile__monogram
    # Settings > Clock: weight Light/Bold of the one-row block, the Stacked layout's big time
    ("geist_30l", 300, 30, -0.012, ASCII + EXTRA),   # clockw-light status date + clock
    ("geist_30b", 700, 30, -0.012, ASCII + EXTRA),   # clockw-bold
    ("clk_104l", 300, 104, -0.010, list("0123456789: ")),        # clock-stacked .clock__time 104
    ("clk_104s", 600, 104, -0.010, list("0123456789: ")),        #   (600 is the stacked default)
    ("clk_104b", 700, 104, -0.010, list("0123456789: ")),
    ("clk_84s", 600, 84, -0.010, ASCII),                         # stacked .clock__sec / __date (.8em)
    ("geist_36m", 500, 36, -0.012, list(" AMP")),                # stacked .clock__period 36
]


def pack4(img):
    """Pack an 8-bit coverage image into continuous 4 bpp (LVGL plain format)."""
    px = img.tobytes()
    out, acc, n = bytearray(), 0, 0
    for v in px:
        acc = (acc << 4) | (v >> 4)
        n += 1
        if n == 2:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << 4)
    return out


def build(name, weight, px, track, chars):
    font = ImageFont.truetype(f"{TTF_DIR}/geist-{weight if weight != 400 else 500}.ttf", px)
    ascent, descent = font.getmetrics()
    chars = sorted(set(chars), key=ord)
    bitmap, dscs = bytearray(), ["{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}"]
    for ch in chars:
        adv = max(0, round((font.getlength(ch) + track * px) * 16))
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w, h = r - l, b - t
        if w <= 0 or h <= 0:
            dscs.append(f"{{.bitmap_index = {len(bitmap)}, .adv_w = {adv}, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}}")
            continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
        dscs.append(f"{{.bitmap_index = {len(bitmap)}, .adv_w = {adv}, .box_w = {w}, .box_h = {h}, "
                    f".ofs_x = {l}, .ofs_y = {-b}}}")
        bitmap += pack4(img)

    codes = [ord(c) for c in chars]
    # One sparse cmap covers both ASCII and the handful of extras.
    ulist = ", ".join(str(c - codes[0]) for c in codes)
    rows = [", ".join(f"0x{v:02x}" for v in bitmap[i:i + 24]) for i in range(0, len(bitmap), 24)]
    return f"""
/* {name}: Geist {weight}, {px}px */
static const uint8_t {name}_bitmap[] = {{
    {(","+chr(10)+"    ").join(rows) or "0"}
}};
static const lv_font_fmt_txt_glyph_dsc_t {name}_glyphs[] = {{
    {(","+chr(10)+"    ").join(dscs)}
}};
static const uint16_t {name}_ulist[] = {{ {ulist} }};
static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{
    {{ .range_start = {codes[0]}, .range_length = {codes[-1] - codes[0] + 1}, .glyph_id_start = 1,
      .unicode_list = {name}_ulist, .glyph_id_ofs_list = NULL, .list_length = {len(codes)},
      .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY }}
}};
static lv_font_fmt_txt_glyph_cache_t {name}_cache;
static const lv_font_fmt_txt_dsc_t {name}_dsc = {{
    .glyph_bitmap = {name}_bitmap, .glyph_dsc = {name}_glyphs, .cmaps = {name}_cmaps,
    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = 1, .bpp = 4, .kern_classes = 0,
    .bitmap_format = 0, .cache = &{name}_cache
}};
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt, .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {ascent + descent}, .base_line = {descent}, .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -2, .underline_thickness = 1, .dsc = &{name}_dsc
}};
"""


with open(OUT_C, "w") as f:
    f.write("/* Generated by tools/make_fonts.py from lumen's Geist variable font. Do not edit. */\n")
    f.write('#include "lvgl/lvgl.h"\n')
    for spec in FONTS:
        f.write(build(*spec))
with open(OUT_H, "w") as f:
    f.write("/* Generated by tools/make_fonts.py. */\n#pragma once\n#include \"lvgl/lvgl.h\"\n")
    for spec in FONTS:
        f.write(f"extern const lv_font_t {spec[0]};\n")
print("fonts:", ", ".join(s[0] for s in FONTS))
