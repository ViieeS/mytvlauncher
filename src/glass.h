/* lumen's "glass" without live blur.
 *
 * lumen fakes frosted glass by showing a pre-blurred copy of the wallpaper through each glass
 * shape, plus a white sheen gradient and thin inset rim lights. LVGL v8 has neither backdrop
 * blur nor inset shadows, so every glass surface is composited once on the CPU into a plain
 * ARGB image (shape, rim, sheen and the outer drop shadow baked in). At runtime it is a single
 * cached texture blit -- the same cost as any icon.
 *
 * Nothing here touches LVGL state, so glass_render() is safe to call from a worker thread. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

#define GLASS_DIV 8   /* blur source is the wallpaper downscaled 8x (240x135 for 1080p) */

/* Blurred, downscaled wallpaper (BGRA, opaque). Also used directly, zoomed 8x, as the
 * full-screen "lower" veil. */
typedef struct {
    uint8_t *px;
    int w, h;
} glass_src_t;

/* One glass recipe. Alphas are 0..255; the CSS each one mirrors is noted at the call site. */
typedef struct {
    uint8_t opa;                         /* whole-surface opacity (dock tray: .74) */
    float sheen_deg;                     /* gradient angle, CSS convention (135 / 145) */
    uint8_t sheen_a[3];                  /* white alpha at the 0 / mid / 100 % stops */
    float sheen_mid;                     /* position of the middle stop, 0..1 */
    uint8_t rim_t, rim_l, rim_b, rim_r;  /* inset 1-2px highlight per side */
    uint8_t glow;                        /* inset 0 0 <glow_w>px white */
    int glow_w;
    uint8_t dim;                         /* black overlay over the blur */
    uint32_t flat;                       /* 0xAARRGGBB: a flat translucent base instead of the blur
                                          * (1.1.4 tray: rgba(22,28,40,.22)); src may then be NULL */
    int sh_y, sh_blur;                   /* outer drop shadow: 0 <sh_y>px <sh_blur>px black */
    uint8_t sh_opa;
} glass_style_t;

/* A rendered surface. The image is larger than the shape by the shadow margin; `ox, oy` is
 * where the shape's top-left sits inside it, so place the image at (x - ox, y - oy). */
typedef struct {
    lv_img_dsc_t dsc;
    int ox, oy;
} glass_img_t;

/* Downscale + blur a BGRA wallpaper into `out` (allocates out->px). */
void glass_src_build(glass_src_t *out, const uint8_t *bgra, int w, int h);

/* Composite a rounded rect of size w x h at screen position (x, y), radius r, over the
 * blurred wallpaper `src`. Allocates the pixel buffer; returns false on OOM. */
bool glass_render(glass_img_t *out, const glass_src_t *src, int x, int y, int w, int h, int r,
                  const glass_style_t *st);

/* A rim-only overlay (transparent interior): lumen's inset highlights on surfaces whose fill
 * LVGL draws itself (control-centre tiles, circles, slider). */
bool glass_rim(lv_img_dsc_t *out, int w, int h, int r, uint8_t rim_tl, uint8_t rim_br);

/* lumen's focus sheen on a tile: linear-gradient(115deg, #fff .36, .10 @28 %, 0 @46 %),
 * clipped to the tile's rounded rect. */
bool glass_sheen(lv_img_dsc_t *out, int w, int h, int r);

/* Multiply a BGRA image's alpha by a rounded-rect mask of radius r (antialiased edge). */
void glass_round_alpha(uint8_t *bgra, int w, int h, float r);

/* Darken (`a` = black alpha) every pixel of a BGRA buffer from row y0 to y1 with a linear
 * ramp from a0 at y0 to a1 at y1 -- the wallpaper scrim, baked once per wallpaper. */
void glass_scrim(uint8_t *bgra, int w, int y0, int y1, uint8_t a0, uint8_t a1);
