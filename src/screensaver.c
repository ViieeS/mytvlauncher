#include "screensaver.h"
#include "lvgl/src/draw/sdl/lv_draw_sdl.h"
#include "../assets/fonts.h"
#include "../assets/icons.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1920
#define H 1080
static const uint32_t COLORS[8] = { 0xffffff, 0xff453a, 0xff9f0a, 0xffd60a, 0x30d158, 0x0a84ff, 0xbf5af2, 0xff6ca8 };
static const char *const FACE_NAMES[SB_FACES] = { "Digital", "Analog", "Clock and Calendar", "Solar", "Photos",
                                                   "Weather", "Fade", "Flow" };
#define NIGHT_RED 0xff2d1f        /* Gl */
#define SECOND_ORANGE 0xff9f0a
#define CAL_RED 0xff3b30

enum { MODE_NONE, MODE_STANDBY, MODE_PHOTOS };

static struct {
    lv_obj_t *root, *content, *tint, *hint;
    int mode, face, color;
    saver_env_t env;
    lv_timer_t *tick;
    lv_obj_t *time, *period, *sub;                      /* the face's clock text, refreshed by the tick */
    lv_obj_t *hand_h, *hand_m, *hand_s;                 /* analog hands: round-capped lines */
    lv_point_t hand_pt[3][2];
    int hand_cx, hand_cy;
    float hand_scale;
    lv_obj_t *flare;
    /* photo saver (standalone or the StandBy Photos face) */
    lv_obj_t *ph_img[2], *ph_block, *ph_time, *ph_period, *ph_wx, *ph_wx_icon, *ph_date;
    int ph_front, ph_last, ph_ahead;                    /* ph_ahead: the next picture, -1 until picked */
    uint32_t ph_next;
    int warm_f, warm_i;                                 /* saver_warm's cursor: font, glyph (then sprite) */
    uint32_t warm_from;                                 /* not during the .5 s fade-in: it would stutter */
} sv;

/* ---------------------------------------------------------------- sprites */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void dsc_init(lv_img_dsc_t *d, uint8_t *px, int w, int h) {
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d->header.w = w; d->header.h = h;
    d->data_size = (uint32_t) w * h * 4;
    d->data = px;
}

static uint8_t *white_buf(int w, int h) {
    uint8_t *px = calloc((size_t) w * h, 4);
    for (int i = 0; i < w * h; i++) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 255;
    return px;
}

/* A thick antialiased segment p0-p1 (width wd, round caps) into a white buffer's alpha, max-blended. */
static void capsule(uint8_t *px, int w, int h, float x0, float y0, float x1, float y1, float wd, float a) {
    float minx = fminf(x0, x1) - wd, maxx = fmaxf(x0, x1) + wd, miny = fminf(y0, y1) - wd, maxy = fmaxf(y0, y1) + wd;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = (int) fmaxf(0, miny); y < (int) fminf(h, maxy + 1); y++)
        for (int x = (int) fmaxf(0, minx); x < (int) fminf(w, maxx + 1); x++) {
            float px_ = x + .5f - x0, py_ = y + .5f - y0;
            float t = len2 > 0 ? clampf((px_ * dx + py_ * dy) / len2, 0, 1) : 0;
            float ex = px_ - t * dx, ey = py_ - t * dy;
            float cov = clampf(wd / 2 + .5f - sqrtf(ex * ex + ey * ey), 0, 1) * a;
            uint8_t *o = px + ((size_t) y * w + x) * 4 + 3;
            if (cov * 255 > *o) *o = (uint8_t) (cov * 255);
        }
}

/* The analog dial at a scale of `s` px per viewBox unit (lumen viewBox -100..100): 48 minor ticks
 * r 93-97 at .5, 12 hour ticks r 84-97 at .92. White; recoloured red at night. */
static lv_img_dsc_t *dial(float s) {
    static lv_img_dsc_t big, small;
    lv_img_dsc_t *d = s > 3.5f ? &big : &small;
    if (d->data) return d;
    int n = (int) (200 * s);
    uint8_t *px = white_buf(n, n);
    float c = n / 2.f;
    for (int i = 0; i < 60; i++) {
        float a = i * 6 * (float) M_PI / 180, sn = sinf(a), cs = cosf(a);
        bool hour = i % 5 == 0;
        float r0 = (hour ? 84 : 93) * s, r1 = 97 * s;
        capsule(px, n, n, c + r0 * sn, c - r0 * cs, c + r1 * sn, c - r1 * cs, (hour ? 1.8f : 1.f) * s, hour ? .92f : .5f);
    }
    dsc_init(d, px, n, n);
    return d;
}

/* A radial blob: white at the centre fading to nothing at 65 % of the radius (.sb__blob). */
static lv_img_dsc_t *blob(void) {
    static lv_img_dsc_t d;
    if (d.data) return &d;
    const int n = 256;
    uint8_t *px = white_buf(n, n);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float r = hypotf(x + .5f - n / 2.f, y + .5f - n / 2.f) / (n / 2.f);
            px[(y * n + x) * 4 + 3] = (uint8_t) (255 * clampf(1 - r / .65f, 0, 1));
        }
    dsc_init(&d, px, n, n);
    return &d;
}

