#include "glass.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BLUR_R 3        /* box radius at 1/8 scale; 3 passes ~ a 28px gaussian at full size */
#define BLUR_PASSES 3

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Signed distance from p (relative to the shape centre) to a rounded rect with half-size
 * (bx, by) and radius r; negative inside. Also returns the outward normal. */
static float rrect_sdf(float px, float py, float bx, float by, float r, float *nx, float *ny) {
    float sx = px < 0 ? -1.f : 1.f, sy = py < 0 ? -1.f : 1.f;
    float qx = fabsf(px) - (bx - r), qy = fabsf(py) - (by - r);
    if (qx > 0 && qy > 0) {
        float len = sqrtf(qx * qx + qy * qy);
        if (nx) { *nx = sx * qx / len; *ny = sy * qy / len; }
        return len - r;
    }
    if (nx) {
        if (qx > qy) { *nx = sx; *ny = 0; } else { *nx = 0; *ny = sy; }
    }
    return (qx > qy ? qx : qy) - r;
}

/* CSS linear-gradient parameter (0 at the start corner, 1 at the end) for angle `deg`. */
static void grad_setup(float deg, int w, int h, float *gx, float *gy) {
    float a = deg * (float)M_PI / 180.f;
    float sa = sinf(a), ca = cosf(a);
    float len = fabsf(w * sa) + fabsf(h * ca);
    *gx = sa / len;
    *gy = -ca / len;
}

static inline float grad3(float s, const uint8_t a[3], float mid) {
    s = clampf(s, 0.f, 1.f);
    if (s < mid) return (a[0] + (a[1] - a[0]) * (s / mid)) / 255.f;
    return (a[1] + (a[2] - a[1]) * ((s - mid) / (1.f - mid))) / 255.f;
}

static void box_blur(uint8_t *px, int w, int h, uint8_t *tmp) {
    const int d = 2 * BLUR_R + 1;
    for (int y = 0; y < h; y++) {           /* horizontal: px -> tmp */
        for (int c = 0; c < 3; c++) {
            int acc = 0;
            for (int k = -BLUR_R; k <= BLUR_R; k++) acc += px[(y * w + (k < 0 ? 0 : k)) * 4 + c];
            for (int x = 0; x < w; x++) {
                tmp[(y * w + x) * 4 + c] = (uint8_t)(acc / d);
                int xo = x - BLUR_R, xi = x + BLUR_R + 1;
                acc += px[(y * w + (xi >= w ? w - 1 : xi)) * 4 + c] - px[(y * w + (xo < 0 ? 0 : xo)) * 4 + c];
            }
        }
    }
    for (int x = 0; x < w; x++) {           /* vertical: tmp -> px */
        for (int c = 0; c < 3; c++) {
            int acc = 0;
            for (int k = -BLUR_R; k <= BLUR_R; k++) acc += tmp[((k < 0 ? 0 : k) * w + x) * 4 + c];
            for (int y = 0; y < h; y++) {
                px[(y * w + x) * 4 + c] = (uint8_t)(acc / d);
                int yo = y - BLUR_R, yi = y + BLUR_R + 1;
                acc += tmp[((yi >= h ? h - 1 : yi) * w + x) * 4 + c] - tmp[((yo < 0 ? 0 : yo) * w + x) * 4 + c];
            }
        }
    }
}

void glass_src_build(glass_src_t *out, const uint8_t *bgra, int w, int h) {
    int sw = w / GLASS_DIV, sh = h / GLASS_DIV;
    uint8_t *px = malloc((size_t)sw * sh * 4), *tmp = malloc((size_t)sw * sh * 4);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            unsigned s[3] = { 0, 0, 0 };
            for (int j = 0; j < GLASS_DIV; j++) {
                const uint8_t *row = bgra + ((size_t)(y * GLASS_DIV + j) * w + x * GLASS_DIV) * 4;
                for (int i = 0; i < GLASS_DIV; i++) { s[0] += row[i * 4]; s[1] += row[i * 4 + 1]; s[2] += row[i * 4 + 2]; }
            }
            uint8_t *o = px + (y * sw + x) * 4;
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(s[c] / (GLASS_DIV * GLASS_DIV));
            o[3] = 255;
        }
    for (int p = 0; p < BLUR_PASSES; p++) box_blur(px, sw, sh, tmp);
    free(tmp);
    out->px = px; out->w = sw; out->h = sh;
}