/* The solar flare: two radial ellipses (70 %x55 % at 118 % down, 120 %x90 % at 135 %), centred
 * horizontally, drawn 2 screens wide at 1/8 scale so it can slide with the time of day. */
static lv_img_dsc_t *flare(void) {
    static lv_img_dsc_t d;
    if (d.data) return &d;
    const int w = 2 * W / 8, h = H / 8;
    uint8_t *px = white_buf(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float X = (x + .5f) * 8 - W, Y = (y + .5f) * 8;
            float d1 = hypotf(X / (.7f * W), (Y - 1.18f * H) / (.55f * H)), d2 = hypotf(X / (1.2f * W), (Y - 1.35f * H) / (.9f * H));
            float a1 = clampf(1 - d1 / .7f, 0, 1), a2 = clampf(1 - d2 / .62f, 0, 1);
            px[(y * w + x) * 4 + 3] = (uint8_t) (255 * (1 - (1 - a1) * (1 - a2)));
        }
    dsc_init(&d, px, w, h);
    return &d;
}

/* .ambient__shade: the bottom 440 px fading to #0000006b; 1/8 scale, black */
static lv_img_dsc_t *shade(void) {
    static lv_img_dsc_t d;
    if (d.data) return &d;
    const int w = W / 8, h = 440 / 8;
    uint8_t *px = calloc((size_t) w * h, 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) px[(y * w + x) * 4 + 3] = (uint8_t) (0x6b * y / (h - 1));
    dsc_init(&d, px, w, h);
    return &d;
}

/* ---------------------------------------------------------------- helpers */

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return o;
}
static lv_obj_t *text(lv_obj_t *p, const lv_font_t *f, uint32_t color, lv_opa_t opa, const char *s) {
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_label_set_text(l, s);
    return l;
}
static lv_obj_t *centered(lv_obj_t *p, const lv_font_t *f, uint32_t color, lv_opa_t opa, const char *s, int y) {
    lv_obj_t *l = text(p, f, color, opa, s);
    lv_obj_set_width(l, W);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, y);
    return l;
}
static lv_obj_t *sprite(lv_obj_t *p, const lv_img_dsc_t *d, uint32_t color, lv_opa_t opa) {
    lv_obj_t *i = lv_img_create(p);
    lv_img_set_src(i, d);
    lv_obj_set_style_img_recolor(i, lv_color_hex(color), 0);
    lv_obj_set_style_img_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_set_style_img_opa(i, opa, 0);
    return i;
}

static void a_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t) v, 0); }
static void a_zoom(void *o, int32_t v) { lv_img_set_zoom(o, (uint16_t) v); }
static void a_tx(void *o, int32_t v) { lv_obj_set_style_translate_x(o, (lv_coord_t) v, 0); }
static void a_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, (lv_coord_t) v, 0); }

/* lumen keyframes `alternate`: there and back forever, ease-in-out */
static void loop(void *o, lv_anim_exec_xcb_t cb, int32_t a, int32_t b, uint32_t ms, bool reverse) {
    if (sv.env.reduce_motion) return;
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, o);
    lv_anim_set_exec_cb(&an, cb);
    lv_anim_set_values(&an, reverse ? b : a, reverse ? a : b);
    lv_anim_set_time(&an, ms);
    lv_anim_set_playback_time(&an, ms);
    lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);
}

static bool night_now(void) {
    if (sv.env.night == 1) return true;
    if (sv.env.night == 2) return false;
    time_t now = time(NULL);
    int h = localtime(&now)->tm_hour;
    return h >= 19 || h < 6;
}

/* --sb: night red; on Solar the white choice becomes the warm #ffb020 */
static uint32_t sb_color(void) {
    if (night_now()) return NIGHT_RED;
    if (sv.face == SB_SOLAR && sv.color == 0) return 0xffb020;
    return COLORS[sv.color];
}

static void fmt_time(char *out, int n, bool pad_hour, bool want_period, char *period) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int h = t->tm_hour;
    if (sv.env.h12) {
        int h12 = h % 12 ? h % 12 : 12;
        snprintf(out, n, pad_hour ? "%02d:%02d" : "%d:%02d", h12, t->tm_min);
        if (period) snprintf(period, 4, "%s", want_period && sv.env.ampm ? (h < 12 ? "AM" : "PM") : "");
    } else {
        snprintf(out, n, "%02d:%02d", h, t->tm_min);
        if (period) period[0] = 0;
    }
}

/* "Monday, October 27" */
static void long_date(char *out, int n) {
    static const char *WD[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *MO[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                "September", "October", "November", "December" };
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(out, n, "%s, %s %d", WD[t->tm_wday], MO[t->tm_mon], t->tm_mday);
}

/* .sb__sub: the long date and, when there is one, the weather */
static void sub_text(char *out, int n) {
    char d[48];
    long_date(d, sizeof(d));
    if (sv.env.weather[0]) snprintf(out, n, "%s \xC2\xB7 %s", d, sv.env.weather);
    else snprintf(out, n, "%s", d);
}

/* ---------------------------------------------------------------- photo saver */

static const lv_img_dsc_t *weather_icon(const char *w) {
    char t[160];
    snprintf(t, sizeof(t), "%s", w);
    for (char *c = t; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    time_t now = time(NULL);
    int h = localtime(&now)->tm_hour;
    bool night = h >= 19 || h < 6;
    return strstr(t, "thunder") || strstr(t, "storm") ? &wx_storm
         : strstr(t, "snow") || strstr(t, "sleet") || strstr(t, "ice") ? &wx_snow
         : strstr(t, "rain") || strstr(t, "drizzle") || strstr(t, "shower") ? &wx_rain
         : strstr(t, "fog") || strstr(t, "mist") || strstr(t, "haze") ? &wx_fog
         : strstr(t, "partly") || strstr(t, "few clouds") || strstr(t, "scattered") ? (night ? &wx_partly_moon : &wx_partly_sun)
         : strstr(t, "cloud") || strstr(t, "overcast") ? &wx_cloud
         : night ? &wx_moon : &wx_sun;
}

/* Setting a label to the text it has still invalidates it, and the clock block may be zoomed
 * (a layer re-render), so the 1 Hz tick only touches what changed. */
static void set_text(lv_obj_t *l, const char *s) {
    if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static void photo_clock_update(void) {
    if (!sv.ph_time) return;
    char t[16], p[4];
    fmt_time(t, sizeof(t), false, true, p);
    set_text(sv.ph_time, t);
    set_text(sv.ph_period, p);
    if (sv.ph_wx) {
        set_text(sv.ph_wx, sv.env.weather);
        const lv_img_dsc_t *ic = weather_icon(sv.env.weather);
        if (lv_img_get_src(sv.ph_wx_icon) != ic) lv_img_set_src(sv.ph_wx_icon, ic);
        char d[48];
        long_date(d, sizeof(d));
        set_text(sv.ph_date, d);
    }
}

/* Next picture: crossfade 1.6 s ease-in-out, then a 36 s Ken Burns zoom 1 -> 1.07 around 38/48 %
 * (the other layer 62/44 %). Random, never the one just shown. */
static int photo_pick(void) {
    int i = rand() % sv.env.nphotos;
    if (sv.env.nphotos > 1 && i == sv.ph_last) i = (i + 1) % sv.env.nphotos;
    return i;
}
static void photo_next(bool first) {
    if (!sv.env.nphotos) return;
    int i = sv.ph_ahead >= 0 ? sv.ph_ahead : photo_pick();
    sv.ph_ahead = -1;
    sv.ph_last = i;
    int back = first ? sv.ph_front : 1 - sv.ph_front;
    lv_obj_t *img = sv.ph_img[back];
    lv_img_set_src(img, sv.env.photos[i]);
    lv_img_set_pivot(img, back ? 1190 : 730, back ? 475 : 518);
    lv_img_set_zoom(img, 256);
    lv_obj_move_foreground(img);
    lv_obj_move_to_index(img, 1);
    lv_anim_del(img, a_opa);
    lv_obj_set_style_opa(img, first ? LV_OPA_COVER : 0, 0);
    if (!first) {
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, img);
        lv_anim_set_exec_cb(&an, a_opa);
        lv_anim_set_values(&an, 0, LV_OPA_COVER);
        lv_anim_set_time(&an, sv.env.reduce_motion ? 0 : 1600);
        lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
        lv_anim_start(&an);
    }
    if (!sv.env.reduce_motion) {
        lv_anim_t z;
        lv_anim_init(&z);
        lv_anim_set_var(&z, img);
        lv_anim_set_exec_cb(&z, a_zoom);
        lv_anim_set_values(&z, 256, 274);
        lv_anim_set_time(&z, 36000);
        lv_anim_set_path_cb(&z, lv_anim_path_linear);
        lv_anim_start(&z);
    }
    sv.ph_front = back;
    sv.ph_next = lv_tick_get() + (uint32_t) sv.env.photo_seconds * 1000;
}

/* .ambient: two photo layers, bottom shade, optional dim and print look, the clock block. */
static void build_photos(lv_obj_t *p, bool embedded) {
    for (int i = 0; i < 2; i++) {
        sv.ph_img[i] = lv_img_create(p);
        lv_obj_set_style_opa(sv.ph_img[i], 0, 0);
    }
    sv.ph_front = 0;
    sv.ph_last = -1;
    photo_next(true);
    lv_obj_t *sh = sprite(p, shade(), 0, LV_OPA_COVER);
    lv_obj_set_style_img_recolor_opa(sh, 0, 0);
    lv_img_set_pivot(sh, 0, 0);
    lv_img_set_zoom(sh, 256 * 8);
    lv_obj_set_pos(sh, 0, H - 440);
    if (sv.env.print) {                 /* .ambient__print: warm paper, deeper blacks */
        lv_obj_t *a = box(p, 0, 0, W, H);
        lv_obj_set_style_bg_color(a, lv_color_hex(0xece4d4), 0);
        lv_obj_set_style_bg_opa(a, 0x17, 0);
        lv_obj_t *b = box(p, 0, 0, W, H);
        lv_obj_set_style_bg_color(b, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(b, 0x24, 0);
    }
    if (sv.env.dim) {                   /* .ambient__night .45 with the wallpaper dim on */
        lv_obj_t *d = box(p, 0, 0, W, H);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(d, 0x73, 0);
    }
    sv.ph_time = NULL;
    sv.ph_wx = NULL;
    if (sv.env.pc_pos == PC_OFF && !embedded) return;
    /* .ambient__block: 120px/650 time, 32px period, 28px weather + date; frosted = .58 white */
    static const int SIZE_ZOOM[5] = { 179, 256, 358, 486, 666 };   /* .7 1 1.4 1.9 2.6 */
    lv_obj_t *blk = box(p, 0, 0, 1000, 240);
    lv_obj_t *row = box(blk, 0, 0, 1000, 120);
    lv_opa_t to = sv.env.frosted ? 0x94 : LV_OPA_COVER;
    sv.ph_time = text(row, &sb_small, 0xffffff, to, "");
    sv.ph_period = text(row, &geist_32n, 0xffffff, sv.env.frosted ? 0xb3 : LV_OPA_COVER, "");
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    int y = lv_font_get_line_height(&sb_small) + 6;
    if (sv.env.info) {
        lv_obj_t *wr = box(blk, 0, y, 1000, 36);
        sv.ph_wx_icon = sprite(wr, &wx_cloud, 0xffffff, 0xe6);
        lv_obj_set_style_img_recolor_opa(sv.ph_wx_icon, 0, 0);
        lv_obj_set_pos(sv.ph_wx_icon, 0, 2);
        sv.ph_wx = text(wr, &geist_28m, 0xffffff, 0xe6, "");
        lv_obj_set_pos(sv.ph_wx, 35, 0);
        sv.ph_date = text(blk, &geist_28m, 0xffffff, 0xd9, "");
        lv_obj_set_pos(sv.ph_date, 0, y + 40);
    }
    int bh = y + (sv.env.info ? 80 : 0);
    lv_obj_set_height(blk, bh);
    int pos = embedded && sv.env.pc_pos == PC_OFF ? PC_BL : sv.env.pc_pos;
    bool top = pos == PC_TL || pos == PC_TC || pos == PC_TR, centre = pos == PC_BC || pos == PC_TC || pos == PC_C;
    bool right = pos == PC_BR || pos == PC_TR;
    int bx = right ? W - 90 - 1000 : centre ? (W - 1000) / 2 : 90;
    int by = pos == PC_C ? (H - bh) / 2 : top ? 70 : H - 64 - bh;
    lv_obj_set_pos(blk, bx, by);
    if (right || centre) {
        lv_obj_set_style_text_align(sv.ph_date ? sv.ph_date : sv.ph_time, right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(row, right ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_MID, 0, 0);
    }
    lv_obj_set_style_transform_pivot_x(blk, right ? 1000 : centre ? 500 : 0, 0);
    lv_obj_set_style_transform_pivot_y(blk, top ? 0 : bh, 0);
    lv_obj_set_style_transform_zoom(blk, SIZE_ZOOM[sv.env.pc_size % 5], 0);
    sv.ph_block = blk;
    photo_clock_update();
}

/* ---------------------------------------------------------------- StandBy faces */

/* hh:mm in --sb with an optional AM/PM; the sub line 24 px below; centred as one column */
static void clock_column(lv_obj_t *p, const lv_font_t *tf, uint32_t color, bool period, int dy) {
    char t[16], pr[4];
    fmt_time(t, sizeof(t), true, period, pr);
    int th = lv_font_get_line_height(tf), sh = lv_font_get_line_height(&sb_sub);
    int y = (H - (th + 24 + sh)) / 2 + dy;
    lv_obj_t *row = box(p, 0, y, W, th);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 20, 0);
    sv.time = text(row, tf, color, LV_OPA_COVER, t);
    sv.period = text(row, &sb_period, color, LV_OPA_COVER, pr);
    lv_obj_set_style_pad_bottom(sv.period, th / 6, 0);
    if (!pr[0]) lv_obj_add_flag(sv.period, LV_OBJ_FLAG_HIDDEN);
    char s[224];
    sub_text(s, sizeof(s));
    sv.sub = centered(p, &sb_sub, night_now() ? 0xff5a3c : 0xffffff, night_now() ? 0xb3 : 0xc7, s, y + th + 24);
}

/* The analog dial: ticks sprite, numerals 1-12 at r 68, three rotated hands, hub. */
static void build_dial(lv_obj_t *p, int cx, int cy, float s) {
    uint32_t c = night_now() ? sb_color() : 0xffffff;
    lv_img_dsc_t *d = dial(s);
    lv_obj_t *di = sprite(p, d, c, LV_OPA_COVER);
    lv_obj_set_pos(di, cx - d->header.w / 2, cy - d->header.h / 2);
    for (int n = 1; n <= 12; n++) {
        char b[4];
        snprintf(b, sizeof(b), "%d", n);
        float a = n * 30 * (float) M_PI / 180;
        lv_obj_t *l = text(p, &sb_numeral, c, LV_OPA_COVER, b);
        lv_obj_update_layout(l);
        lv_obj_set_pos(l, (int) (cx + 68 * s * sinf(a)) - lv_obj_get_width(l) / 2,
                       (int) (cy - 68 * s * cosf(a)) - lv_obj_get_height(l) / 2);
    }
    /* Lines, not rotated images: the SDL backend draws a rotated image in broken pieces. */
    static const float WD[3] = { 7.5f, 5.5f, 1.5f };
    lv_obj_t **hands[3] = { &sv.hand_h, &sv.hand_m, &sv.hand_s };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *h = lv_line_create(p);
        lv_obj_remove_style_all(h);
        int r = (int) (100 * s);                    /* the dial's box: a tick redraws only that */
        lv_obj_set_pos(h, cx - r, cy - r);
        lv_obj_set_size(h, 2 * r, 2 * r);
        lv_obj_set_style_line_width(h, (lv_coord_t) lroundf(WD[i] * s), 0);
        lv_obj_set_style_line_rounded(h, true, 0);
        lv_obj_set_style_line_color(h, lv_color_hex(i == 2 ? (night_now() ? sb_color() : SECOND_ORANGE) : c), 0);
        lv_obj_set_style_line_opa(h, LV_OPA_COVER, 0);
        lv_line_set_points(h, sv.hand_pt[i], 2);
        *hands[i] = h;
    }
    sv.hand_cx = sv.hand_cy = (int) (100 * s);        /* points are relative to the line's box */
    sv.hand_scale = s;
    lv_obj_t *hub = box(p, cx - (int) (5.5f * s), cy - (int) (5.5f * s), (int) (11 * s), (int) (11 * s));
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hub, lv_color_hex(night_now() ? sb_color() : SECOND_ORANGE), 0);
    lv_obj_set_style_border_width(hub, (int) (1.6f * s), 0);
}

static void hands_update(void) {
    if (!sv.hand_h) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    /* lumen viewBox units: each hand runs from `tail` behind the hub to `len` in front of it */
    static const float TAIL[3] = { 10, 12, 24 }, LEN[3] = { 46, 76, 84 };
    float deg[3] = { (t->tm_hour % 12) * 30 + t->tm_min * .5f, t->tm_min * 6 + t->tm_sec * .1f, t->tm_sec * 6.f };
    lv_obj_t *h[3] = { sv.hand_h, sv.hand_m, sv.hand_s };
    for (int i = 0; i < 3; i++) {
        float a = deg[i] * (float) M_PI / 180, dx = sinf(a) * sv.hand_scale, dy = -cosf(a) * sv.hand_scale;
        sv.hand_pt[i][0] = (lv_point_t) { (lv_coord_t) lroundf(sv.hand_cx - dx * TAIL[i]), (lv_coord_t) lroundf(sv.hand_cy - dy * TAIL[i]) };
        sv.hand_pt[i][1] = (lv_point_t) { (lv_coord_t) lroundf(sv.hand_cx + dx * LEN[i]), (lv_coord_t) lroundf(sv.hand_cy + dy * LEN[i]) };
        lv_line_set_points(h[i], sv.hand_pt[i], 2);
    }
}

/* .sb__cal: month in Apple red, narrow weekdays (week starts Sunday), today on a red circle */
static void build_calendar(lv_obj_t *p, int x0, int ytop) {
    static const char *MO[] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST",
                                "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };
    bool night = night_now();
    uint32_t accent = night ? sb_color() : CAL_RED;
    time_t now = time(NULL);
    struct tm t = *localtime(&now);
    lv_obj_set_pos(text(p, &sb_month, accent, LV_OPA_COVER, MO[t.tm_mon]), x0, ytop);
    int y = ytop + lv_font_get_line_height(&sb_month) + 26;
    const float cw = 760 / 7.f;
    static const char *WDN[7] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = text(p, &sb_wd, night ? accent : 0xffffff, night ? 0x8c : 0x9e, WDN[i]);
        lv_obj_set_width(l, (int) cw);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, x0 + (int) (i * cw), y + (64 - lv_font_get_line_height(&sb_wd)) / 2);
    }
    y += 64 + 14;
    struct tm first = t;
    first.tm_mday = 1;
    mktime(&first);
    static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int yr = t.tm_year + 1900, nd = DAYS[t.tm_mon] + (t.tm_mon == 1 && ((yr % 4 == 0 && yr % 100) || yr % 400 == 0));
    for (int d = 1; d <= nd; d++) {
        int cell = first.tm_wday + d - 1, col = cell % 7, row = cell / 7;
        int cx = x0 + (int) (col * cw + cw / 2), cy = y + row * (78 + 14) + 39;
        bool today = d == t.tm_mday, weekend = col == 0 || col == 6;
        if (today) {
            lv_obj_t *c = box(p, cx - 39, cy - 39, 78, 78);
            lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(c, lv_color_hex(accent), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        }
        char b[12];
        snprintf(b, sizeof(b), "%d", d);
        uint32_t col_c = today ? (night ? 0x000000 : 0xffffff) : night ? accent : 0xffffff;
        lv_opa_t op = today ? LV_OPA_COVER : weekend ? (night ? 0x8c : 0x80) : LV_OPA_COVER;
        lv_obj_t *l = text(p, &sb_day, col_c, op, b);
        lv_obj_set_width(l, 78);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, cx - 39, cy - lv_font_get_line_height(&sb_day) / 2);
    }
}