/* Bilinear sample of the blurred source at full-resolution screen coordinates. */
static inline void sample(const glass_src_t *s, float X, float Y, float rgb[3]) {
    float fx = clampf((X + 0.5f) / GLASS_DIV - 0.5f, 0.f, s->w - 1.001f);
    float fy = clampf((Y + 0.5f) / GLASS_DIV - 0.5f, 0.f, s->h - 1.001f);
    int x0 = (int)fx, y0 = (int)fy;
    float tx = fx - x0, ty = fy - y0;
    const uint8_t *a = s->px + (y0 * s->w + x0) * 4, *b = a + s->w * 4;
    for (int c = 0; c < 3; c++) {
        float top = a[c] + (a[4 + c] - a[c]) * tx, bot = b[c] + (b[4 + c] - b[c]) * tx;
        rgb[c] = top + (bot - top) * ty;
    }
}

static void dsc_init(lv_img_dsc_t *d, uint8_t *px, int w, int h) {
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d->header.w = w; d->header.h = h;
    d->data_size = (uint32_t)w * h * 4;
    d->data = px;
}

bool glass_render(glass_img_t *out, const glass_src_t *src, int x, int y, int w, int h, int r,
                  const glass_style_t *st) {
    int m = st->sh_opa ? st->sh_blur + abs(st->sh_y) : 1;
    int iw = w + 2 * m, ih = h + 2 * m;
    uint8_t *px = malloc((size_t)iw * ih * 4);
    if (!px) return false;
    float bx = w * 0.5f, by = h * 0.5f, cx = m + bx, cy = m + by;
    float gx, gy; grad_setup(st->sheen_deg, w, h, &gx, &gy);
    float sigma = st->sh_blur > 0 ? st->sh_blur * 0.5f : 1.f;
    float opa = st->opa / 255.f, dim = 1.f - st->dim / 255.f;

    for (int j = 0; j < ih; j++) {
        for (int i = 0; i < iw; i++) {
            float lx = i + 0.5f - cx, ly = j + 0.5f - cy, nx, ny;
            float d = rrect_sdf(lx, ly, bx, by, (float)r, &nx, &ny);
            float cov = clampf(0.5f - d, 0.f, 1.f);
            float sa = 0.f, rgb[3] = { 0, 0, 0 }, base_a = 1.f;
            if (cov > 0.f) {
                if (st->flat) {
                    base_a = (st->flat >> 24) / 255.f;
                    rgb[0] = st->flat & 0xff; rgb[1] = (st->flat >> 8) & 0xff; rgb[2] = (st->flat >> 16) & 0xff;
                } else sample(src, (float)(x + i - m), (float)(y + j - m), rgb);
                float t = -d;  /* depth inside the shape, px */
                float a = grad3(0.5f + lx * gx + ly * gy, st->sheen_a, st->sheen_mid);
                if (st->glow) a += (1.f - a) * (st->glow / 255.f) * clampf(1.f - t / st->glow_w, 0.f, 1.f);
                float band = clampf((2.f - t) / 1.5f, 0.f, 1.f);
                if (band > 0.f) {
                    float rw = (st->rim_t * fmaxf(0.f, -ny) + st->rim_l * fmaxf(0.f, -nx) +
                                st->rim_b * fmaxf(0.f, ny) + st->rim_r * fmaxf(0.f, nx)) / 255.f;
                    a += (1.f - a) * clampf(rw, 0.f, 1.f) * band;
                }
                /* white layers over the base; a translucent base lets the white carry its own alpha */
                float la = base_a + a * (1.f - base_a);
                for (int c = 0; c < 3; c++) rgb[c] = (rgb[c] * dim * base_a * (1.f - a) + 255.f * a) / (la > 0.f ? la : 1.f);
                sa = cov * opa * la;
            }
            float sh = 0.f;
            if (st->sh_opa && cov < 1.f) {
                float ds = rrect_sdf(lx, ly - st->sh_y, bx, by, (float)r, NULL, NULL);
                sh = (st->sh_opa / 255.f) * (1.f - cov) / (1.f + expf(1.7f * ds / sigma));
            }
            float oa = sa + sh * (1.f - sa);
            uint8_t *o = px + ((size_t)j * iw + i) * 4;
            if (oa <= 0.002f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            float k = sa / oa;  /* the shadow is black, so it only darkens through alpha */
            o[0] = (uint8_t)(rgb[0] * k); o[1] = (uint8_t)(rgb[1] * k); o[2] = (uint8_t)(rgb[2] * k);
            o[3] = (uint8_t)(oa * 255.f + 0.5f);
        }
    }
    dsc_init(&out->dsc, px, iw, ih);
    out->ox = m; out->oy = m;
    return true;
}

bool glass_rim(lv_img_dsc_t *out, int w, int h, int r, uint8_t rim_tl, uint8_t rim_br) {
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return false;
    float bx = w * 0.5f, by = h * 0.5f;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            float nx, ny, d = rrect_sdf(i + 0.5f - bx, j + 0.5f - by, bx, by, (float)r, &nx, &ny);
            float cov = clampf(0.5f - d, 0.f, 1.f), band = clampf((2.f + d) / 1.5f, 0.f, 1.f);
            float rw = (rim_tl * (fmaxf(0.f, -nx) + fmaxf(0.f, -ny)) +
                        rim_br * (fmaxf(0.f, nx) + fmaxf(0.f, ny))) / 255.f;
            uint8_t *o = px + ((size_t)j * w + i) * 4;
            o[0] = o[1] = o[2] = 255;
            o[3] = (uint8_t)(255.f * cov * band * clampf(rw, 0.f, 1.f));
        }
    dsc_init(out, px, w, h);
    return true;
}