/* Fade / Flow: three 1500 px blobs drifting; a small clock in the corner */
static void build_blobs(lv_obj_t *p, bool flow) {
    static const struct { float x, y; uint32_t c; lv_opa_t a; uint32_t ms; bool rev; } FADE[3] = {
        { .25f, .30f, 0xff785a, 0xd9, 24000, false }, { .75f, .60f, 0x785aff, 0xd9, 24000, true },
        { .50f, 1.0f, 0xffbe5a, 0xb3, 30000, false } };
    static const struct { float x, y; uint32_t c; lv_opa_t a; uint32_t ms; bool rev; int ab; } FLOW[3] = {
        { .20f, .40f, 0x28c8c8, 0xcc, 36000, false, 0 }, { .80f, .30f, 0x3c78ff, 0xcc, 42000, false, 1 },
        { .50f, .90f, 0x78ffaa, 0x99, 48000, true, 0 } };
    for (int i = 0; i < 3; i++) {
        float x = flow ? FLOW[i].x : FADE[i].x, y = flow ? FLOW[i].y : FADE[i].y;
        lv_obj_t *b = sprite(p, blob(), flow ? FLOW[i].c : FADE[i].c, flow ? FLOW[i].a : FADE[i].a);
        lv_obj_set_pos(b, (int) (x * W) - 128, (int) (y * H) - 128);
        lv_img_set_zoom(b, 1500);           /* 256 px sprite drawn 1500 px wide */
        if (!flow) {                        /* sb-fade: opacity .1 -> 1, scale .85 -> 1.1 */
            loop(b, a_opa, 26, 255, FADE[i].ms, FADE[i].rev);
            loop(b, a_zoom, 1275, 1650, FADE[i].ms, FADE[i].rev);
        } else if (FLOW[i].ab == 0) {       /* sb-flow-a: (-300,120) .9 -> (400,-200) 1.15 */
            loop(b, a_tx, -300, 400, FLOW[i].ms, FLOW[i].rev);
            loop(b, a_ty, 120, -200, FLOW[i].ms, FLOW[i].rev);
            loop(b, a_zoom, 1350, 1725, FLOW[i].ms, FLOW[i].rev);
        } else {                            /* sb-flow-b: (260,-160) 1.1 -> (-420,220) .85 */
            loop(b, a_tx, 260, -420, FLOW[i].ms, false);
            loop(b, a_ty, -160, 220, FLOW[i].ms, false);
            loop(b, a_zoom, 1650, 1275, FLOW[i].ms, false);
        }
    }
    /* .sb__corner: left 100, bottom 80; 120px/650 time in --sb and the sub line */
    char t[16], s[224];
    fmt_time(t, sizeof(t), true, false, NULL);
    sub_text(s, sizeof(s));
    int sh = lv_font_get_line_height(&sb_sub), th = lv_font_get_line_height(&sb_small);
    sv.time = text(p, &sb_small, sb_color(), LV_OPA_COVER, t);
    lv_obj_set_pos(sv.time, 100, H - 80 - sh - 8 - th);
    sv.sub = text(p, &sb_sub, night_now() ? 0xff5a3c : 0xffffff, night_now() ? 0xb3 : 0xc7, s);
    lv_obj_set_pos(sv.sub, 100, H - 80 - sh);
}

static void flare_update(void) {
    if (!sv.flare) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    float f = clampf((t->tm_hour + t->tm_min / 60.f - 5) / 15, 0, 1);   /* 05:00 left edge .. 20:00 right */
    lv_obj_set_x(sv.flare, (int) (f * W) - W);
}

static void build_face(void) {
    if (sv.content) lv_obj_del(sv.content);
    sv.content = box(sv.root, 0, 0, W, H);
    lv_obj_move_to_index(sv.content, 0);
    sv.time = sv.period = sv.sub = sv.hand_h = sv.hand_m = sv.hand_s = sv.flare = NULL;
    sv.ph_time = sv.ph_wx = NULL;
    lv_obj_t *p = sv.content;
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    uint32_t c = sb_color();
    switch (sv.face) {
        case SB_DIGITAL: clock_column(p, &sb_time, c, true, 0); break;
        case SB_ANALOG: build_dial(p, W / 2, H / 2, 3.8f); hands_update(); break;
        case SB_CALENDAR: {                 /* dial 620 + gap 160 + calendar 760, centred as a row */
            int x0 = (W - (620 + 160 + 760)) / 2;
            build_dial(p, x0 + 310, H / 2, 3.1f);
            hands_update();
            build_calendar(p, x0 + 620 + 160, H / 2 - 330);
            break;
        }
        case SB_SOLAR: {
            sv.flare = sprite(p, flare(), c, night_now() ? 0x66 : 0xb3);
            lv_img_set_pivot(sv.flare, 0, 0);
            lv_img_set_zoom(sv.flare, 256 * 8);
            flare_update();
            clock_column(p, &sb_time_light, 0xffffff, false, 0);
            break;
        }
        case SB_PHOTOS: build_photos(p, true); break;
        case SB_WEATHER: {                  /* gradient sky; place, big temperature, condition */
            lv_obj_set_style_bg_color(p, lv_color_hex(0x10224a), 0);
            lv_obj_set_style_bg_grad_color(p, lv_color_hex(0x2a1d5a), 0);
            lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0);
            char place[96] = "", temp[16] = "\xE2\x80\x94", cond[64] = "";
            const char *w = sv.env.weather, *dot = strstr(w, " \xC2\xB7 ");
            if (dot) { snprintf(place, sizeof(place), "%.*s", (int) (dot - w), w); w = dot + 4; }
            int deg;
            if (sscanf(w, "%d", &deg) == 1) {
                snprintf(temp, sizeof(temp), "%d\xC2\xB0", deg);
                const char *sp = strstr(w, "\xC2\xB0");
                if (sp) snprintf(cond, sizeof(cond), "%s", sp[2] == ' ' ? sp + 3 : sp + 2);
            }
            int lp = lv_font_get_line_height(&sb_place), lt = lv_font_get_line_height(&sb_temp),
                lc = lv_font_get_line_height(&sb_cond), ls = lv_font_get_line_height(&sb_sub);
            int y = (H - (lp + lt + lc + 24 + ls)) / 2;
            centered(p, &sb_place, 0xffffff, 0xcc, place, y);
            centered(p, &sb_temp, c, LV_OPA_COVER, temp, y + lp);
            centered(p, &sb_cond, 0xffffff, LV_OPA_COVER, cond, y + lp + lt);
            char t[16], d[48], s[80];
            fmt_time(t, sizeof(t), true, false, NULL);
            long_date(d, sizeof(d));
            snprintf(s, sizeof(s), "%s \xC2\xB7 %s", d, t);
            sv.sub = centered(p, &sb_sub, night_now() ? 0xff5a3c : 0xffffff, night_now() ? 0xb3 : 0xc7, s, y + lp + lt + lc + 24);
            break;
        }
        case SB_FADE:
            lv_obj_set_style_bg_color(p, lv_color_hex(0x1a1030), 0);
            build_blobs(p, false);
            break;
        case SB_FLOW:
            lv_obj_set_style_bg_color(p, lv_color_hex(0x061a2a), 0);
            build_blobs(p, true);
            break;
    }
    /* .sb__night: rgba(40,0,0,.55); on the Photos face rgba(60,0,0,.6) */
    if (night_now()) {
        lv_obj_set_style_bg_color(sv.tint, lv_color_hex(sv.face == SB_PHOTOS ? 0x3c0000 : 0x280000), 0);
        lv_obj_set_style_bg_opa(sv.tint, sv.face == SB_PHOTOS ? 0x99 : 0x8c, 0);
    } else lv_obj_set_style_bg_opa(sv.tint, 0, 0);
}