bool glass_sheen(lv_img_dsc_t *out, int w, int h, int r) {
    static const uint8_t stops[3] = { 0x5c, 0x1a, 0x00 };
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return false;
    float bx = w * 0.5f, by = h * 0.5f, gx, gy;
    grad_setup(115.f, w, h, &gx, &gy);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            float lx = i + 0.5f - bx, ly = j + 0.5f - by;
            float cov = clampf(0.5f - rrect_sdf(lx, ly, bx, by, (float)r, NULL, NULL), 0.f, 1.f);
            float s = 0.5f + lx * gx + ly * gy;
            float a = s < 0.46f ? grad3(s / 0.46f, stops, 0.28f / 0.46f) : 0.f;
            uint8_t *o = px + ((size_t)j * w + i) * 4;
            o[0] = o[1] = o[2] = 255;
            o[3] = (uint8_t)(255.f * a * cov);
        }
    dsc_init(out, px, w, h);
    return true;
}

void glass_round_alpha(uint8_t *bgra, int w, int h, float r) {
    float bx = w * 0.5f, by = h * 0.5f;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            float cov = clampf(0.5f - rrect_sdf(i + 0.5f - bx, j + 0.5f - by, bx, by, r, NULL, NULL), 0.f, 1.f);
            if (cov < 1.f) bgra[((size_t)j * w + i) * 4 + 3] = (uint8_t)(bgra[((size_t)j * w + i) * 4 + 3] * cov);
        }
}

void glass_scrim(uint8_t *bgra, int w, int y0, int y1, uint8_t a0, uint8_t a1) {
    for (int y = y0; y < y1; y++) {
        int a = a0 + (a1 - a0) * (y - y0) / (y1 - y0 > 1 ? y1 - y0 - 1 : 1);
        int k = 255 - a;
        uint8_t *row = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w * 4; x += 4) {
            row[x] = (uint8_t)(row[x] * k / 255);
            row[x + 1] = (uint8_t)(row[x + 1] * k / 255);
            row[x + 2] = (uint8_t)(row[x + 2] * k / 255);
        }
    }
}