/* ---------------------------------------------------------------- shell */

static void tick_cb(lv_timer_t *t) {
    (void) t;
    hands_update();
    if (sv.mode == MODE_PHOTOS || sv.face == SB_PHOTOS) {
        photo_clock_update();
        if ((int32_t) (lv_tick_get() - sv.ph_next) >= 0) photo_next(false);
        return;
    }
    time_t now = time(NULL);
    if (localtime(&now)->tm_sec != 0) return;            /* text changes once a minute */
    if (sv.face == SB_ANALOG) return;
    if (sv.face == SB_FADE || sv.face == SB_FLOW) {      /* a rebuild would restart the slow drift */
        char tm[16], s[224];
        fmt_time(tm, sizeof(tm), true, false, NULL);
        sub_text(s, sizeof(s));
        lv_label_set_text(sv.time, tm);
        lv_label_set_text(sv.sub, s);
        return;
    }
    build_face();                                        /* static faces: the minute's text, Solar's flare */
}

static lv_obj_t *root_create(void) {
    lv_obj_t *r = box(lv_layer_top(), 0, 0, W, H);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(r, 0, 0);
    lv_anim_t an;                                        /* sb-in .5s ease-out */
    lv_anim_init(&an);
    lv_anim_set_var(&an, r);
    lv_anim_set_exec_cb(&an, a_opa);
    lv_anim_set_values(&an, 0, LV_OPA_COVER);
    lv_anim_set_time(&an, sv.env.reduce_motion ? 0 : 500);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
    lv_anim_start(&an);
    sv.warm_from = lv_tick_get() + 1000;
    return r;
}

void standby_open(const saver_env_t *env, int face, int color) {
    saver_close();
    sv.env = *env;
    sv.face = face < 0 || face >= SB_FACES ? SB_DIGITAL : face;
    sv.color = color & 7;
    sv.mode = MODE_STANDBY;
    sv.ph_ahead = -1;
    sv.root = root_create();
    sv.tint = box(sv.root, 0, 0, W, H);
    /* .sb__hint: shown once, white .55 for 70 % of 6 s, then fades out */
    char h[96];
    snprintf(h, sizeof(h), "%s \xC2\xB7 \xE2\x97\x80 \xE2\x96\xB6 face \xC2\xB7 \xE2\x96\xB2 \xE2\x96\xBC colour \xC2\xB7 OK exit", FACE_NAMES[sv.face]);
    sv.hint = centered(sv.root, &geist_22m, 0xffffff, 0x8c, h, H - 28 - lv_font_get_line_height(&geist_22m));
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, sv.hint);
    lv_anim_set_exec_cb(&an, a_opa);
    lv_anim_set_values(&an, LV_OPA_COVER, 0);
    lv_anim_set_delay(&an, 4200);
    lv_anim_set_time(&an, 1800);
    lv_anim_start(&an);
    sv.content = NULL;
    build_face();
    sv.tick = lv_timer_create(tick_cb, 1000, NULL);
}

void photos_open(const saver_env_t *env) {
    saver_close();
    sv.env = *env;
    sv.mode = MODE_PHOTOS;
    sv.face = SB_PHOTOS;
    sv.ph_ahead = -1;
    sv.root = root_create();
    sv.content = box(sv.root, 0, 0, W, H);
    build_photos(sv.content, false);
    sv.tick = lv_timer_create(tick_cb, 1000, NULL);
}

/* Fonts whose glyphs are big enough for their first upload to show (460 px digits: a face not
 * seen yet stalled 40-60 ms). The text fonts' glyphs are small and come as they are drawn. */
static const lv_font_t *const WARM_FONTS[] = { &sb_time, &sb_time_light, &sb_temp, &sb_small, &sb_period,
                                               &sb_numeral, &sb_month, &sb_day, &sb_wd };

int saver_warm(void) {
    if (sv.mode == MODE_NONE || (int32_t) (lv_tick_get() - sv.warm_from) < 0) return 0;
    lv_draw_ctx_t *dc = lv_disp_get_default()->driver->draw_ctx;
    if (sv.mode == MODE_STANDBY)
        for (; sv.warm_f < (int) (sizeof(WARM_FONTS) / sizeof(*WARM_FONTS)); sv.warm_f++, sv.warm_i = 0) {
            const lv_font_t *f = WARM_FONTS[sv.warm_f];
            const lv_font_fmt_txt_cmap_t *m = &((const lv_font_fmt_txt_dsc_t *) f->dsc)->cmaps[0];   /* make_fonts.py: one sparse cmap */
            while (sv.warm_i < m->list_length)
                if (lv_draw_sdl_letter_warm(dc, f, m->range_start + m->unicode_list[sv.warm_i++])) return 1;
        }
    if (sv.mode == MODE_STANDBY && sv.warm_f == (int) (sizeof(WARM_FONTS) / sizeof(*WARM_FONTS))) {
        /* the faces' sprites: built on first use (the dial is CPU work too), then uploaded */
        lv_img_dsc_t *(*const make[])(void) = { blob, flare, shade };
        while (sv.warm_i < 5) {
            int i = sv.warm_i++;
            if (lv_draw_sdl_img_warm(dc, i < 2 ? dial(i ? 3.1f : 3.8f) : make[i - 2]())) return 1;
        }
    }
    /* the photo saver's next picture, or the first one of StandBy's Photos face */
    if (!sv.env.nphotos) return 0;
    if (sv.ph_ahead < 0) sv.ph_ahead = photo_pick();
    return lv_draw_sdl_img_warm(dc, sv.env.photos[sv.ph_ahead]);
}

bool saver_active(void) { return sv.mode != MODE_NONE; }
int standby_face(void) { return sv.face; }
int standby_color(void) { return sv.color; }

void saver_set_weather(const char *t) {
    snprintf(sv.env.weather, sizeof(sv.env.weather), "%s", t);
    if (sv.mode == MODE_STANDBY && (sv.face == SB_WEATHER || sv.face == SB_DIGITAL || sv.face == SB_SOLAR)) build_face();
}

void saver_close(void) {
    if (sv.mode == MODE_NONE) return;
    if (sv.tick) lv_timer_del(sv.tick);
    lv_obj_del(sv.root);
    memset(&sv, 0, sizeof(sv));
}

bool saver_key(int key) {
    if (sv.mode == MODE_STANDBY && key != SV_OTHER) {
        /* Left/Right change the face (wrapping), Down/Up the colour */
        if (key == SV_LEFT || key == SV_RIGHT) sv.face = (sv.face + (key == SV_RIGHT ? 1 : SB_FACES - 1)) % SB_FACES;
        else sv.color = (sv.color + (key == SV_DOWN ? 1 : 7)) % 8;
        build_face();
        return true;
    }
    saver_close();
    return false;
}
