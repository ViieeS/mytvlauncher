/* Native webOS launcher: SDL2 surface + LVGL UI, a pixel-level copy of the lumen home screen
 * (github.com/ammar027/lumen-webos). Design tokens, layout and behaviour follow lumen's CSS and
 * key handling; none of its code is reused.
 *
 * Performance model (C1 GPU is weak, CPU is fine): every glass surface, icon and scrim is
 * pre-composited once into a static texture (see glass.c), animations only touch transform
 * and opacity, positions are absolute (no flex layout at runtime), and focus is our own model
 * rather than LVGL groups/indevs. Apps and settings go over the Luna bus via libhelpers; calls
 * the developer uid may not make are retried as root through the Homebrew Channel. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/sdl/lv_draw_sdl.h"
#include "../third_party/cJSON.h"
#include "../assets/fonts.h"
#include "../assets/icons.h"
#include "../assets/cities.h"
#include "glass.h"
#include "screensaver.h"
#include "lvgl/src/extra/libs/png/lodepng.h"
#include <webos-helpers/libhelpers.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *lg;
#define LOG(...) do { if (lg) { fprintf(lg, __VA_ARGS__); fflush(lg); } } while (0)

#define SELF_ID "com.viiees.mytvlauncher"
#define APP_DIR "/media/developer/apps/usr/palm/applications/" SELF_ID
/* Settings and icon copies live outside the app dir: an update replaces that dir whole. */
#define DATA_DIR "/media/developer/" SELF_ID "-data"
#define STATE_PATH DATA_DIR "/state.txt"

/* ---- lumen tokens (theme objects `se`, `Un`, `qn` and the final CSS cascade) ---- */
#define SCR_W 1920
#define SCR_H 1080
#define EDGE 140                  /* --layout-edge-padding */
#define TILE_W 236                /* se.tileSize / tileHeight / tileRadius / tileGap */
#define TILE_H 142
#define TILE_R 34
#define TILE_GAP 28
#define TILE_STEP (TILE_W + TILE_GAP)
#define ROW_VIEW_W (SCR_W - 2 * EDGE)   /* It: a row scrolls once it is wider than this */
#define ROWS_TOP 846              /* se.rowsTop: top of the first row's tiles */
#define ROW_PITCH 264             /* tile + label block (26 + 19*2.4) + rowGap 40 + 10 */
#define LOWER_SHIFT 300           /* .rows--lower */
static const int FOCUS_ZOOMS[3] = { 277, 297, 323 };   /* focusStrength subtle 1.08 / normal 1.16 / strong 1.26 */
#define FOCUS_LIFT 6
#define FOCUS_MS 160              /* Un.focusMs */
#define UNFOCUSED_OPA 153         /* Un.unfocusedRowOpacity .6 */
#define LONG_PRESS_MS 600         /* rt.longPressMs */
#define KEY_REPEAT_MS 90
#define TOAST_MS 2200
#define RECENT_MAX 8
#define PANEL_TILE_W 200          /* .panel: tile 200x120, radius 24, gap 24, 3 per row */
#define PANEL_TILE_H 120
#define PANEL_TILE_R 24
#define PILL 58
#define PILL_Y 64
#define PILL_ZOOM 292             /* .pill--focused scale(1.14) */
#define WP_COUNT 5                /* bundled pictures (lumen's five defaults) */
#define WP_PLAIN WP_COUNT         /* "Plain background": lumen's .screen gradient, generated */
#define WP_N (WP_COUNT + 1)
#define WP_FADE_MS 700
#define IDLE_HIDE_MS 8000         /* dockHide "idle" */
#define C_TEXT 0xEEEEEE           /* qn.textPrimary */
#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))
/* Control Center 1.1.4: --cc-u, --cc-gap, top 150, right edge */
#define CC_U 90
#define CC_GAP 18
#define CC_W (4 * CC_U + 3 * CC_GAP)
#define CC_X (SCR_W - EDGE - CC_W)
#define CC_Y 150
#define CC_ON 0x40c4ff                      /* --cc-on: rgb(64, 196, 255) */
#define CC_OPT_H 72
#define CC_OPT_STEP 84

/* ================================================================ app model */

typedef struct {
    char *id, *title, *icon;      /* icon: the launch point's largeIcon (130 px) when it has one */
    const lv_img_dsc_t *img;      /* icon scaled to the tile and rounded 16 % (lumen tile__image--icon) */
    bool has_img;
    lv_color_t color;             /* brand colour sampled from the icon's border ring */
    bool has_color;
} app_t;

#define MAX_APPS 96
#define NFOLDERS 3
static const char *FOLDER_NAMES[NFOLDERS] = { "Games", "Tools", "Media" };   /* lumen Eo */

static app_t apps[MAX_APPS];
static int napps;
static int order[MAX_APPS], norder;          /* every app, user order */
static bool hidden[MAX_APPS];                /* removed from Home (Settings > Apps adds back) */
static int folder_of[MAX_APPS];              /* -1 or folder index */
static int folder_items[NFOLDERS][MAX_APPS], folder_n[NFOLDERS];
static int recent[RECENT_MAX], nrecent;
static int clock_fmt;                        /* 0 auto (the TV's own setting), 1 12h, 2 24h */
static bool sys_12h;
static char src_hidden[16][48];              /* input appIds hidden in Settings > Sources */
static int nsrc_hidden;

/* Settings > Options and Wallpaper. Only what the TV's own settings don't already cover. */
static struct {
    bool wp_plain;              /* Wallpaper type: Plain instead of Pictures */
    int picks;                  /* bitmask of the bundled pictures ticked in Choose wallpaper */
    bool wp_new;                /* New picture on startup (and on every return home) */
    int wp_dim;                 /* wallpaperDim: 0 off, 1 at night (19:00-06:00), 2 always */
    int wp_auto_min;            /* 0, 15, 30, 60: a new pick on a timer (with more than one pick) */
    bool show_weather, show_ampm;
    int dock_hide;              /* 0 off, 1 Down arrow, 2 after idle, 3 both */
    char country[4], city[48];  /* "" = locate by IP */
    int apps_row;               /* appsPerRow 4/5/6: tiles in the tray row (and per More apps row) */
    bool hide_names, dock_dim, show_recents, show_date, sys_apps, reduce_motion;
    int focus;                  /* focusStrength: 0 subtle 1.08, 1 normal 1.16, 2 strong 1.26 */
    int ss_type;                /* Screensaver type: 0 Lumen Gallery or LG (the TV's own), 1 Photos in Lumen, 2 StandBy */
    int idle_min;               /* Screensaver after idle: 0, 5, 10, 20, 30 */
    int ph_bright;              /* photoBrightness: 0 as the TV is set, 1 dim (backlight 30), 2 very dim (10) */
    int pc_pos, pc_size;        /* photo clock: PC_*, 0 small .. 4 huge */
    bool pc_info, pc_solid, ph_print;
    int ph_secs;
    int sb_face;                /* StandBy face: 0 last used, else SB_* + 1 */
    int sb_color, sb_night, sb_last;
    int clock_style;            /* clockStyle: CS_* */
    int clock_color;            /* 0 auto, then CLOCK_COLORS */
    bool clock_frosted, clock_stacked, clock_below;   /* look, layout, Stacked order (time below) */
    int clock_size, clock_weight, clock_pos;           /* 0-4 (1 normal), 0-2 (1 regular), 0 left 1 centre 2 right */
} prefs = { false, (1 << WP_COUNT) - 1, true, 0, 0, true, true, 1, "", "", 6, false, false, true, true, false, false, 1, 0, 10, 0, PC_BL, 1, true, false, false, 30, 0, 0, 0, SB_DIGITAL, 0, 0, false, false, false, 1, 1, 0 };

typedef struct { char app_id[48]; char label[48]; } input_t;
static input_t inputs[12];
static int ninputs;

static int app_by_id(const char *id) {
    for (int i = 0; i < napps; i++) if (!strcmp(apps[i].id, id)) return i;
    return -1;
}

static void state_save(void) {
    FILE *f = fopen(STATE_PATH, "w");
    if (!f) { LOG("state_save: cannot write %s\n", STATE_PATH); return; }
    for (int i = 0; i < norder; i++) fprintf(f, "order %s\n", apps[order[i]].id);
    for (int i = 0; i < napps; i++) if (hidden[i]) fprintf(f, "hide %s\n", apps[i].id);
    for (int k = 0; k < NFOLDERS; k++)
        for (int i = 0; i < folder_n[k]; i++) fprintf(f, "folder %d %s\n", k, apps[folder_items[k][i]].id);
    for (int i = 0; i < nrecent; i++) fprintf(f, "recent %s\n", apps[recent[i]].id);
    for (int i = 0; i < nsrc_hidden; i++) fprintf(f, "srchide %s\n", src_hidden[i]);
    fprintf(f, "clock %d\nwpplain %d\npicks %d\nwpnew %d\nwpdim %d\nwpauto %d\nweather %d\nampm %d\ndock %d\n",
            clock_fmt, prefs.wp_plain, prefs.picks, prefs.wp_new, prefs.wp_dim, prefs.wp_auto_min, prefs.show_weather,
            prefs.show_ampm, prefs.dock_hide);
    if (prefs.country[0]) fprintf(f, "country %s\n", prefs.country);
    if (prefs.city[0]) fprintf(f, "city %s\n", prefs.city);
    fprintf(f, "appsrow %d\nhidenames %d\ndockdim %d\nrecents %d\nshowdate %d\nsysapps %d\nmotion %d\nfocus %d\n",
            prefs.apps_row, prefs.hide_names, prefs.dock_dim, prefs.show_recents, prefs.show_date,
            prefs.sys_apps, prefs.reduce_motion, prefs.focus);
    fprintf(f, "sstype %d\nidle %d\nphbright %d\npcpos %d\npcsize %d\npcinfo %d\npcsolid %d\nphprint %d\nphsecs %d\nsbface %d\nsbcolor %d\nsbnight %d\nsblast %d\n",
            prefs.ss_type, prefs.idle_min, prefs.ph_bright, prefs.pc_pos, prefs.pc_size, prefs.pc_info, prefs.pc_solid,
            prefs.ph_print, prefs.ph_secs, prefs.sb_face, prefs.sb_color, prefs.sb_night, prefs.sb_last);
    fprintf(f, "clkstyle %d\nclkcolor %d\nclkfrost %d\nclkstack %d\nclkbelow %d\nclksize %d\nclkweight %d\nclkpos %d\n",
            prefs.clock_style, prefs.clock_color, prefs.clock_frosted, prefs.clock_stacked, prefs.clock_below,
            prefs.clock_size, prefs.clock_weight, prefs.clock_pos);
    fclose(f);
}

/* Preferences are needed before the app list arrives (the wallpaper is the first thing drawn),
 * so they are read in their own pass at start-up. */
static void prefs_load(void) {
    FILE *f = fopen(STATE_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *arg = strchr(line, ' ');
        if (!arg) continue;
        *arg++ = 0;
        int v = atoi(arg);
        if (!strcmp(line, "clock")) clock_fmt = v % 3;
        else if (!strcmp(line, "wallpaper")) {          /* 1.0.9-era key: -1 shuffle, 5 plain, n one picture */
            prefs.wp_plain = v == WP_PLAIN;
            prefs.picks = v >= 0 && v < WP_COUNT ? 1 << v : (1 << WP_COUNT) - 1;
        }
        else if (!strcmp(line, "wpplain")) prefs.wp_plain = v;
        else if (!strcmp(line, "picks")) prefs.picks = v & ((1 << WP_COUNT) - 1) ? v & ((1 << WP_COUNT) - 1) : 1;
        else if (!strcmp(line, "wpnew")) prefs.wp_new = v;
        else if (!strcmp(line, "wpdim")) prefs.wp_dim = v % 3;
        else if (!strcmp(line, "night")) prefs.wp_dim = v ? 1 : 0;
        else if (!strcmp(line, "wpauto")) prefs.wp_auto_min = v;
        else if (!strcmp(line, "weather")) prefs.show_weather = v;
        else if (!strcmp(line, "ampm")) prefs.show_ampm = v;
        else if (!strcmp(line, "dock")) prefs.dock_hide = v & 3;
        else if (!strcmp(line, "country")) snprintf(prefs.country, sizeof(prefs.country), "%s", arg);
        else if (!strcmp(line, "city")) snprintf(prefs.city, sizeof(prefs.city), "%s", arg);
        else if (!strcmp(line, "appsrow")) prefs.apps_row = v >= 4 && v <= 6 ? v : 6;
        else if (!strcmp(line, "hidenames")) prefs.hide_names = v;
        else if (!strcmp(line, "dockdim")) prefs.dock_dim = v;
        else if (!strcmp(line, "recents")) prefs.show_recents = v;
        else if (!strcmp(line, "showdate")) prefs.show_date = v;
        else if (!strcmp(line, "sysapps")) prefs.sys_apps = v;
        else if (!strcmp(line, "motion")) prefs.reduce_motion = v;
        else if (!strcmp(line, "focus")) prefs.focus = v >= 0 && v <= 2 ? v : 1;
        else if (!strcmp(line, "sstype")) prefs.ss_type = v >= 0 && v <= 2 ? v : 0;
        else if (!strcmp(line, "idle")) prefs.idle_min = v == 5 || v == 10 || v == 20 || v == 30 ? v : 0;
        else if (!strcmp(line, "phbright")) prefs.ph_bright = v >= 0 && v <= 2 ? v : 0;
        else if (!strcmp(line, "pcpos")) prefs.pc_pos = v >= PC_OFF && v <= PC_C ? v : PC_BL;
        else if (!strcmp(line, "pcsize")) prefs.pc_size = v >= 0 && v <= 4 ? v : 1;
        else if (!strcmp(line, "pcinfo")) prefs.pc_info = v;
        else if (!strcmp(line, "pcsolid")) prefs.pc_solid = v;
        else if (!strcmp(line, "phprint")) prefs.ph_print = v;
        else if (!strcmp(line, "phsecs")) prefs.ph_secs = v >= 10 && v <= 300 ? v : 30;
        else if (!strcmp(line, "sbface")) prefs.sb_face = v >= 0 && v <= SB_FACES ? v : 0;
        else if (!strcmp(line, "sbcolor")) prefs.sb_color = v & 7;
        else if (!strcmp(line, "sbnight")) prefs.sb_night = v >= 0 && v <= 2 ? v : 0;
        else if (!strcmp(line, "sblast")) prefs.sb_last = v >= 0 && v < SB_FACES ? v : SB_DIGITAL;
        else if (!strcmp(line, "clkstyle")) prefs.clock_style = v >= 0 && v <= 4 ? v : 0;
        else if (!strcmp(line, "clkcolor")) prefs.clock_color = v >= 0 && v <= 6 ? v : 0;
        else if (!strcmp(line, "clkfrost")) prefs.clock_frosted = v;
        else if (!strcmp(line, "clkstack")) prefs.clock_stacked = v;
        else if (!strcmp(line, "clkbelow")) prefs.clock_below = v;
        else if (!strcmp(line, "clksize")) prefs.clock_size = v >= 0 && v <= 4 ? v : 1;
        else if (!strcmp(line, "clkweight")) prefs.clock_weight = v >= 0 && v <= 2 ? v : 1;
        else if (!strcmp(line, "clkpos")) prefs.clock_pos = v >= 0 && v <= 2 ? v : 0;
    }
    fclose(f);
}

/* Apps the file does not know (newly installed) land at the end of Home, visible. The older
 * order.txt (one id per line) is read as a plain order list. */
static void state_load(void) {
    /* the app list can be reloaded (after an uninstall): indices change, so start clean */
    memset(hidden, 0, sizeof(hidden));
    memset(folder_n, 0, sizeof(folder_n));
    nrecent = nsrc_hidden = 0;
    for (int i = 0; i < napps; i++) folder_of[i] = -1;
    bool placed[MAX_APPS] = { 0 };
    norder = 0;
    FILE *f = fopen(STATE_PATH, "r");
    bool legacy = false;
    if (!f) { f = fopen(APP_DIR "/order.txt", "r"); legacy = true; }
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *arg = strchr(line, ' ');
            if (legacy) { int a = app_by_id(line); if (a >= 0 && !placed[a]) { order[norder++] = a; placed[a] = true; } continue; }
            if (!arg) continue;
            *arg++ = 0;
            if (!strcmp(line, "order")) { int a = app_by_id(arg); if (a >= 0 && !placed[a]) { order[norder++] = a; placed[a] = true; } }
            else if (!strcmp(line, "hide")) { int a = app_by_id(arg); if (a >= 0) hidden[a] = true; }
            else if (!strcmp(line, "folder")) {
                int k = atoi(arg); char *id = strchr(arg, ' ');
                int a = id ? app_by_id(id + 1) : -1;
                if (a >= 0 && k >= 0 && k < NFOLDERS && folder_of[a] < 0) { folder_of[a] = k; folder_items[k][folder_n[k]++] = a; }
            } else if (!strcmp(line, "recent")) { int a = app_by_id(arg); if (a >= 0 && nrecent < RECENT_MAX) recent[nrecent++] = a; }
            else if (!strcmp(line, "srchide") && nsrc_hidden < 16) snprintf(src_hidden[nsrc_hidden++], 48, "%s", arg);
        }
        fclose(f);
    }
    for (int i = 0; i < napps; i++) if (!placed[i]) order[norder++] = i;
}

/* lumen's U list: Home apps (not hidden, not in a folder) in order, then the folder tiles. */
enum { IT_APP, IT_FOLDER, IT_INPUT };
typedef struct { int kind, ref; } ref_t;

static int home_list(ref_t *out) {
    int n = 0;
    for (int i = 0; i < norder; i++) {
        int a = order[i];
        if (!hidden[a] && folder_of[a] < 0) out[n++] = (ref_t) { IT_APP, a };
    }
    for (int k = 0; k < NFOLDERS; k++) if (folder_n[k]) out[n++] = (ref_t) { IT_FOLDER, k };
    return n;
}

static bool src_is_hidden(const char *app_id) {
    for (int i = 0; i < nsrc_hidden; i++) if (!strcmp(src_hidden[i], app_id)) return true;
    return false;
}

/* ================================================================ Luna */

typedef enum { RQ_APPS, RQ_PIC, RQ_SND, RQ_SLEEP, RQ_INPUTS,
               RQ_TIMEFMT, RQ_RUNNING, RQ_UNINSTALL, RQ_ACT, RQ_ICONS, RQ_HOMEHOOK, RQ_N } rq_t;

typedef struct { rq_t kind; bool root; char *json; } reply_t;
static pthread_mutex_t rq_mtx = PTHREAD_MUTEX_INITIALIZER;
static reply_t rq_queue[64];
static int rq_len;
static HContext rq_ctx[RQ_N][2];
static char rq_uri[RQ_N][128], rq_payload[RQ_N][1000], rq_fail[RQ_N][96];

/* The main loop sleeps in SDL_WaitEventTimeout; results from other threads wake it. */
static void main_wake(void) {
    SDL_Event e = { .type = SDL_USEREVENT };
    SDL_PushEvent(&e);
}

static bool rq_push(rq_t k, bool root, LSMessage *m) {
    const char *msg = HLunaServiceMessage(m);
    pthread_mutex_lock(&rq_mtx);
    if (msg && rq_len < 64) rq_queue[rq_len++] = (reply_t) { k, root, strdup(msg) };
    pthread_mutex_unlock(&rq_mtx);
    main_wake();
    return true;
}
/* libhelpers hands the callback no reliable tag, so each (kind, path) pair gets its own. */
#define RQ_CB(k) \
    static bool cb_##k(LSHandle *s, LSMessage *m, void *c) { (void) s; (void) c; return rq_push(k, false, m); } \
    static bool cbr_##k(LSHandle *s, LSMessage *m, void *c) { (void) s; (void) c; return rq_push(k, true, m); }
RQ_CB(RQ_APPS) RQ_CB(RQ_PIC) RQ_CB(RQ_SND) RQ_CB(RQ_SLEEP)
RQ_CB(RQ_INPUTS) RQ_CB(RQ_TIMEFMT) RQ_CB(RQ_RUNNING) RQ_CB(RQ_UNINSTALL) RQ_CB(RQ_ACT) RQ_CB(RQ_ICONS)
RQ_CB(RQ_HOMEHOOK)
static const LSFilterFunc rq_cbs[RQ_N][2] = {
    { cb_RQ_APPS, cbr_RQ_APPS }, { cb_RQ_PIC, cbr_RQ_PIC }, { cb_RQ_SND, cbr_RQ_SND },
    { cb_RQ_SLEEP, cbr_RQ_SLEEP }, { cb_RQ_INPUTS, cbr_RQ_INPUTS }, { cb_RQ_TIMEFMT, cbr_RQ_TIMEFMT },
    { cb_RQ_RUNNING, cbr_RQ_RUNNING }, { cb_RQ_UNINSTALL, cbr_RQ_UNINSTALL }, { cb_RQ_ACT, cbr_RQ_ACT },
    { cb_RQ_ICONS, cbr_RQ_ICONS }, { cb_RQ_HOMEHOOK, cbr_RQ_HOMEHOOK },
};
static bool rq_raw[RQ_N];     /* a shell command through the Homebrew exec, not a luna call */

/* Root path: luna-send through the Homebrew Channel's exec, as lumen does for the calls a
 * sandboxed app is refused. The reply's stdoutString carries the real response. */
static void rq_send_path(rq_t k, bool root) {
    HContext *c = &rq_ctx[k][root];
    c->callback = rq_cbs[k][root]; c->multiple = 0; c->pub = 1;
    if (!root) { HLunaServiceCall(rq_uri[k], rq_payload[k], c); return; }
    char cmd[1100], esc[2200];
    if (rq_raw[k]) snprintf(cmd, sizeof(cmd), "%s", rq_payload[k]);
    else snprintf(cmd, sizeof(cmd), "luna-send -n 1 %s '%s'", rq_uri[k], rq_payload[k]);
    char *o = esc;
    for (const char *p = cmd; *p && o < esc + sizeof(esc) - 3; p++) {
        if (*p == '"' || *p == '\\') *o++ = '\\';
        *o++ = *p;
    }
    *o = 0;
    char payload[2300];
    snprintf(payload, sizeof(payload), "{\"command\":\"%s\"}", esc);
    HLunaServiceCall("luna://org.webosbrew.hbchannel.service/exec", payload, c);
}

/* `fail` (a toast) is shown only if the root path fails too; NULL = silent. */
static void rq_send(rq_t k, bool root, const char *uri, const char *payload, const char *fail) {
    snprintf(rq_uri[k], sizeof(rq_uri[k]), "%s", uri);
    snprintf(rq_payload[k], sizeof(rq_payload[k]), "%s", payload);
    snprintf(rq_fail[k], sizeof(rq_fail[k]), "%s", fail ? fail : "");
    rq_send_path(k, root);
}

/* Run a root shell command; the reply is the exec's own JSON with stdoutString. */
static void rq_exec(rq_t k, const char *cmd) {
    rq_raw[k] = true;
    snprintf(rq_payload[k], sizeof(rq_payload[k]), "%s", cmd);
    rq_fail[k][0] = 0;
    rq_send_path(k, true);
}

/* The Home key hook (homehook/watch.sh, run as root): `status` prints hook | poll | lginputhook |
 * off | none. Empty while unknown, and for good without root (the exec is refused). */
#define HOMEHOOK APP_DIR "/homehook/watch.sh"
static char homehook_state[16];
static void homehook_query(void) { rq_exec(RQ_HOMEHOOK, HOMEHOOK " status"); }
static bool homehook_on(void) { return !strcmp(homehook_state, "hook") || !strcmp(homehook_state, "poll"); }

static void launch_app_id(const char *id) {
    char p[200];
    snprintf(p, sizeof(p), "{\"id\":\"%s\"}", id);
    rq_send(RQ_ACT, false, "luna://com.webos.applicationManager/launch", p, "Couldn't open the app");
}

/* ================================================================ weather (plain HTTP) */

static pthread_mutex_t wx_mtx = PTHREAD_MUTEX_INITIALIZER;
static char wx_text[160];
static bool wx_ready;
static volatile bool wx_dirty;      /* the weather city changed: fetch again now */
static char wx_city[48];            /* chosen city, "" = locate by IP (guarded by wx_mtx) */
static float wx_lat, wx_lon;
static volatile bool running = true;

static int http_get(const char *host, const char *path, char *out, int outsz) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { 8, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0) { close(fd); return -1; }
    char req[640];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: mytvlauncher\r\nConnection: close\r\n\r\n", path, host);
    if (write(fd, req, rl) != rl) { close(fd); return -1; }
    int total = 0, n;
    while (total < outsz - 1 && (n = read(fd, out + total, outsz - 1 - total)) > 0) total += n;
    close(fd);
    out[total] = 0;
    char *body = strstr(out, "\r\n\r\n");
    if (!body) return -1;
    memmove(out, body + 4, strlen(body + 4) + 1);
    return (int) strlen(out);
}

static const char *wmo_text(int c) {   /* lumen's weather words */
    if (c == 0) return "Clear";
    if (c <= 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 86) return "Showers";
    if (c >= 95) return "Storm";
    return "";
}

static void *weather_thread(void *arg) {
    (void) arg;
    char buf[8192];
    while (running) {
        char city[64] = ""; double lat = 0, lon = 0;
        wx_dirty = false;
        pthread_mutex_lock(&wx_mtx);
        if (wx_city[0]) { snprintf(city, sizeof(city), "%s", wx_city); lat = wx_lat; lon = wx_lon; }
        pthread_mutex_unlock(&wx_mtx);
        if (!city[0] && http_get("ip-api.com", "/json/?fields=status,city,lat,lon", buf, sizeof(buf)) > 0) {
            cJSON *j = cJSON_Parse(buf);
            if (j) {
                cJSON *c = cJSON_GetObjectItem(j, "city"), *la = cJSON_GetObjectItem(j, "lat"), *lo = cJSON_GetObjectItem(j, "lon");
                if (cJSON_IsString(c)) snprintf(city, sizeof(city), "%s", c->valuestring);
                if (cJSON_IsNumber(la)) lat = la->valuedouble;
                if (cJSON_IsNumber(lo)) lon = lo->valuedouble;
                cJSON_Delete(j);
            }
        }
        if (city[0]) {
            char path[256];
            snprintf(path, sizeof(path), "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code", lat, lon);
            if (http_get("api.open-meteo.com", path, buf, sizeof(buf)) > 0) {
                cJSON *j = cJSON_Parse(buf), *cur = j ? cJSON_GetObjectItem(j, "current") : NULL;
                cJSON *t = cur ? cJSON_GetObjectItem(cur, "temperature_2m") : NULL;
                cJSON *wc = cur ? cJSON_GetObjectItem(cur, "weather_code") : NULL;
                if (cJSON_IsNumber(t)) {
                    pthread_mutex_lock(&wx_mtx);
                    snprintf(wx_text, sizeof(wx_text), "%s \xC2\xB7 %d\xC2\xB0 %s", city,
                             (int) lround(t->valuedouble), wmo_text(cJSON_IsNumber(wc) ? wc->valueint : -1));
                    wx_ready = true;
                    pthread_mutex_unlock(&wx_mtx);
                    main_wake();
                }
                cJSON_Delete(j);
            }
        }
        for (int i = 0; i < 900 && running && !wx_dirty; i++) sleep(1);
    }
    return NULL;
}

/* Hand the chosen weather city (or "" = locate by IP) to the weather thread and wake it. */
static void weather_apply_prefs(void) {
    pthread_mutex_lock(&wx_mtx);
    wx_city[0] = 0;
    for (int c = 0; c < NCOUNTRIES && !wx_city[0]; c++) {
        if (strcmp(COUNTRIES[c].code, prefs.country)) continue;
        for (int k = 0; k < COUNTRIES[c].n; k++)
            if (!strcmp(COUNTRIES[c].cities[k].name, prefs.city)) {
                snprintf(wx_city, sizeof(wx_city), "%s", prefs.city);
                wx_lat = COUNTRIES[c].cities[k].lat;
                wx_lon = COUNTRIES[c].cities[k].lon;
            }
    }
    pthread_mutex_unlock(&wx_mtx);
    wx_dirty = true;
}

/* ================================================================ SDL/LVGL bridge */

#ifdef MYTV_DEBUG
/* `dump <n> x y w h`: the next n frames' region as /tmp/frameNN.ppm (diagnostics) */
static int dump_left, dump_idx;
static SDL_Rect dump_rect;

static void frame_dump(SDL_Renderer *r, SDL_Texture *tex) {
    static uint8_t px[1920 * 1080 * 4];
    SDL_SetRenderTarget(r, tex);
    if (SDL_RenderReadPixels(r, &dump_rect, SDL_PIXELFORMAT_ARGB8888, px, dump_rect.w * 4)) {
        LOG("dump: %s\n", SDL_GetError()); dump_left = 0; return;
    }
    char p[64];
    snprintf(p, sizeof(p), "/tmp/frame%02d.ppm", dump_idx++);
    FILE *f = fopen(p, "wb");
    if (!f) { dump_left = 0; return; }
    fprintf(f, "P6\n%d %d\n255\n", dump_rect.w, dump_rect.h);
    for (int i = 0; i < dump_rect.w * dump_rect.h; i++) fwrite(&px[i * 4], 1, 1, f), fwrite(&px[i * 4 + 1], 1, 1, f), fwrite(&px[i * 4 + 2], 1, 1, f);
    fclose(f);
    LOG("dump %s at %u\n", p, SDL_GetTicks());
    dump_left--;
}
#endif

static bool bench_on;      /* statbeg..statend: frame stats and scroll trace */

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *src) {
    LV_UNUSED(src);
    if (area->x2 < 0 || area->y2 < 0 || area->x1 > drv->hor_res - 1 || area->y1 > drv->ver_res - 1) {
        lv_disp_flush_ready(drv); return;
    }
    if (lv_disp_flush_is_last(drv)) {
        lv_draw_sdl_drv_param_t *param = drv->user_data;
        SDL_Renderer *r = param->renderer;
        SDL_Texture *tex = drv->draw_buf->buf1;
#ifdef MYTV_DEBUG
        if (dump_left > 0) frame_dump(r, tex);
#endif
        SDL_SetRenderTarget(r, NULL);
        SDL_RenderClear(r);
        SDL_RenderCopy(r, tex, NULL, NULL);
#ifdef MYTV_DEBUG
        uint32_t p0 = SDL_GetTicks();
        SDL_RenderPresent(r);
        if (bench_on) LOG("present %u +%u\n", p0, SDL_GetTicks() - p0);
#else
        SDL_RenderPresent(r);
#endif
        SDL_SetRenderTarget(r, tex);
    }
    lv_disp_flush_ready(drv);
}

static void display_init(SDL_Renderer *renderer, int w, int h) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t drv;
    static lv_draw_sdl_drv_param_t param;
    SDL_Texture *texture = lv_draw_sdl_create_screen_texture(renderer, w, h);
    lv_disp_draw_buf_init(&draw_buf, texture, NULL, w * h);
    lv_disp_drv_init(&drv);
    param.renderer = renderer;
    drv.user_data = &param;
    drv.draw_buf = &draw_buf;
    drv.flush_cb = flush_cb;
    drv.hor_res = w; drv.ver_res = h;
    SDL_SetRenderTarget(renderer, texture);
    lv_disp_drv_register(&drv);
}

/* ================================================================ animation helpers */

/* lumen's motion curve cubic-bezier(.2,.7,.2,1), tabulated once (x -> y). */
static int16_t bez_lut[257];
static void bez_init(void) {
    const double x1 = .2, y1 = .7, x2 = .2, y2 = 1;
    for (int i = 0; i <= 256; i++) {
        double x = i / 256.0, lo = 0, hi = 1, t = x;
        for (int k = 0; k < 30; k++) {
            t = (lo + hi) / 2;
            double bx = 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
            if (bx < x) lo = t; else hi = t;
        }
        bez_lut[i] = (int16_t) lround((3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t) * 1024);
    }
}
static int32_t path_lumen(const lv_anim_t *a) {
    int32_t i = a->time ? (int32_t) ((int64_t) a->act_time * 256 / a->time) : 256;
    if (i > 256) i = 256;
    return a->start_value + (int32_t) (((int64_t) bez_lut[i] * (a->end_value - a->start_value)) >> 10);
}

static void ax_x(void *o, int32_t v) { lv_obj_set_x(o, (lv_coord_t) v); }
static void ax_y(void *o, int32_t v) { lv_obj_set_y(o, (lv_coord_t) v); }
static void ax_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t) v, 0); }
static void ax_opa_layered(void *o, int32_t v) { lv_obj_set_style_opa_layered(o, (lv_opa_t) v, 0); }
static void ax_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, (lv_coord_t) v, 0); }
static void ax_zoom(void *o, int32_t v) { lv_img_set_zoom(o, (uint16_t) v); }
static void ax_zoom_obj(void *o, int32_t v) { lv_obj_set_style_transform_zoom(o, (lv_coord_t) v, 0); }
/* An empty range (hi < lo, e.g. a row before its apps load) gives lo: an index stays valid. */
static int clampi(int v, int lo, int hi) { return v < lo || hi < lo ? lo : v > hi ? hi : v; }

static bool motion_off;    /* Settings > Appearance > Reduce motion */
static void anim(void *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, lv_anim_path_cb_t path) {
    lv_anim_del(obj, cb);
    if (motion_off) ms = 0;
    if (from == to || ms == 0) { cb(obj, to); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_ease_out);
    lv_anim_start(&a);
}
static void anim_opa(lv_obj_t *o, lv_opa_t to, uint32_t ms) {
    anim(o, ax_opa, lv_obj_get_style_opa(o, 0), to, ms, lv_anim_path_ease_out);
}

/* Scroll: the position follows its target as a critically damped spring (SCROLL_W rad/ms). A held
 * key moves the target one row per repeat, a staircase. An eased anim restarted per repeat, and
 * then a first-order chase, both replayed that staircase as speed surging and dying each step
 * (measured 24 -> 6 px/frame every 100 ms). The spring alone still pulsed 10 -> 16 px/frame with
 * the owner's remote (repeats every ~50 ms, one row per ~100 ms after KEY_REPEAT_MS), so a step
 * that comes within SCROLL_HELD_MS of the last one is spread over that interval: the target becomes
 * a ramp and the speed is flat. A single press still settles in about 260 ms (lumen scrollMs). */
#define SCROLL_W 0.018f
#define SCROLL_HELD_MS 250
/* The spring advances in whole display frames: timer ticks land 15-19 ms apart while vsync shows
 * one frame per 16.7 ms, and stepping by the tick put 13, 16, 13 px into equal frames (shimmer). */
#define FRAME_MS (1000.0f / 60.0f)
/* A timer, not an lv_anim: the chase ends itself, and an lv_anim deleted from its own exec_cb is
 * read again by the anim timer after the free. Not in lv_anim_count_running(): see scrolling(). */
typedef struct {
    void *obj; lv_anim_exec_xcb_t cb; float pos, vel; int32_t to; lv_timer_t *tm;
    float ramp_from; uint32_t ramp_t0, ramp_ms;     /* the target slides from ramp_from to `to` */
    float tf;                                       /* the spring's clock, in whole frames */
} chase_t;
static chase_t chases[8];      /* a slot is busy while its timer runs */

static void chase_stop(chase_t *c) {
    if (c->tm) lv_timer_del(c->tm);
    c->tm = NULL;
}

static float chase_target(const chase_t *c, uint32_t now) {
    uint32_t e = now - c->ramp_t0;
    return c->ramp_ms && e < c->ramp_ms ? c->ramp_from + (c->to - c->ramp_from) * (float) e / (float) c->ramp_ms : (float) c->to;
}

static void chase_step(lv_timer_t *tm) {
    chase_t *c = tm->user_data;
    if (!lv_obj_is_valid(c->obj)) { chase_stop(c); return; }   /* a page rebuilt under the chase */
    uint32_t now = SDL_GetTicks();
    long frames = lroundf(((float) now - c->tf) / FRAME_MS);
    if (frames < 1) frames = 1;                  /* a late frame advances by as many frames as it missed */
    c->tf += (float) frames * FRAME_MS;
    float target = chase_target(c, (uint32_t) c->tf);
    for (float ms = (float) frames * FRAME_MS; ms > 0.0f; ) {   /* 4 ms substeps keep the integration stable */
        float h = ms < 4.0f ? ms : 4.0f;
        c->vel += (SCROLL_W * SCROLL_W * (target - c->pos) - 2.0f * SCROLL_W * c->vel) * h;
        c->pos += c->vel * h;
        ms -= h;
    }
    if (bench_on) LOG("chase %u %d -> %d\n", now, (int) lroundf(c->pos), c->to);
    if (target == (float) c->to && fabsf(c->to - c->pos) < 0.5f && fabsf(c->vel) < 0.02f) { c->cb(c->obj, c->to); chase_stop(c); return; }
    c->cb(c->obj, (int32_t) lroundf(c->pos));
}

static bool scrolling(void) {
    for (int i = 0; i < COUNT(chases); i++) if (chases[i].tm) return true;
    return false;
}

/* While a chase runs, the loop renders without sleeping, so SDL_RenderPresent blocks on the
 * compositor's frame callback and each spring step lands in its own vsync. Waking on LVGL's 16 ms
 * timers presented 8-24 ms apart instead: the compositor showed some steps twice and dropped others. */
static bool scroll_frame_due(void) {
    if (!scrolling()) return false;
    for (int i = 0; i < COUNT(chases); i++) if (chases[i].tm) lv_timer_ready(chases[i].tm);
    lv_timer_ready(lv_disp_get_default()->refr_timer);
    return true;
}

static void anim_scroll(void *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, bool animate) {
    chase_t *c = NULL;
    for (int i = 0; i < COUNT(chases) && !c; i++) if (chases[i].tm && chases[i].obj == obj) c = &chases[i];
    for (int i = 0; i < COUNT(chases) && !c; i++) if (!chases[i].tm) c = &chases[i];
    if (!c || !animate || motion_off || (!c->tm && from == to)) { if (c) chase_stop(c); cb(obj, to); return; }
    uint32_t now = SDL_GetTicks();
    if (!c->tm) { c->ramp_ms = 0; c->ramp_t0 = now; }
    else if (to != c->to) {
        uint32_t gap = now - c->ramp_t0;
        c->ramp_from = chase_target(c, now);
        /* averaged: a skipped remote repeat (150 ms instead of 100) would otherwise sag the speed */
        c->ramp_ms = gap >= SCROLL_HELD_MS ? 0 : c->ramp_ms ? (c->ramp_ms * 3 + gap) / 4 : gap;
        c->ramp_t0 = now;
    }
    c->obj = obj; c->cb = cb; c->to = to;
    if (c->tm) return;          /* already chasing: it picks up the new target on its next step */
    c->pos = (float) from;
    c->vel = 0;
    c->tf = (float) now;
    c->tm = lv_timer_create(chase_step, LV_DISP_DEF_REFR_PERIOD, c);
}

/* LVGL keeps style state per object, so a focus that should restyle a tile's icon, badge and
 * labels too is applied to the whole (shallow) subtree. */
static void set_state_tree(lv_obj_t *o, lv_state_t st, bool on) {
    if (!o) return;
    if (on) lv_obj_add_state(o, st); else lv_obj_clear_state(o, st);
    uint32_t n = lv_obj_get_child_cnt(o);
    for (uint32_t i = 0; i < n; i++) set_state_tree(lv_obj_get_child(o, i), st, on);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return o;
}

/* A moving container whose children overflow it (focus zoom, baked shadows, the label under a
 * tile). LVGL invalidates only an object's own area plus its ext-draw margin when it moves or
 * fades, so the margin must cover the overflow or the old pixels stay on screen. */
static void ext_draw_cb(lv_event_t *e) { lv_event_set_ext_draw_size(e, (lv_coord_t) (intptr_t) lv_event_get_user_data(e)); }
static lv_obj_t *group(lv_obj_t *parent, int x, int y, int w, int h, int margin) {
    lv_obj_t *o = box(parent, x, y, w, h);
    lv_obj_add_event_cb(o, ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *) (intptr_t) margin);
    lv_obj_refresh_ext_draw_size(o);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}
static lv_obj_t *image(lv_obj_t *parent, const void *src) {
    lv_obj_t *i = lv_img_create(parent);
    lv_img_set_src(i, src);
    lv_obj_clear_flag(i, LV_OBJ_FLAG_CLICKABLE);
    return i;
}
/* A 1/8-scale blurred wallpaper stretched over the whole screen from its top-left corner.
 * (SIZE_MODE_REAL is no use here: LVGL 8.3 centres the drawn image for it, ignoring the pivot.) */
static void zoom_full(lv_obj_t *img) {
    lv_img_set_pivot(img, 0, 0);
    lv_img_set_zoom(img, 256 * GLASS_DIV);
}

/* A line icon: 62 % white idle (#ffffff9e), recoloured #111 when its tile is focused. */
static lv_obj_t *icon(lv_obj_t *parent, const lv_img_dsc_t *src) {
    lv_obj_t *i = image(parent, src);
    lv_obj_set_style_img_opa(i, 0x9e, 0);
    lv_obj_set_style_img_opa(i, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_img_recolor(i, lv_color_hex(0x111111), LV_STATE_FOCUSED);
    lv_obj_set_style_img_recolor_opa(i, LV_OPA_COVER, LV_STATE_FOCUSED);
    return i;
}

/* ================================================================ wallpapers + glass */

/* Glass recipes, each the lumen CSS it mirrors. */
/* 1.1.4 tray (lumen-performance.css): flat rgba(22,28,40,.22) under a white 135deg sheen
 * (.18 / .05 @42 % / .12) -- it no longer shows the blurred wallpaper. Rims, glow and shadow as
 * before; the whole :before at opacity .74. Wallpaper-independent, so rendered once. */
static const glass_style_t GS_TRAY = {
    .opa = 0xbd, .flat = 0x38161c28, .sheen_deg = 135, .sheen_a = { 0x2e, 0x0d, 0x1f }, .sheen_mid = .42f,
    .rim_t = 0xe6, .rim_l = 0x8c, .rim_b = 0x4d, .rim_r = 0x4d, .glow = 0x14, .glow_w = 30,
    .sh_y = 20, .sh_blur = 44, .sh_opa = 0x47 };
static const glass_style_t GS_PILL = {    /* .pill */
    .opa = 0xff, .sheen_deg = 135, .sheen_a = { 0x42, 0x12, 0x29 }, .sheen_mid = .42f,
    .rim_t = 0xe6, .rim_l = 0xe6, .rim_b = 0x6b, .rim_r = 0x6b, .glow = 0x1a, .glow_w = 14,
    .sh_y = 8, .sh_blur = 18, .sh_opa = 0x40 };
static const glass_style_t GS_PILL_F = {  /* .pill--focused: linear-gradient(145deg,#ffffff80,#fff3 55%,#ffffff57) */
    .opa = 0xff, .sheen_deg = 145, .sheen_a = { 0x80, 0x33, 0x57 }, .sheen_mid = .55f,
    .rim_t = 0xe6, .rim_l = 0xe6, .rim_b = 0x6b, .rim_r = 0x6b, .glow = 0x1a, .glow_w = 14,
    .sh_y = 8, .sh_blur = 18, .sh_opa = 0x40 };
/* .cc__mod (dark): sheen over #00000061 over the blurred wallpaper, pill rims and shadow;
 * focused: the 145deg sheen over #00000038 */
static const glass_style_t GS_CCM = {
    .opa = 0xff, .sheen_deg = 135, .sheen_a = { 0x42, 0x12, 0x29 }, .sheen_mid = .42f, .dim = 0x61,
    .rim_t = 0xe6, .rim_l = 0xe6, .rim_b = 0x6b, .rim_r = 0x6b, .glow = 0x1a, .glow_w = 14,
    .sh_y = 8, .sh_blur = 18, .sh_opa = 0x40 };
static const glass_style_t GS_CCM_F = {
    .opa = 0xff, .sheen_deg = 145, .sheen_a = { 0x80, 0x33, 0x57 }, .sheen_mid = .55f, .dim = 0x38,
    .rim_t = 0xe6, .rim_l = 0xe6, .rim_b = 0x6b, .rim_r = 0x6b, .glow = 0x1a, .glow_w = 14,
    .sh_y = 8, .sh_blur = 18, .sh_opa = 0x40 };
/* html:not(.light) .panel--menu: rgba(14,18,30,.46) over the blurred wallpaper, no white sheen */
static const glass_style_t GS_MENU = {
    .opa = 0xff, .sheen_deg = 135, .dim = 0x75, .rim_t = 0x80, .rim_l = 0x80, .sh_y = 10, .sh_blur = 24, .sh_opa = 0x4d };
static const glass_style_t GS_PANEL = {   /* .panel: sheen + inset 2px 2px 1.5px -1px #ffffff80, 0 10px 24px #0000004d */
    .opa = 0xff, .sheen_deg = 135, .sheen_a = { 0x42, 0x12, 0x29 }, .sheen_mid = .42f,
    .rim_t = 0x80, .rim_l = 0x80, .sh_y = 10, .sh_blur = 24, .sh_opa = 0x4d };

/* Glass surfaces, each anchored where lumen puts it. */
/* G_CCM + 2*module + focused: Control Center modules; G_CCO + 2*option + focused: its value lists */
enum { G_TRAY, G_PILL0, G_PILL1, G_PILL0_F, G_PILL1_F, G_INPUTS, G_CTX, G_FOLDER, G_CCM, G_CCO = G_CCM + 20, G_KINDS = G_CCO + 16 };

typedef struct {
    glass_src_t src;
    lv_img_dsc_t veil, settings_bg;   /* zoomed 8x: .screen--lower:before and .screen--settings:before */
    lv_img_dsc_t thumb;               /* 360x203 for Settings > Wallpaper (bundled pictures only) */
    bool ready;                       /* src, veil, thumb and tray exist (wp_ready) */
    bool px;                          /* wp_dsc holds the full-size picture (wp_px): the worker sets
                                       * it, the UI clears it while another app is in front */
    bool warm;                        /* its glass is rendered for every known size (wp_warm) */
} wp_t;

static lv_img_dsc_t wp_dsc[WP_N];
static wp_t wps[WP_N];
static int wp_cur;
static int tray_w_want = 6 * TILE_STEP - TILE_GAP + 48;
static const int PILL_X[2] = { SCR_W - EDGE - 16 - PILL - 16 - PILL, SCR_W - EDGE - 16 - PILL };

static void cc_rect(int id, int *x, int *y, int *w);
static void glass_geom(int kind, int w, int *x, int *y, int *r, const glass_style_t **st) {
    *st = &GS_PANEL;
    *r = 44;
    if (kind >= G_CCO) {
        *x = CC_X; *y = CC_Y + (kind - G_CCO) / 2 * CC_OPT_STEP; *r = CC_OPT_H / 2;
        *st = (kind - G_CCO) & 1 ? &GS_CCM_F : &GS_CCM;
        return;
    }
    if (kind >= G_CCM) {
        int mx, my, mw;
        cc_rect((kind - G_CCM) / 2, &mx, &my, &mw);
        *x = CC_X + mx; *y = CC_Y + my; *r = CC_U / 2;
        *st = (kind - G_CCM) & 1 ? &GS_CCM_F : &GS_CCM;
        return;
    }
    switch (kind) {
        case G_TRAY: *x = (SCR_W - w) / 2; *y = ROWS_TOP - 24; *r = TILE_R + 24; *st = &GS_TRAY; break;
        case G_PILL0: case G_PILL1: case G_PILL0_F: case G_PILL1_F:
            *x = PILL_X[(kind - G_PILL0) % 2]; *y = PILL_Y; *r = PILL / 2;
            *st = kind >= G_PILL0_F ? &GS_PILL_F : &GS_PILL;
            break;
        case G_INPUTS: *x = SCR_W - EDGE - 760; *y = 190; break;
        case G_CTX: *x = SCR_W / 2 - 220; *y = 300; *st = &GS_MENU; break;
        default: *x = SCR_W / 2 - 380; *y = 250; break;   /* G_FOLDER */
    }
}

/* Rendered surfaces keyed by (wallpaper, kind, size). Entries are never freed: LVGL's SDL
 * texture cache is keyed by the descriptor's address, so an address must never carry new
 * pixels. Only the pixels of a wallpaper off screen are (glass_shed); rendering is
 * deterministic, so the re-render into the same entry is the same picture. The set of sizes
 * stays tiny (a tray width per app count, a few panel heights). */
typedef struct gentry { int wp, kind, w, h; glass_img_t g; struct gentry *next; } gentry_t;
static gentry_t *gcache;
static pthread_mutex_t gmtx = PTHREAD_MUTEX_INITIALIZER;

static gentry_t *gcache_find(int wp, int kind, int w, int h) {
    for (gentry_t *e = gcache; e; e = e->next) if (e->wp == wp && e->kind == kind && e->w == w && e->h == h) return e;
    return NULL;
}

/* Safe from any thread. A miss renders synchronously (tens of ms), so the UI only misses
 * when a size appears that the worker has not been asked for yet, or a wallpaper is shown
 * before the worker has warmed it up again. Every UI use of glass goes through here, so a
 * shed entry is never drawn without its pixels. */
static const glass_img_t *glass_get(int wp, int kind, int w, int h) {
    pthread_mutex_lock(&gmtx);
    gentry_t *e = gcache_find(wp, kind, w, h);
    bool have = e && e->g.dsc.data;
    pthread_mutex_unlock(&gmtx);
    if (have) return &e->g;
    int x, y, r;
    const glass_style_t *st;
    glass_geom(kind, w, &x, &y, &r, &st);
    glass_img_t g;
    uint32_t t0 = SDL_GetTicks();
    glass_render(&g, &wps[wp].src, x, y, w, h, r, st);
    if (kind >= G_INPUTS && kind < G_CCM) LOG("glass %d/%d %dx%d: %u ms\n", wp, kind, w, h, SDL_GetTicks() - t0);
    pthread_mutex_lock(&gmtx);
    e = gcache_find(wp, kind, w, h);            /* the other thread may have rendered it meanwhile */
    if (!e) {
        e = calloc(1, sizeof(*e));
        e->wp = wp; e->kind = kind; e->w = w; e->h = h;
        e->next = gcache;
        gcache = e;
    }
    if (e->g.dsc.data) free((void *) g.dsc.data);
    else e->g = g;
    pthread_mutex_unlock(&gmtx);
    return &e->g;
}

static bool wp_warm(int i) { return __atomic_load_n(&wps[i].warm, __ATOMIC_ACQUIRE); }

/* Free the pixels of wallpaper i's glass once it has left the screen (UI thread): a set is
 * ~12 MB, and only the wallpaper on screen and a pending one need theirs. The tray is shared
 * by all wallpapers and stays. */
static void glass_shed(int i) {
    size_t n = 0;
    pthread_mutex_lock(&gmtx);
    for (gentry_t *e = gcache; e; e = e->next)
        if (e->wp == i && e->kind != G_TRAY && e->g.dsc.data) {
            /* LVGL's decoder cache keeps the pixel pointer: it would re-upload freed memory once
             * the SDL texture is evicted and the descriptor is filled again */
            lv_img_cache_invalidate_src(&e->g.dsc);
            free((void *) e->g.dsc.data);
            e->g.dsc.data = NULL;
            n += e->g.dsc.data_size;
        }
    pthread_mutex_unlock(&gmtx);
    __atomic_store_n(&wps[i].warm, false, __ATOMIC_RELEASE);
    if (n) LOG("glass of wallpaper %d freed: %zu KB\n", i, n / 1024);
}

/* Worker jobs: one surface size, rendered for every prepared wallpaper. */
typedef struct { int kind, w, h; } gjob_t;
static gjob_t gjobs[96];
static int ngjobs;
static pthread_mutex_t jmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jcv = PTHREAD_COND_INITIALIZER;

/* Every size ever queued, so a wallpaper that becomes wanted later gets the same set. */
static gjob_t ghist[128];
static int nghist;
static bool worker_kick;

static void glass_job(int kind, int w, int h) {
    pthread_mutex_lock(&jmtx);
    if (ngjobs < 96) gjobs[ngjobs++] = (gjob_t) { kind, w, h };
    bool seen = false;
    for (int i = 0; i < nghist; i++) seen |= ghist[i].kind == kind && ghist[i].w == w && ghist[i].h == h;
    if (!seen && nghist < 128) ghist[nghist++] = (gjob_t) { kind, w, h };
    pthread_cond_signal(&jcv);
    pthread_mutex_unlock(&jmtx);
}

/* The set of wanted wallpapers changed (picks, Plain, a pending switch): let the worker look. */
static void worker_wake(void) {
    pthread_mutex_lock(&jmtx);
    worker_kick = true;
    pthread_cond_signal(&jcv);
    pthread_mutex_unlock(&jmtx);
}

static void small_from_src(lv_img_dsc_t *d, const glass_src_t *s, uint8_t dim, uint32_t under, uint8_t opa) {
    uint8_t *px = malloc((size_t) s->w * s->h * 4);
    int ur = (under >> 16) & 0xff, ug = (under >> 8) & 0xff, ub = under & 0xff;
    for (int i = 0; i < s->w * s->h; i++) {
        const uint8_t *p = s->px + i * 4;
        int k = 255 - dim;
        int b = p[0] * k / 255, g = p[1] * k / 255, r = p[2] * k / 255;
        px[i * 4 + 0] = (uint8_t) ((b * opa + ub * (255 - opa)) / 255);
        px[i * 4 + 1] = (uint8_t) ((g * opa + ug * (255 - opa)) / 255);
        px[i * 4 + 2] = (uint8_t) ((r * opa + ur * (255 - opa)) / 255);
        px[i * 4 + 3] = 255;
    }
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d->header.w = s->w; d->header.h = s->h;
    d->data_size = (uint32_t) s->w * s->h * 4;
    d->data = px;
}

/* Everything a wallpaper needs before it can be shown. Pure CPU work on buffers; runs on
 * the worker for every wallpaper but the first. */
/* Box-filtered thumbnail of a 1920x1080 BGRA buffer (.wp__thumb is 360x203). */
static void thumb_from(lv_img_dsc_t *d, const uint8_t *px) {
    const int tw = 360, th = 203;
    uint8_t *out = malloc((size_t) tw * th * 4);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int x0 = x * SCR_W / tw, x1 = (x + 1) * SCR_W / tw, y0 = y * SCR_H / th, y1 = (y + 1) * SCR_H / th;
            unsigned s[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, n++)
                    for (int c = 0; c < 3; c++) s[c] += px[((size_t) yy * SCR_W + xx) * 4 + c];
            uint8_t *o = out + (y * tw + x) * 4;
            for (int c = 0; c < 3; c++) o[c] = (uint8_t) (s[c] / n);
            o[3] = 255;
        }
    glass_round_alpha(out, tw, th, 22);
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d->header.w = tw; d->header.h = th;
    d->data_size = (uint32_t) tw * th * 4;
    d->data = out;
}

/* lumen's plain .screen background: three soft radial glows over #0a0f1e. Each is
 * radial-gradient(ellipse RX RY at CX CY, colour, transparent 70%). */
static void wp_plain_build(void) {
    static const struct { float rx, ry, cx, cy; uint8_t r, g, b, a; } G[3] = {
        { .70f, .50f, .55f, 1.00f, 0x1e, 0xaa, 0xbe, 0x57 },     /* bottom layer first */
        { .55f, .60f, .85f, .30f, 0xaa, 0x50, 0xe6, 0x61 },
        { .60f, .55f, .18f, .18f, 0x46, 0x78, 0xff, 0x73 } };
    uint8_t *px = malloc((size_t) SCR_W * SCR_H * 4);
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++) {
            float c[3] = { 0x1e, 0x0f, 0x0a };                   /* BGR of #0a0f1e */
            for (int k = 0; k < 3; k++) {
                float dx = (x / (float) SCR_W - G[k].cx) / G[k].rx, dy = (y / (float) SCR_H - G[k].cy) / G[k].ry;
                float t = sqrtf(dx * dx + dy * dy) / .7f;
                if (t >= 1.f) continue;
                float a = G[k].a / 255.f * (1.f - t);
                c[0] += (G[k].b - c[0]) * a; c[1] += (G[k].g - c[1]) * a; c[2] += (G[k].r - c[2]) * a;
            }
            uint8_t *o = px + ((size_t) y * SCR_W + x) * 4;
            o[0] = (uint8_t) c[0]; o[1] = (uint8_t) c[1]; o[2] = (uint8_t) c[2]; o[3] = 255;
        }
    wp_dsc[WP_PLAIN].header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    wp_dsc[WP_PLAIN].header.w = SCR_W; wp_dsc[WP_PLAIN].header.h = SCR_H;
    wp_dsc[WP_PLAIN].data_size = (uint32_t) SCR_W * SCR_H * 4;
    wp_dsc[WP_PLAIN].data = px;
}

/* `ready` is written by the worker and read by the UI: release/acquire, not just volatile. */
static bool wp_ready(int i) { return __atomic_load_n(&wps[i].ready, __ATOMIC_ACQUIRE); }
static bool wp_px(int i) { return __atomic_load_n(&wps[i].px, __ATOMIC_ACQUIRE); }

/* lumen .wp-image .screen scrim: to top #0009 -> 0 at 45 %, to bottom #00000059 -> 0 at 25 % */
static void wp_scrim(uint8_t *px) {
    glass_scrim(px, SCR_W, 0, 270, 0x59, 0);
    glass_scrim(px, SCR_W, 594, SCR_H, 0, 0x99);
}

/* Glass is kept only for the wallpaper on screen and the one a switch waits for. Any other
 * is warmed up on demand: wp_switch waits for it (a few hundred ms) instead of holding
 * ~12 MB per ticked picture. */
static int wp_pending = -1;     /* a switch asked for before its wallpaper was warm */
static bool app_away;           /* another app is in front: no polling, no spare pictures */
static bool glass_wanted(int i) { return i == wp_cur || i == wp_pending; }

static void wp_prepare(int i) {
    wp_t *w = &wps[i];
    uint8_t *px = (uint8_t *) wp_dsc[i].data;
    if (!px || wp_ready(i)) return;
    glass_src_build(&w->src, px, SCR_W, SCR_H);
    if (i < WP_COUNT) {
        thumb_from(&w->thumb, px);
        wp_scrim(px);
    }
    small_from_src(&w->veil, &w->src, 0x42, 0x10131c, 255);
    small_from_src(&w->settings_bg, &w->src, 0x85, 0x181c28, 0xe6);   /* .screen--settings:before 1.1.4 */
    glass_get(0, G_TRAY, tray_w_want, TILE_H + 48);
    __atomic_store_n(&w->px, true, __ATOMIC_RELEASE);
    __atomic_store_n(&w->ready, true, __ATOMIC_RELEASE);
}

static void wp_decode(int i);

/* The full-size picture again after wp_px_shed (worker). src, thumb and veil were kept. */
static void wp_redecode(int i) {
    wp_decode(i);
    if (!wp_dsc[i].data) return;
    if (i < WP_COUNT) wp_scrim((uint8_t *) wp_dsc[i].data);
    __atomic_store_n(&wps[i].px, true, __ATOMIC_RELEASE);
}

/* UI thread, while another app is in front: drop the full-size pictures not on screen, 8 MB
 * each. The TV closes the biggest background app first when a heavy one starts, and myTV
 * coming back from scratch is slower than the worker decoding them again on return. */
static void wp_px_shed(void) {
    int n = 0;
    for (int i = 0; i < WP_N; i++)
        if (i != wp_cur && i != wp_pending && wp_px(i)) {
            __atomic_store_n(&wps[i].px, false, __ATOMIC_RELEASE);
            lv_img_cache_invalidate_src(&wp_dsc[i]);      /* as in glass_shed */
            free((void *) wp_dsc[i].data);
            wp_dsc[i].data = NULL;
            n++;
        }
    if (n) LOG("away: %d wallpaper pictures freed\n", n);
}

/* One wallpaper into our own BGRA buffer, kept while myTV is in front: a rotation never decodes
 * (a 1080p PNG is ~300 ms) and the glass/scrim passes work on the pixels. lodepng directly, because LVGL's
 * decoder and image cache are not thread-safe and this also runs on the worker. */
static void wp_decode(int i) {
    if (i == WP_PLAIN) { wp_plain_build(); return; }
    char p[256];
    snprintf(p, sizeof(p), "%s/wallpapers/wp%d.png", APP_DIR, i);
    FILE *f = fopen(p, "rb");
    if (!f) { LOG("wp%d: cannot open\n", i); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *png = malloc((size_t) n), *px = NULL;
    unsigned w = 0, h = 0, err = png && fread(png, 1, (size_t) n, f) == (size_t) n ? lodepng_decode32(&px, &w, &h, png, (size_t) n) : 1;
    fclose(f);
    free(png);
    if (err || w != SCR_W || h != SCR_H) { LOG("wp%d: decode failed (%u)\n", i, err); free(px); return; }
    for (size_t k = 0; k < (size_t) w * h * 4; k += 4) { uint8_t t = px[k]; px[k] = px[k + 2]; px[k + 2] = t; }  /* RGBA -> BGRA */
    wp_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    wp_dsc[i].header.w = w; wp_dsc[i].header.h = h;
    wp_dsc[i].data_size = (uint32_t) w * h * 4;
    wp_dsc[i].data = px;
}

/* Only the wallpaper on screen is decoded before the first frame. The worker then serves the
 * glass queue first, decodes the other pictures in idle time (the picker shows them all), and
 * fills in the glass of every wanted wallpaper from the size history. */
static void *glass_worker(void *arg) {
    (void) arg;
    static bool failed[WP_N];
    bool told = false;
    for (;;) {
        gjob_t j;
        pthread_mutex_lock(&jmtx);
        bool job = ngjobs > 0;
        if (job) { j = gjobs[0]; memmove(gjobs, gjobs + 1, sizeof(gjob_t) * --ngjobs); }
        pthread_mutex_unlock(&jmtx);
        if (job) {
            for (int i = 0; i < WP_N; i++) 
                if (!wp_ready(i)) continue;
                else if (glass_wanted(i)) glass_get(i, j.kind, j.w, j.h);
                else __atomic_store_n(&wps[i].warm, false, __ATOMIC_RELEASE);   /* missed this size */
            continue;
        }
        int next = -1;
        for (int i = 0; i < WP_N && next < 0; i++)
            if (!wp_ready(i) && !failed[i] && (i < WP_COUNT || glass_wanted(i))) next = i;
        if (next >= 0) {
            if (!wp_dsc[next].data) wp_decode(next);
            wp_prepare(next);
            failed[next] = !wp_ready(next);
            main_wake();                                /* a pending switch may go now */
            continue;
        }
        bool away = __atomic_load_n(&app_away, __ATOMIC_ACQUIRE);
        for (int i = 0; i < WP_N && next < 0; i++)
            if (wp_ready(i) && !wp_px(i) && !failed[i] && (glass_wanted(i) || (i < WP_COUNT && !away))) next = i;
        if (next >= 0) {
            uint32_t t0 = SDL_GetTicks();
            wp_redecode(next);
            LOG("wallpaper %d decoded again in %u ms\n", next, SDL_GetTicks() - t0);
            failed[next] = !wp_px(next);
            main_wake();
            continue;
        }
        int g = -1;
        for (int i = 0; i < WP_N && g < 0; i++) if (wp_ready(i) && glass_wanted(i) && !wp_warm(i)) g = i;
        if (g >= 0) {
            gjob_t h[128];
            pthread_mutex_lock(&jmtx);
            int n = nghist;
            memcpy(h, ghist, sizeof(gjob_t) * (size_t) n);
            pthread_mutex_unlock(&jmtx);
            uint32_t t0 = SDL_GetTicks();
            for (int k = G_PILL0; k <= G_PILL1_F; k++) glass_get(g, k, PILL, PILL);
            for (int k = 0; k < n; k++) glass_get(g, h[k].kind, h[k].w, h[k].h);
            LOG("glass of wallpaper %d warmed in %u ms\n", g, SDL_GetTicks() - t0);
            if (glass_wanted(g)) __atomic_store_n(&wps[g].warm, true, __ATOMIC_RELEASE);   /* else shed meanwhile */
            main_wake();                                /* a pending switch may go now */
            continue;
        }
        if (!told) { told = true; LOG("glass worker: wallpapers prepared\n"); }
        pthread_mutex_lock(&jmtx);
        while (!ngjobs && !worker_kick) pthread_cond_wait(&jcv, &jmtx);
        worker_kick = false;
        pthread_mutex_unlock(&jmtx);
    }
    return NULL;
}

/* Two stacked images per wallpaper-dependent layer; a rotation fades the back one in. */
enum { L_WP, L_VEIL, L_TRAY, L_PILL0, L_PILL1, L_N };
typedef struct { lv_obj_t *img[2]; int front; } dual_t;
static dual_t layers[L_N];
static bool pill_focused[2];

static void layer_set(int l, lv_obj_t *img, int wpi) {
    wp_t *W = &wps[wpi];
    switch (l) {
        case L_WP: lv_img_set_src(img, &wp_dsc[wpi]); break;
        case L_VEIL: lv_img_set_src(img, &W->veil); zoom_full(img); break;   /* set_src re-centres the pivot */
        case L_TRAY: {
            const glass_img_t *g = glass_get(0, G_TRAY, tray_w_want, TILE_H + 48);   /* same for every wallpaper */
            lv_img_set_src(img, &g->dsc);
            lv_obj_set_pos(img, (SCR_W - tray_w_want) / 2 - g->ox, -24 - g->oy);
            break;
        }
        default: {
            int p = l - L_PILL0;
            const glass_img_t *g = glass_get(wpi, (pill_focused[p] ? G_PILL0_F : G_PILL0) + p, PILL, PILL);
            lv_img_set_src(img, &g->dsc);
            lv_obj_set_pos(img, PILL_X[p] - g->ox, PILL_Y - g->oy);
            lv_img_set_pivot(img, g->ox + PILL / 2, g->oy + PILL / 2);
        }
    }
}

/* Opacity a layer rests at: the veil only shows in lower mode, pills vanish with the dock. */
static bool ui_lower(void);
static bool ui_dock_hidden(void);
static bool night_now;          /* "Dim the wallpaper at night" is in effect (19:00-06:00) */
static lv_opa_t layer_rest_opa(int l) {
    if (l == L_WP) return night_now ? 0x8c : LV_OPA_COVER;    /* html.night #wpimage{opacity:.55} */
    if (l == L_VEIL) return ui_lower() ? LV_OPA_COVER : 0;
    if (l == L_PILL0 || l == L_PILL1) return ui_dock_hidden() ? 0 : LV_OPA_COVER;
    return LV_OPA_COVER;
}

static void fade_exec(void *v, int32_t o) {
    (void) v;
    for (int l = 0; l < L_N; l++)
        if (layers[l].img[0]) lv_obj_set_style_opa(layers[l].img[1 - layers[l].front], (lv_opa_t) (o * layer_rest_opa(l) / 255), 0);
}
static void fade_done(lv_anim_t *a) {
    (void) a;
    for (int l = 0; l < L_N; l++) {
        if (!layers[l].img[0]) continue;
        lv_obj_t *old = layers[l].img[layers[l].front];
        lv_obj_set_style_opa(old, 0, 0);
        layer_set(l, old, wp_cur);              /* nothing may keep pointing at glass about to be shed */
        layers[l].front = 1 - layers[l].front;
    }
    for (int i = 0; i < WP_N; i++) if (!glass_wanted(i)) glass_shed(i);
}

/* Cross-fade every wallpaper-dependent layer to wallpaper `next`. */
static void wp_switch(int next) {
    if (next == wp_cur) return;
    if (!wp_ready(next) || !wp_px(next) || !wp_warm(next) || lv_anim_get(layers, fade_exec)) {   /* the main loop retries it */
        wp_pending = next;
        worker_wake();
        return;
    }
    wp_pending = -1;
    for (int l = 0; l < L_N; l++) {
        dual_t *d = &layers[l];
        if (!d->img[0]) continue;
        lv_obj_t *back = d->img[1 - d->front], *front = d->img[d->front];
        layer_set(l, back, next);
        lv_img_set_zoom(back, lv_img_get_zoom(front));
        if (lv_obj_get_index(back) < lv_obj_get_index(front)) lv_obj_swap(front, back);
        lv_obj_set_style_opa(back, 0, 0);
    }
    wp_cur = next;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, layers);
    lv_anim_set_exec_cb(&a, fade_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, WP_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&a, fade_done);
    lv_anim_start(&a);
}

/* A random ticked picture, not the one on screen when there is a choice. */
static int wp_random_pick(void) {
    int c[WP_COUNT], n = 0;
    for (int i = 0; i < WP_COUNT; i++) if ((prefs.picks >> i & 1) && i != wp_cur) c[n++] = i;
    if (!n) for (int i = 0; i < WP_COUNT; i++) if (prefs.picks >> i & 1) return i;
    return n ? c[rand() % n] : 0;
}
static int npicks(void) { return __builtin_popcount(prefs.picks); }

/* The wallpaper the next rotation shows, picked ahead so that wp_upload_ahead can put its
 * texture on the GPU while nothing moves: uploading a 1080p picture in the frame that first
 * shows it stalls the cross-fade. */
static int wp_next = -1;
static bool wp_next_valid(void) { return wp_next >= 0 && wp_next != wp_cur && (prefs.picks >> wp_next & 1); }
static int wp_next_take(void) {
    int n = wp_next_valid() ? wp_next : wp_random_pick();
    wp_next = -1;
    return n;
}

/* Main loop, idle only. A cache hit costs a lookup, so it may run on every idle pass; the
 * LRU may have evicted the texture since the last one. */
static void wp_upload(int i) {
    if (!wp_ready(i) || !wp_px(i)) return;
    lv_draw_ctx_t *dc = lv_disp_get_default()->driver->draw_ctx;
    uint32_t t0 = SDL_GetTicks();
    bool up = lv_draw_sdl_img_warm(dc, &wp_dsc[i]);
    up |= lv_draw_sdl_img_warm(dc, &wps[i].veil);
    if (up) LOG("wallpaper %d uploaded ahead in %u ms\n", i, SDL_GetTicks() - t0);
}
static void wp_upload_ahead(void) {
    if (prefs.wp_plain || npicks() < 2 || !(prefs.wp_auto_min || prefs.wp_new)) return;
    if (!wp_next_valid()) wp_next = wp_random_pick();
    wp_upload(wp_next);
}

/* lumen "Change wallpaper automatically": only with Shuffle, every 15/30/60 min. */
static bool ui_busy(void);
static lv_timer_t *wp_timer;
static void wp_rotate_cb(lv_timer_t *t) {
    (void) t;
    if (!ui_busy() && !prefs.wp_plain && !app_away) wp_switch(wp_next_take());   /* no crossfade nobody sees */
}
static void wp_timer_update(void) {
    if (!prefs.wp_plain && npicks() > 1 && prefs.wp_auto_min) {
        lv_timer_set_period(wp_timer, (uint32_t) prefs.wp_auto_min * 60000);
        lv_timer_reset(wp_timer);
        lv_timer_resume(wp_timer);
    } else lv_timer_pause(wp_timer);
}

static lv_obj_t *dual_create(int l, lv_obj_t *parent) {
    for (int i = 0; i < 2; i++) {
        layers[l].img[i] = image(parent, NULL);
        layer_set(l, layers[l].img[i], wp_cur);
        lv_obj_set_style_opa(layers[l].img[i], i == 0 ? LV_OPA_COVER : 0, 0);
    }
    layers[l].front = 0;
    return layers[l].img[0];
}

/* ================================================================ app icons */

/* Decode, sample the brand colour, scale to lumen's icon box (66 % of the tile height, at most
 * 70 % of its width) and round the corners 16 %. One-time CPU work; the tile then blits a
 * small, exactly-sized texture instead of GPU-minifying a large PNG every frame. */
/* Prepared icons by app id, never freed or reused. The SDL texture cache is keyed by the
 * descriptor's address, so after a reload (an uninstall shifts the list) one app's address must
 * not carry another app's pixels; a known id also skips the decode. */
typedef struct { char *id; lv_img_dsc_t img; lv_color_t color; bool has_img, has_color, own_copy; } icon_t;
static icon_t icons[2 * MAX_APPS];
static int nicons;

static void app_prepare(app_t *a) {
    for (int i = 0; i < nicons; i++)
        if (!strcmp(icons[i].id, a->id)) {
            a->img = &icons[i].img; a->has_img = icons[i].has_img;
            a->color = icons[i].color; a->has_color = icons[i].has_color;
            return;
        }
    if (nicons == COUNT(icons)) return;
    icon_t *e = &icons[nicons++];
    e->id = strdup(a->id);
    a->img = &e->img;
    /* The jail cannot read other apps' dirs: icons_copy() puts each app's icon into DATA_DIR as root. The
     * older 80 px copies are the fallback until that copy lands. */
    char copy[512], legacy[512], orig[600];
    snprintf(copy, sizeof(copy), "A:%s/icons/%s.L.png", DATA_DIR, a->id);
    snprintf(legacy, sizeof(legacy), "A:%s/icons/%s.png", DATA_DIR, a->id);
    const char *srcs[3] = { copy, legacy, NULL };
    if (a->icon && a->icon[0]) { snprintf(orig, sizeof(orig), "A:%s", a->icon); srcs[2] = orig; }
    for (int s = 0; s < 3; s++) {
        if (!srcs[s]) continue;
        lv_img_decoder_dsc_t d;
        if (lv_img_decoder_open(&d, srcs[s], lv_color_white(), 0) != LV_RES_OK) continue;
        const uint8_t *px = d.img_data;
        int W = d.header.w, H = d.header.h;
        if (!px || W < 4 || H < 4) { lv_img_decoder_close(&d); continue; }

        static uint16_t hist[4096];
        memset(hist, 0, sizeof(hist));
        int m = (W < H ? W : H) / 8, total = 0, opaque = 0;
        if (m < 2) m = 2;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (!(x < m || x >= W - m || y < m || y >= H - m)) continue;
                total++;
                const uint8_t *p = px + (y * W + x) * 4;
                if (p[3] < 128) continue;
                opaque++;
                hist[((p[2] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[0] >> 4)]++;
            }
        if (total && opaque * 100 / total >= 35) {
            int best = 0;
            for (int i = 1; i < 4096; i++) if (hist[i] > hist[best]) best = i;
            /* the mean of the pixels in the winning bin, not its centre: the bin is 16 levels
             * wide, and up to 8 off per channel shows the icon's square on the tile */
            uint32_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    if (!(x < m || x >= W - m || y < m || y >= H - m)) continue;
                    const uint8_t *p = px + (y * W + x) * 4;
                    if (p[3] < 128 || (((p[2] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[0] >> 4)) != best) continue;
                    sr += p[2]; sg += p[1]; sb += p[0]; n++;
                }
            a->color = lv_color_make((uint8_t) ((sr + n / 2) / n), (uint8_t) ((sg + n / 2) / n), (uint8_t) ((sb + n / 2) / n));
            a->has_color = true;
        }

        float sc = fminf(TILE_H * 0.66f / H, TILE_W * 0.70f / W);
        int tw = (int) lroundf(W * sc), th = (int) lroundf(H * sc);
        uint8_t *out = malloc((size_t) tw * th * 4);
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++) {
                if (sc > 1) {        /* enlarging: the box below would pick one pixel and show steps */
                    float fx = fmaxf((x + .5f) / sc - .5f, 0), fy = fmaxf((y + .5f) / sc - .5f, 0);
                    int ix = (int) fx, iy = (int) fy, jx = ix + 1 < W ? ix + 1 : ix, jy = iy + 1 < H ? iy + 1 : iy;
                    float ax = fx - ix, ay = fy - iy, w4[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
                    const uint8_t *q[4] = { px + (iy * W + ix) * 4, px + (iy * W + jx) * 4, px + (jy * W + ix) * 4, px + (jy * W + jx) * 4 };
                    float sb = 0, sg = 0, sr = 0, sa = 0;
                    for (int k = 0; k < 4; k++) {
                        float wa = w4[k] * q[k][3];
                        sb += q[k][0] * wa; sg += q[k][1] * wa; sr += q[k][2] * wa; sa += wa;
                    }
                    uint8_t *o = out + (y * tw + x) * 4;
                    if (sa > 0) { o[0] = (uint8_t) (sb / sa + .5f); o[1] = (uint8_t) (sg / sa + .5f); o[2] = (uint8_t) (sr / sa + .5f); }
                    else o[0] = o[1] = o[2] = 0;
                    o[3] = (uint8_t) (sa + .5f);
                    continue;
                }
                /* box filter over the source footprint, alpha-weighted */
                int x0 = (int) (x / sc), x1 = (int) ((x + 1) / sc), y0 = (int) (y / sc), y1 = (int) ((y + 1) / sc);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 > W) x1 = W;
                if (y1 > H) y1 = H;
                unsigned sb = 0, sg = 0, sr = 0, sa = 0, n = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        const uint8_t *p = px + (yy * W + xx) * 4;
                        sb += p[0] * p[3]; sg += p[1] * p[3]; sr += p[2] * p[3]; sa += p[3]; n++;
                    }
                uint8_t *o = out + (y * tw + x) * 4;
                if (sa) { o[0] = sb / sa; o[1] = sg / sa; o[2] = sr / sa; } else o[0] = o[1] = o[2] = 0;
                o[3] = n ? sa / n : 0;
            }
        lv_img_decoder_close(&d);
        glass_round_alpha(out, tw, th, 0.16f * (tw < th ? tw : th));
        e->img.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        e->img.header.w = tw; e->img.header.h = th;
        e->img.data_size = (uint32_t) tw * th * 4;
        e->img.data = out;
        a->has_img = e->has_img = true;
        e->own_copy = s == 0;
        e->color = a->color; e->has_color = a->has_color;
        return;
    }
    e->color = a->color; e->has_color = a->has_color;
}

static void rows_build(void);
static void panels_prefetch(void);

/* Copy the icons the sandbox cannot read into DATA_DIR, as root, in one exec: the list goes to a
 * file because a command line through the exec is capped at ~1000 bytes. */
static void icons_copy(void) {
    FILE *f = NULL;
    int n = 0;
    for (int i = 0; i < napps; i++) {
        if (!apps[i].img) continue;
        const icon_t *e = (const icon_t *) ((const char *) apps[i].img - offsetof(icon_t, img));
        if (e->own_copy || !apps[i].icon || !apps[i].icon[0] || strchr(apps[i].icon, ' ')) continue;
        if (!f && !(f = fopen(DATA_DIR "/icons.todo", "w"))) { LOG("icons_copy: cannot write the list\n"); return; }
        fprintf(f, "%s %s\n", apps[i].icon, apps[i].id);
        n++;
    }
    if (!f) return;
    fclose(f);
    LOG("icons_copy: %d icons\n", n);
    rq_exec(RQ_ICONS, "cd " DATA_DIR " && mkdir -p icons && while read s d; do cp \"$s\" \"icons/$d.L.png\" 2>/dev/null; done < icons.todo; "
                      "chmod a+r icons/*; rm icons.todo; echo ok");
}

/* The copies landed: re-prepare under a fresh entry, as the old descriptor's address keys a
 * texture already uploaded with the old pixels. */
static void icons_copied(void) {
    int n = 0;
    for (int i = 0; i < nicons; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/icons/%s.L.png", DATA_DIR, icons[i].id);
        if (icons[i].own_copy || !icons[i].id[0] || access(p, R_OK)) continue;
        icons[i].id[0] = 0;                 /* forgotten: app_prepare decodes the copy */
        n++;
    }
    for (int i = 0; i < napps; i++) app_prepare(&apps[i]);
    LOG("icons_copied: %d re-prepared\n", n);
    if (n) { rows_build(); panels_prefetch(); }
}

/* ================================================================ styles */

static lv_style_t st_tile, st_tile_f, st_ptile, st_ptile_f, st_cc, st_cc_f, st_sheen, st_sheen_f;
static lv_img_dsc_t sheen_tile;

/* Focus zoom/lift transition shared by every focusable; Reduce motion sets its time to 0. */
static lv_style_transition_dsc_t st_tr;

static void styles_init(void) {
    static const lv_style_prop_t props[] = { LV_STYLE_TRANSFORM_ZOOM, LV_STYLE_TRANSLATE_Y, 0 };
    lv_style_transition_dsc_init(&st_tr, props, lv_anim_path_ease_out, FOCUS_MS, 0, NULL);
    glass_sheen(&sheen_tile, TILE_W, TILE_H, TILE_R);

    /* .tile__box: --color-tile-bg unless the app has a colour; focus = scale 1.16, lift 6px,
     * 0 22px 34px #00000073 and the 115deg sheen. The shadow is static per state (an animated
     * blur would be a fresh shadow-cache key every frame). */
    lv_style_init(&st_tile);
    lv_style_set_radius(&st_tile, TILE_R);
    lv_style_set_bg_color(&st_tile, lv_color_white());
    lv_style_set_bg_opa(&st_tile, 0x1a);
    lv_style_set_shadow_color(&st_tile, lv_color_black());
    lv_style_set_transform_pivot_x(&st_tile, TILE_W / 2);
    lv_style_set_transform_pivot_y(&st_tile, TILE_H / 2);
    lv_style_set_transition(&st_tile, &st_tr);
    lv_style_init(&st_tile_f);
    lv_style_set_transform_zoom(&st_tile_f, FOCUS_ZOOMS[1]);
    lv_style_set_translate_y(&st_tile_f, -FOCUS_LIFT);
    lv_style_set_shadow_width(&st_tile_f, 34);
    lv_style_set_shadow_ofs_y(&st_tile_f, 22);
    lv_style_set_shadow_opa(&st_tile_f, 0x73);

    /* .panel .tile: same box at 200x120 r24; focus adds a 4px white ring, 0 14px 24px #00000059 */
    lv_style_init(&st_ptile);
    lv_style_set_radius(&st_ptile, PANEL_TILE_R);
    lv_style_set_bg_color(&st_ptile, lv_color_white());
    lv_style_set_bg_opa(&st_ptile, 0x1a);
    lv_style_set_shadow_color(&st_ptile, lv_color_black());
    lv_style_set_outline_color(&st_ptile, lv_color_white());
    lv_style_set_transform_pivot_x(&st_ptile, PANEL_TILE_W / 2);
    lv_style_set_transform_pivot_y(&st_ptile, PANEL_TILE_H / 2);
    lv_style_set_transition(&st_ptile, &st_tr);
    lv_style_init(&st_ptile_f);
    lv_style_set_transform_zoom(&st_ptile_f, FOCUS_ZOOMS[1]);
    lv_style_set_translate_y(&st_ptile_f, -FOCUS_LIFT);
    lv_style_set_outline_width(&st_ptile_f, 4);
    lv_style_set_outline_opa(&st_ptile_f, LV_OPA_COVER);
    lv_style_set_shadow_width(&st_ptile_f, 24);
    lv_style_set_shadow_ofs_y(&st_ptile_f, 14);
    lv_style_set_shadow_opa(&st_ptile_f, 0x59);

    /* The sheen is an overlay above the icon, on the rows below the dock only. A child image, not
     * the tile's bg_img: under the tile's radius LVGL draws a bg image through a corner mask whose
     * texture it builds mid-frame on a cache miss, and that frame showed a grey block. */
    lv_style_init(&st_sheen);
    lv_style_set_img_opa(&st_sheen, LV_OPA_TRANSP);
    lv_style_init(&st_sheen_f);
    lv_style_set_img_opa(&st_sheen_f, LV_OPA_COVER);

    /* control centre surfaces: #1c223057 with inset rims; focus = solid white, text #111 */
    lv_style_init(&st_cc);
    lv_style_set_bg_color(&st_cc, lv_color_hex(0x1c2230));
    lv_style_set_bg_opa(&st_cc, 0x57);
    lv_style_set_text_color(&st_cc, lv_color_hex(C_TEXT));
    lv_style_init(&st_cc_f);
    lv_style_set_bg_color(&st_cc_f, lv_color_white());
    lv_style_set_bg_opa(&st_cc_f, LV_OPA_COVER);
    lv_style_set_text_color(&st_cc_f, lv_color_hex(0x111111));
}

/* Settings > Appearance > Focus highlight: how much a focused tile grows; subtle also softens
 * its shadow (html.focus-subtle .tile--focused .tile__box: 0 14px 24px #00000052). */
static void styles_focus_apply(void) {
    int z = FOCUS_ZOOMS[prefs.focus];
    lv_style_set_transform_zoom(&st_tile_f, z);
    lv_style_set_transform_zoom(&st_ptile_f, z);
    lv_style_set_shadow_width(&st_tile_f, prefs.focus == 0 ? 24 : 34);
    lv_style_set_shadow_ofs_y(&st_tile_f, prefs.focus == 0 ? 14 : 22);
    lv_style_set_shadow_opa(&st_tile_f, prefs.focus == 0 ? 0x52 : 0x73);
    lv_obj_report_style_change(&st_tile_f);
    lv_obj_report_style_change(&st_ptile_f);
}

/* ================================================================ UI state */

enum { K_LEFT, K_RIGHT, K_UP, K_DOWN, K_OK, K_BACK, K_HOLD, K_NUM1 };   /* K_NUM1 + n-1: number keys 1-6 */

typedef enum { Z_ROWS, Z_TOP, Z_PANEL, Z_CC, Z_CTX, Z_FOLDER, Z_SETTINGS, Z_SWITCHER } zone_t;

typedef struct {
    lv_obj_t *cont, *track, *label;
    ref_t it[MAX_APPS];
    lv_obj_t *obj[MAX_APPS], *dot[MAX_APPS];
    int n, col, kind;          /* kind: 0 Apps (tray), 1 More apps, 2 Recent */
    int track_x;
    bool shown;
} row_t;

static struct {
    zone_t zone;
    bool engaged;              /* false = lumen's rest state: nothing focused until a key */
    bool dock_hidden;
    bool moving;
    int row, top;
    int panel, cc_r, cc_x, ctx, ctx_from_folder, folder, folder_sel;   /* cc_x: unit of the 4-unit CC row */
    bool lower;
} ui;

static row_t rows[3];
static int nrows;
static lv_obj_t *scr, *rows_cont, *status_cont, *status_wx, *status_wx_icon, *status_wx_text, *topbar_lbl[2], *pill_icon[2];
static lv_obj_t *dockhint, *dockdim, *movehint, *toast, *overlay, *settings_layer;
static lv_obj_t *focused;     /* the object currently wearing LV_STATE_FOCUSED */
static lv_obj_t *labelled;    /* the tile whose name is shown under it */
static uint32_t toast_until;

static bool ui_busy(void);
static bool ui_lower(void) { return ui.lower; }
static bool ui_dock_hidden(void) { return ui.dock_hidden; }

/* Every focusable object is tagged so a pointer hit can be mapped back to the focus model. */
#define TAG(z, a, b) ((void *) (intptr_t) (((z) + 1) << 16 | (a) << 8 | (b)))
#define TAG_BACKDROP ((void *) (intptr_t) 0x7fff0000)

static void show_toast(const char *msg, bool error) {
    lv_label_set_text(lv_obj_get_child(toast, 0), msg);
    lv_obj_set_style_text_color(lv_obj_get_child(toast, 0), lv_color_hex(error ? 0xe0564f : 0xAAAAAA), 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    toast_until = SDL_GetTicks() + TOAST_MS;
}

static void focus_obj(lv_obj_t *o) {
    if (focused == o) return;
    if (focused) set_state_tree(focused, LV_STATE_FOCUSED, false);
    focused = o;
    if (o) set_state_tree(o, LV_STATE_FOCUSED, true);
}

/* Call before deleting `root`. Only a focus inside it is forgotten: a focus outside (the tile a
 * menu was opened from) keeps its state, and the next focus_obj must still clear it, or that
 * tile stays zoomed next to the new one. */
static void focus_forget(lv_obj_t *root) {
    for (lv_obj_t *o = focused; o; o = lv_obj_get_parent(o))
        if (o == root) { focused = NULL; return; }
}

/* ================================================================ tiles */

static lv_obj_t *mini_tile(lv_obj_t *parent, int app, int x, int y, int w, int h) {
    lv_obj_t *m = box(parent, x, y, w, h);
    lv_obj_set_style_radius(m, 10, 0);
    lv_obj_set_style_bg_color(m, apps[app].has_color ? apps[app].color : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(m, apps[app].has_color ? LV_OPA_COVER : 0x24, 0);
    if (apps[app].has_img) {
        lv_obj_t *i = image(m, apps[app].img);
        lv_img_set_zoom(i, (uint16_t) (256 * (h * 0.8f) / apps[app].img->header.h));
        lv_obj_center(i);
    }
    return m;
}

/* One tile of lumen's `st` component. `small` = the 200x120 panel variant. */
static lv_obj_t *make_tile(lv_obj_t *parent, ref_t r, int x, int y, bool small, void *tag) {
    int w = small ? PANEL_TILE_W : TILE_W, h = small ? PANEL_TILE_H : TILE_H;
    lv_obj_t *t = box(parent, x, y, w, h);
    lv_obj_add_style(t, small ? &st_ptile : &st_tile, 0);
    lv_obj_add_style(t, small ? &st_ptile_f : &st_tile_f, LV_STATE_FOCUSED);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(t, tag);
    if (r.kind == IT_APP) {
        app_t *a = &apps[r.ref];
        if (a->has_color) { lv_obj_set_style_bg_color(t, a->color, 0); lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0); }
        else lv_obj_set_style_bg_opa(t, 0x2e, LV_STATE_FOCUSED);   /* --color-tile-bg-focus .18 */
        if (a->has_img) {
            lv_obj_t *i = image(t, a->img);
            if (small) lv_img_set_zoom(i, 256 * PANEL_TILE_H / TILE_H);
            lv_obj_center(i);
        } else {
            char s[2] = { (char) ((a->title[0] >= 'a' && a->title[0] <= 'z') ? a->title[0] - 32 : a->title[0]), 0 };
            lv_obj_center(label(t, &geist_64s, C_TEXT, s));
        }
    } else if (r.kind == IT_FOLDER) {
        /* .tile__folder: a 2x2 preview of the first four apps, 76 % x 72 % of the tile */
        lv_obj_set_style_bg_color(t, lv_color_hex(0x3b4256), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        int fw = w * 76 / 100, fh = h * 72 / 100, mw = fw * 47 / 100, mh = fh * 47 / 100;
        for (int i = 0; i < folder_n[r.ref] && i < 4; i++)
            mini_tile(t, folder_items[r.ref][i], w * 12 / 100 + (i % 2) * (fw - mw), h * 14 / 100 + (i / 2) * (fh - mh), mw, mh);
    } else {
        /* input: lumen's glyph image at 64 % of the tile; we draw the inputs line glyph */
        lv_obj_t *i = image(t, &ic_inputs_76);
        lv_obj_set_style_img_opa(i, 0xd0, 0);
        lv_obj_center(i);
    }
    return t;
}

static const char *ref_title(ref_t r) {
    if (r.kind == IT_APP) return apps[r.ref].title;
    if (r.kind == IT_FOLDER) return FOLDER_NAMES[r.ref];
    return inputs[r.ref].label;
}

/* ================================================================ rows */

static int row_width(const row_t *R) { return R->n ? R->n * TILE_STEP - TILE_GAP : 0; }

/* Track position for the selected column (lumen Xn: centred, clamped; rows after the first
 * start 42px in when they fit). */
static int row_track_x(const row_t *R) {
    int M = row_width(R);
    if (M <= ROW_VIEW_W) return R->kind == 0 ? (SCR_W - M) / 2 : EDGE + 42;
    int x = R->col * TILE_STEP + TILE_W / 2 - ROW_VIEW_W / 2;
    return EDGE - clampi(x, 0, M - ROW_VIEW_W);
}

static void row_label_show(row_t *R, bool on) {
    if (!on || !R->n) { lv_obj_set_style_opa(R->label, 0, 0); return; }
    lv_label_set_text(R->label, ref_title(R->it[R->col]));
    lv_obj_set_pos(R->label, R->col * TILE_STEP - (TILE_GAP - 8) / 2, TILE_H + 26);
    lv_obj_set_style_opa(R->label, 0, 0);
    anim_opa(R->label, LV_OPA_COVER, FOCUS_MS);
}

static void row_fill(int ri, int kind, const ref_t *items, int n) {
    row_t *R = &rows[ri];
    R->kind = kind;
    R->n = n;
    R->col = clampi(R->col, 0, n - 1);
    lv_obj_clean(R->track);
    for (int i = 0; i < n; i++) {
        R->it[i] = items[i];
        R->obj[i] = make_tile(R->track, items[i], i * TILE_STEP, 0, false, TAG(Z_ROWS, ri, i));
        if (R->kind != 0) {    /* lumen .row:not(.row--dock) .tile--focused .tile__box:after */
            lv_obj_t *sh = image(R->obj[i], &sheen_tile);
            lv_obj_add_style(sh, &st_sheen, 0);
            lv_obj_add_style(sh, &st_sheen_f, LV_STATE_FOCUSED);
        }
        /* .tile__dot:before: 8px rgba(255,255,255,.92), 0 0 6px rgba(0,0,0,.45), shown while running */
        lv_obj_t *d = box(R->track, i * TILE_STEP + TILE_W / 2 - 4, -24 - 4, 8, 8);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_color(d, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(d, 6, 0);
        lv_obj_set_style_shadow_opa(d, 0x73, 0);
        lv_obj_set_style_opa(d, 0, 0);
        R->dot[i] = d;
    }
    /* .tile__label: under the focused tile, 19px/600, two lines, centred on the tile + gap */
    R->label = label(R->track, &geist_19s, C_TEXT, "");
    lv_obj_set_width(R->label, TILE_W + TILE_GAP - 8);
    lv_label_set_long_mode(R->label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(R->label, lv_font_get_line_height(&geist_19s) * 2);
    lv_obj_set_style_text_align(R->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(R->label, 0, 0);
    lv_obj_set_width(R->track, row_width(R) > 0 ? row_width(R) : 1);
    R->track_x = row_track_x(R);
    lv_obj_set_x(R->track, R->track_x);
}

static void tray_refresh(void);
static void rows_layout(bool animate);

/* Rebuild the rows from the model (after a load, a remove, a folder change, a launch). */
static void rows_build(void) {
    static ref_t U[MAX_APPS + NFOLDERS], rec[RECENT_MAX];
    int n = home_list(U), nr = 0;
    for (int i = 0; i < nrecent; i++) rec[nr++] = (ref_t) { IT_APP, recent[i] };
    focus_obj(NULL);
    labelled = NULL;
    nrows = 0;
    int per = prefs.apps_row;
    row_fill(nrows++, 0, U, n < per ? n : per);
    if (n > per) row_fill(nrows++, 1, U + per, n - per);
    if (nr) row_fill(nrows++, 2, rec, nr);
    for (int r = 0; r < 3; r++) {
        if (r < nrows) lv_obj_clear_flag(rows[r].cont, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(rows[r].cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui.row >= nrows) ui.row = nrows - 1;
    tray_refresh();
    rows_layout(false);
}

static lv_obj_t *cur_tile(void) {
    row_t *R = &rows[ui.row];
    return R->n ? R->obj[R->col] : NULL;
}

/* Place rows for the current zone/row: lower mode, hidden rows, dimmed unfocused rows. */
static void rows_layout(bool animate) {
    bool in_rows = ui.zone == Z_ROWS || ui.zone == Z_CTX || ui.zone == Z_FOLDER;
    bool lower = in_rows && ui.row >= 1;
    /* lumen-performance.css: rows move by translate3d in 160 ms (-300 lower, +1300 hidden). lumen
     * keeps -300 for every lower row, which leaves the Recent row off-screen; we follow the focus. */
    int y = ROWS_TOP - (lower ? LOWER_SHIFT + (ui.row - 1) * ROW_PITCH : 0);
    if (ui.dock_hidden) y += 1300;
    anim(rows_cont, ax_y, lv_obj_get_y(rows_cont), y, animate ? 160 : 0, path_lumen);
    bool veil = lower && !ui.dock_hidden;            /* .lowerveil is display:none with the dock away */
    if (veil != ui.lower) {
        ui.lower = veil;
        lv_obj_t *v = layers[L_VEIL].img[layers[L_VEIL].front];
        anim_opa(v, veil ? LV_OPA_COVER : 0, animate ? 220 : 0);
    }
    anim_opa(dockdim, ui.dock_hidden && prefs.dock_dim ? 0x73 : 0, animate ? 400 : 0);   /* .dockdim .45, .4s */
    for (int r = 0; r < nrows; r++) {
        row_t *R = &rows[r];
        bool show = r == 0 || (in_rows && ui.row >= r);
        if (show != R->shown) {
            R->shown = show;
            anim(R->cont, ax_ty, show ? 26 : 0, show ? 0 : 26, animate ? (show ? 120 : 100) : 0, NULL);
            anim_opa(R->cont, show ? LV_OPA_COVER : 0, animate ? (show ? 120 : 100) : 0);
        }
        bool row_focused = (in_rows && ui.row == r) || (!ui.engaged && r == 0);
        /* layered, as CSS opacity: plain `opa` fades each child on its own, so an icon blends
         * over its already faded tile and its square shows in another shade */
        anim(R->track, ax_opa_layered, lv_obj_get_style_opa_layered(R->track, 0), row_focused ? LV_OPA_COVER : UNFOCUSED_OPA,
             animate ? FOCUS_MS : 0, lv_anim_path_ease_out);
        int tx = row_track_x(R);
        if (tx != R->track_x) { anim_scroll(R->track, ax_x, lv_obj_get_x(R->track), tx, animate); R->track_x = tx; }
    }
}

static void tray_refresh(void) {
    int want = row_width(&rows[0]) + 48;
    if (want == tray_w_want) return;
    tray_w_want = want;
    for (int i = 0; i < 2; i++) layer_set(L_TRAY, layers[L_TRAY].img[i], wp_cur);
    glass_job(G_TRAY, want, TILE_H + 48);
}

/* ================================================================ topbar */

static void pill_focus(int p, bool on) {
    if (pill_focused[p] == on) return;
    pill_focused[p] = on;
    dual_t *d = &layers[L_PILL0 + p];
    layer_set(L_PILL0 + p, d->img[d->front], wp_cur);
    for (int i = 0; i < 2; i++) anim(d->img[i], ax_zoom, lv_img_get_zoom(d->img[i]), on ? PILL_ZOOM : 256, FOCUS_MS, NULL);
    anim(pill_icon[p], ax_zoom, lv_img_get_zoom(pill_icon[p]), on ? PILL_ZOOM : 256, FOCUS_MS, NULL);
}

static void topbar_update(void) {
    bool open0 = ui.zone == Z_PANEL, open1 = ui.zone == Z_CC;
    for (int p = 0; p < 2; p++) {
        bool f = ui.engaged && ui.zone == Z_TOP && ui.top == p;
        pill_focus(p, f || (p == 0 ? open0 : open1));
        anim_opa(topbar_lbl[p], f ? LV_OPA_COVER : 0, FOCUS_MS);
    }
}

/* ================================================================ focus */

static void ui_refresh(bool animate);

/* The name under the focused tile; re-faded only when the focused tile changes. */
static void labels_update(void) {
    bool want = ui.engaged && !prefs.hide_names && (ui.zone == Z_ROWS || ui.zone == Z_CTX);
    lv_obj_t *t = want ? cur_tile() : NULL;
    if (t == labelled) return;
    labelled = t;
    for (int r = 0; r < nrows; r++) row_label_show(&rows[r], t && r == ui.row);
}

static void focus_rows(void) {
    lv_obj_t *t = ui.engaged ? cur_tile() : NULL;
    focus_obj(t);
    if (t && ui.moving) lv_obj_set_style_transform_zoom(t, FOCUS_ZOOMS[prefs.focus] * 114 / 100, LV_STATE_FOCUSED);   /* .screen--moving: 1.14 more */
}

/* ================================================================ overlays: shared */

static lv_obj_t *panel_root;    /* the glass panel inside `overlay` */

static void overlay_close(void) {
    if (overlay) { focus_forget(overlay); lv_obj_del(overlay); overlay = NULL; panel_root = NULL; }
}

/* .panel-backdrop + a glass panel of size w x h where lumen anchors that panel. */
static lv_obj_t *overlay_open(int kind, int w, int h) {
    overlay_close();
    overlay = box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, 0x61, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(overlay, TAG_BACKDROP);
    if (kind < 0) {                 /* Control Center: no card, its modules carry their own glass */
        panel_root = group(overlay, CC_X, CC_Y, w, h, 80);
        return panel_root;
    }
    int x, y, r;
    const glass_style_t *st;
    glass_geom(kind, w, &x, &y, &r, &st);
    panel_root = box(overlay, x, y, w, h);
    const glass_img_t *g = glass_get(wp_cur, kind, w, h);
    lv_obj_t *gi = image(panel_root, &g->dsc);
    lv_obj_set_pos(gi, -g->ox, -g->oy);
    return panel_root;
}

/* ================================================================ Inputs panel */

static int panel_n;
static lv_obj_t *panel_obj[16];
static ref_t panel_ref[16];

/* lumen .panel__grid: 3 columns of 200x120 tiles with wrapping labels under them; a grid row
 * is as tall as its tallest label. Returns the panel height; fills each tile's y if asked. */
static int grid3_layout(const ref_t *items, int n, int *ty) {
    int y = 96, lh = lv_font_get_line_height(&geist_24s);
    for (int r = 0; r * 3 < (n ? n : 1); r++) {
        int maxh = lh;
        for (int c = 0; c < 3 && r * 3 + c < n; c++) {
            lv_point_t sz;
            lv_txt_get_size(&sz, ref_title(items[r * 3 + c]), &geist_24s, 0, 0, PANEL_TILE_W, LV_TEXT_FLAG_NONE);
            if (sz.y > maxh) maxh = sz.y;
            if (ty) ty[r * 3 + c] = y;
        }
        y += PANEL_TILE_H + 12 + maxh + 18;
    }
    return y - 18 + 14;
}

/* Inputs / folder: lumen .panel with .panel__title and the 3-column grid. */
static void grid_panel(int kind, const char *title, const ref_t *items, int n, zone_t z) {
    int ty[16];
    int h = grid3_layout(items, n, ty);
    lv_obj_t *p = overlay_open(kind, 760, h);
    lv_obj_set_pos(label(p, &geist_30s, C_TEXT, title), 36 + 6, 30);
    panel_n = n;
    for (int i = 0; i < n; i++) {
        int tx = 42 + (i % 3) * (PANEL_TILE_W + 24);
        panel_ref[i] = items[i];
        panel_obj[i] = make_tile(p, items[i], tx, ty[i], true, TAG(z, 0, i));
        lv_obj_t *l = label(p, &geist_24s, C_TEXT, ref_title(items[i]));
        lv_obj_set_width(l, PANEL_TILE_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, tx, ty[i] + PANEL_TILE_H + 12);
    }
    if (!n) lv_obj_set_pos(label(p, &geist_24m, 0xAAAAAA, "No inputs selected: add some in Settings > Sources"), 42, 100);
}

static int inputs_shown(ref_t *items) {
    int n = 0;
    for (int i = 0; i < ninputs && n < 15; i++) if (!src_is_hidden(inputs[i].app_id)) items[n++] = (ref_t) { IT_INPUT, i };
    return n;
}
static int folder_refs(int k, ref_t *items) {
    int n = folder_n[k] > 15 ? 15 : folder_n[k];
    for (int i = 0; i < n; i++) items[i] = (ref_t) { IT_APP, folder_items[k][i] };
    return n;
}

/* Ask the worker for the panels whose size depends on data (inputs, folders), so they are
 * ready before they open. */
static void panels_prefetch(void) {
    ref_t items[16];
    glass_job(G_INPUTS, 760, grid3_layout(items, inputs_shown(items), NULL));
    for (int k = 0; k < NFOLDERS; k++) if (folder_n[k]) glass_job(G_FOLDER, 760, grid3_layout(items, folder_refs(k, items), NULL));
}

static void open_inputs(void) {
    ref_t items[16];
    grid_panel(G_INPUTS, "Inputs", items, inputs_shown(items), Z_PANEL);
    ui.zone = Z_PANEL;
    ui.panel = 0;
}

static void open_folder(int k) {
    ref_t items[16];
    grid_panel(G_FOLDER, FOLDER_NAMES[k], items, folder_refs(k, items), Z_FOLDER);
    ui.zone = Z_FOLDER;
    ui.folder = k;
    ui.folder_sel = 0;
}

/* ================================================================ context menu */

/* lumen 1.1.4 long-press menu on an app tile in the Apps / More apps rows: Recent Apps, Move,
 * Uninstall App (third-party apps only, OK twice), Remove from Home, Add to a folder. Inside an
 * open folder it offers Take out of Folder. */
enum { CI_RECENT, CI_MOVE, CI_UNINSTALL, CI_REMOVE, CI_ADD0, CI_ADD1, CI_ADD2, CI_TAKEOUT };
static const char *CI_LABELS[] = { "Recent Apps", "Move", "Uninstall App", "Remove from Home",
                                   "Add to Games", "Add to Tools", "Add to Media", "Take out of Folder" };
static int ctx_n, ctx_ids[7];
static lv_obj_t *ctx_rows[7], *ctx_labels[7];
static bool ctx_confirm;

/* .panel--menu: 16px padding, 76px rows 6px apart */
static int ctx_height(int n) { return 16 * 2 + n * 82 - 6; }

/* lumen Si: never offer to uninstall the TV's own apps, lumen itself or the Homebrew Channel */
static bool app_removable(const char *id) {
    static const char *const SYS[] = { "com.ammar027.lumen", "com.samrenfrew.lumen", "com.webos.", "com.palm.", "com.lge.", SELF_ID };
    for (const char *c = id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-')) return false;
    for (int i = 0; i < COUNT(SYS); i++) if (!strncmp(id, SYS[i], strlen(SYS[i]))) return false;
    return strcmp(id, "org.webosbrew.hbchannel") != 0;
}

static void open_ctx(bool from_folder) {
    ref_t r = rows[ui.row].it[rows[ui.row].col];
    ctx_n = 0;
    if (from_folder) ctx_ids[ctx_n++] = CI_TAKEOUT;
    else {
        ctx_ids[ctx_n++] = CI_RECENT;
        ctx_ids[ctx_n++] = CI_MOVE;
        if (app_removable(apps[r.ref].id)) ctx_ids[ctx_n++] = CI_UNINSTALL;
        for (int c = CI_REMOVE; c <= CI_ADD2; c++) ctx_ids[ctx_n++] = c;
    }
    lv_obj_t *p = overlay_open(G_CTX, 440, ctx_height(ctx_n));
    for (int i = 0; i < ctx_n; i++) {
        lv_obj_t *row = box(p, 16, 16 + i * 82, 408, 76);
        lv_obj_add_style(row, &st_cc, 0);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_add_style(row, &st_cc_f, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(row, 38, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, TAG(Z_CTX, 0, i));
        lv_obj_t *l = label(row, &geist_28m, C_TEXT, CI_LABELS[ctx_ids[i]]);
        lv_obj_set_style_text_color(l, lv_color_hex(0x111111), LV_STATE_FOCUSED);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 24, 0);
        ctx_rows[i] = row;
        ctx_labels[i] = l;
    }
    ui.ctx_from_folder = from_folder;
    ui.zone = Z_CTX;
    ui.ctx = 0;
    ctx_confirm = false;
}

static void ctx_confirm_reset(void) {
    if (!ctx_confirm) return;
    ctx_confirm = false;
    for (int i = 0; i < ctx_n; i++) if (ctx_ids[i] == CI_UNINSTALL) lv_label_set_text(ctx_labels[i], CI_LABELS[CI_UNINSTALL]);
}

/* ================================================================ running apps */

/* applicationManager/running, polled every 5 s while the launcher is in front (lumen: first
 * poll after 2.5 s). Feeds the dots over the tiles and the Recent Apps switcher. */
static char running_ids[32][96];
static int nrunning;

static bool app_running_id(const char *id) {
    for (int i = 0; i < nrunning; i++) if (!strcmp(running_ids[i], id)) return true;
    return false;
}

static bool ref_running(ref_t r) {
    if (r.kind == IT_APP) return app_running_id(apps[r.ref].id);
    if (r.kind == IT_FOLDER)                       /* a folder is running when any child is */
        for (int i = 0; i < folder_n[r.ref]; i++) if (app_running_id(apps[folder_items[r.ref][i]].id)) return true;
    return false;
}

/* .tile__dot: an 8 px dot 24 px above the tile, lifted 8 px more over the focused tile. */
static void dots_update(void) {
    lv_obj_t *f = focused;
    for (int r = 0; r < nrows; r++)
        for (int c = 0; c < rows[r].n; c++) {
            lv_obj_t *d = rows[r].dot[c];
            bool on = prefs.show_recents && ref_running(rows[r].it[c]);
            lv_opa_t want = on ? 0xeb : 0;
            if (lv_obj_get_style_opa(d, 0) != want) anim_opa(d, want, FOCUS_MS);
            int y = rows[r].obj[c] == f ? -32 - 4 : -24 - 4;
            if (lv_obj_get_y(d) != y) anim(d, ax_y, lv_obj_get_y(d), y, FOCUS_MS, NULL);
        }
}

static void running_poll(lv_timer_t *t) {
    (void) t;
    if (!app_away) rq_send(RQ_RUNNING, false, "luna://com.webos.applicationManager/running", "{}", NULL);
}

/* ================================================================ Recent Apps switcher */

/* lumen 1.1.4 js(): full-screen list of the running apps (lumen itself left out), opened from
 * the long-press menu. Left/Right pick a card, Up closes that app, Down reaches Close all. */
static lv_obj_t *switcher_layer, *sw_cards[8], *sw_all, *sw_strip;
static char sw_ids[8][96];
static int sw_n, sw_sel;
static bool sw_on_all;

static void switcher_build(void) {
    lv_obj_clean(switcher_layer);
    lv_obj_t *t = label(switcher_layer, &geist_44b, 0xFFFFFF, "Recent Apps");
    lv_obj_set_width(t, SCR_W);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, 110);
    int strip_y = 110 + lv_font_get_line_height(&geist_44b) + 70;
    if (!sw_n) {
        lv_obj_t *e = label(switcher_layer, &geist_34m, 0xFFFFFF, "No other apps are open");
        lv_obj_set_style_text_opa(e, 0xb3, 0);
        lv_obj_set_width(e, SCR_W);
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(e, 0, strip_y + 200);
        sw_all = NULL;
        return;
    }
    /* .switcher__cards: 400px cards 56 apart, padded 40/60, at most 1760 wide (a 4th card clips) */
    int shown = sw_n > 4 ? 4 : sw_n, w = shown * 400 + (shown - 1) * 56 + 120;
    if (w > 1760) w = 1760;
    sw_strip = box(switcher_layer, (SCR_W - w) / 2, strip_y, w, 225 + 22 + 40 + 80);
    lv_obj_clear_flag(sw_strip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (int i = 0; i < sw_n; i++) {
        int a = app_by_id(sw_ids[i]);
        lv_obj_t *card = group(sw_strip, 60 + i * 456, 40, 400, 225 + 22 + 40, 40);
        lv_obj_set_style_opa(card, 0xd9, 0);
        lv_obj_set_style_opa(card, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_transform_pivot_x(card, 200, 0);
        lv_obj_set_style_transform_pivot_y(card, 112, 0);
        lv_obj_set_style_transform_zoom(card, 282, LV_STATE_FOCUSED);   /* scale(1.1) */
        /* .switcher__art: 400x225 r22, the app's colour and icon; focused: 5px white ring */
        lv_obj_t *art = box(card, 0, 0, 400, 225);
        lv_obj_set_style_radius(art, 22, 0);
        lv_obj_set_style_bg_color(art, a >= 0 && apps[a].has_color ? apps[a].color : lv_color_hex(0x3b4256), 0);
        lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(art, lv_color_black(), 0);
        lv_obj_set_style_shadow_width(art, 44, 0);
        lv_obj_set_style_shadow_ofs_y(art, 20, 0);
        lv_obj_set_style_shadow_opa(art, 0x73, 0);
        lv_obj_set_style_outline_color(art, lv_color_white(), 0);
        lv_obj_set_style_outline_width(art, 5, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(art, LV_OPA_COVER, LV_STATE_FOCUSED);
        if (a >= 0 && apps[a].has_img) {
            lv_obj_t *ic = image(art, apps[a].img);
            lv_img_set_zoom(ic, 512);
            lv_obj_center(ic);
        } else {
            char m[2] = { a >= 0 ? apps[a].title[0] : sw_ids[i][0], 0 };
            if (m[0] >= 'a' && m[0] <= 'z') m[0] -= 32;
            lv_obj_center(label(art, &geist_96b, 0xFFFFFF, m));
        }
        lv_obj_t *n = label(card, &geist_30s, 0xFFFFFF, a >= 0 ? apps[a].title : sw_ids[i]);
        lv_obj_set_width(n, 400);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(n, 0, 225 + 22);
        sw_cards[i] = card;
    }
    int y = strip_y + 225 + 22 + 40 + 80 + 36;
    lv_obj_t *h = label(switcher_layer, &geist_26m, 0xFFFFFF, "Up closes an app, OK opens it");
    lv_obj_set_style_text_opa(h, 0x99, 0);
    lv_obj_set_width(h, SCR_W);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h, 0, y);
    /* .switcher__all: pill, 18px 56px, 32px, rgba(255,255,255,.14); focused white with #111 */
    sw_all = box(switcher_layer, 0, y + 34 + 60, 0, 0);
    lv_obj_t *l = label(sw_all, &geist_32m, 0xFFFFFF, "Close all");
    lv_obj_set_style_text_color(l, lv_color_hex(0x111111), LV_STATE_FOCUSED);
    int lw = lv_txt_get_width("Close all", 9, &geist_32m, 0, 0);
    lv_obj_set_size(sw_all, lw + 112, lv_font_get_line_height(&geist_32m) + 36);
    lv_obj_set_x(sw_all, (SCR_W - lw - 112) / 2);
    lv_obj_center(l);
    lv_obj_set_style_radius(sw_all, 999, 0);
    lv_obj_set_style_bg_color(sw_all, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sw_all, 0x24, 0);
    lv_obj_set_style_bg_opa(sw_all, LV_OPA_COVER, LV_STATE_FOCUSED);
}

static void switcher_focus(void) {
    if (!sw_n) { focus_obj(NULL); return; }
    focus_obj(sw_on_all ? sw_all : sw_cards[sw_sel]);
}

static void switcher_fill_from_running(void) {
    sw_n = 0;
    for (int i = 0; i < nrunning && sw_n < 8; i++)
        if (strcmp(running_ids[i], SELF_ID)) snprintf(sw_ids[sw_n++], 96, "%.95s", running_ids[i]);
    sw_sel = clampi(sw_sel, 0, sw_n - 1);
}

static void open_switcher(void) {
    switcher_layer = box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(switcher_layer, lv_color_hex(0x080a12), 0);
    lv_obj_set_style_bg_opa(switcher_layer, 0xdb, 0);              /* rgba(8,10,18,.86) */
    sw_sel = 0;
    sw_on_all = false;
    switcher_fill_from_running();
    switcher_build();
    ui.zone = Z_SWITCHER;
    running_poll(NULL);                                            /* refreshed when it opens */
}

static void switcher_close(void) {
    if (!switcher_layer) return;
    focus_forget(switcher_layer);
    lv_obj_del(switcher_layer);
    switcher_layer = NULL;
    ui.zone = Z_ROWS;
}

static void close_app(const char *id) {
    char p[160];
    snprintf(p, sizeof(p), "{\"id\":\"%.95s\"}", id);
    rq_send(RQ_ACT, true, "luna://com.webos.applicationManager/closeByAppId", p, "Couldn't close the app");
}

static void switcher_key(int k) {
    if (k == K_BACK || (!sw_n && k == K_OK)) { switcher_close(); return; }
    if (!sw_n) return;
    if (k == K_LEFT && !sw_on_all) sw_sel = clampi(sw_sel - 1, 0, sw_n - 1);
    else if (k == K_RIGHT && !sw_on_all) sw_sel = clampi(sw_sel + 1, 0, sw_n - 1);
    else if (k == K_DOWN) sw_on_all = true;
    else if (k == K_UP && sw_on_all) sw_on_all = false;
    else if (k == K_UP) {                                          /* close the focused app */
        close_app(sw_ids[sw_sel]);
        memmove(sw_ids[sw_sel], sw_ids[sw_sel + 1], (size_t) (sw_n - sw_sel - 1) * 96);
        sw_n--;
        sw_sel = clampi(sw_sel, 0, sw_n - 1);
        focus_forget(switcher_layer);
        switcher_build();
    } else if (k == K_OK && sw_on_all) {
        for (int i = 0; i < sw_n; i++) close_app(sw_ids[i]);
        show_toast("All apps closed", false);
        switcher_close();
        return;
    } else if (k == K_OK) {
        char id[96];
        snprintf(id, sizeof(id), "%s", sw_ids[sw_sel]);
        switcher_close();
        launch_app_id(id);
        return;
    }
    switcher_focus();
}

/* ================================================================ control centre */

/* lumen 1.1.4 Control Center: no card, a 4-unit grid of glass modules floating top-right
 * (--cc-u 90px, --cc-gap 18px). Tiles open a list of values in place; circles act at once. */
enum { CC_SOUND, CC_PICTURE, CC_SLEEP, CC_ART, CC_HOME, CC_SCREENOFF, CC_SETTINGS, CC_BACKLIGHT, CC_EYE, CC_N };
/* lumen `Me`: rows of cells, each spanning 1-3 units of the 4-unit row */
static const struct { int id, span; } CC_ROWS[4][4] = {
    { { CC_SOUND, 2 }, { CC_PICTURE, 2 } }, { { CC_SLEEP, 4 } },
    { { CC_ART, 1 }, { CC_HOME, 1 }, { CC_SCREENOFF, 1 }, { CC_SETTINGS, 1 } },
    { { CC_BACKLIGHT, 3 }, { CC_EYE, 1 } } };
static const int CC_ROW_N[4] = { 2, 1, 4, 2 };

static const char *PIC_MODES[] = { "vivid", "normal", "eco", "cinema", "sports", "game", "filmMaker" };
static const char *PIC_LABELS[] = { "Vivid", "Standard", "Eco", "Cinema", "Sports", "Game", "FILMMAKER" };
static const char *SND_MODES[] = { "standard", "news", "movie", "music", "sports", "game", "aiSound" };
static const char *SND_LABELS[] = { "Standard", "Clear Voice", "Cinema", "Music", "Sports", "Game Optimizer", "AI Sound" };
static const int SLEEP_STEPS[] = { 0, 15, 30, 60, 90, 120 };

static struct {
    char picture[24], sound[24];
    int sleep_min, backlight;
    bool eye;
} tv = { "normal", "standard", 0, 100, false };

static int idx_of(const char *const *list, int n, const char *v) {
    for (int i = 0; i < n; i++) if (!strcmp(list[i], v)) return i;
    return -1;
}

/* Module rectangle relative to the grid's top-left. */
static void cc_rect(int id, int *x, int *y, int *w) {
    for (int r = 0; r < 4; r++) {
        int u = 0;
        for (int c = 0; c < CC_ROW_N[r]; c++) {
            if (CC_ROWS[r][c].id == id) {
                int span = CC_ROWS[r][c].span;
                *x = u * (CC_U + CC_GAP);
                *y = r * (CC_U + CC_GAP);
                *w = span * CC_U + (span - 1) * CC_GAP;
                return;
            }
            u += CC_ROWS[r][c].span;
        }
    }
}

static struct {
    lv_obj_t *mod[CC_N], *glass[CC_N], *icon[CC_N], *sub[CC_N];
    lv_obj_t *grid, *list, *caption, *fill, *num;
    lv_obj_t *opt[8], *opt_glass[8], *opt_icon[8];
    int nopt, opt_cell, opt_sel;
    bool listing;
    uint32_t last_down, bl_due;    /* double-Down closes; backlight writes are debounced */
} cc;

static const char *cc_sub_text(int id, char *buf, int n) {
    int i;
    switch (id) {
        case CC_SOUND: i = idx_of(SND_MODES, COUNT(SND_MODES), tv.sound); return i >= 0 ? SND_LABELS[i] : tv.sound;
        case CC_PICTURE: i = idx_of(PIC_MODES, COUNT(PIC_MODES), tv.picture); return i >= 0 ? PIC_LABELS[i] : tv.picture;
        default: if (!tv.sleep_min) return "Off"; snprintf(buf, n, "%d min", tv.sleep_min); return buf;
    }
}

static bool cc_is_on(int id) {
    return (id == CC_SLEEP && tv.sleep_min > 0) || (id == CC_EYE && tv.eye);
}

static int cc_focused_id(void) {
    int u = 0;
    for (int c = 0; c < CC_ROW_N[ui.cc_r]; c++) {
        if (ui.cc_x < u + CC_ROWS[ui.cc_r][c].span) return CC_ROWS[ui.cc_r][c].id;
        u += CC_ROWS[ui.cc_r][c].span;
    }
    return CC_ROWS[ui.cc_r][CC_ROW_N[ui.cc_r] - 1].id;
}

/* Swap a module to its normal / focused glass; skipped when it already shows that one. */
static void cc_set_glass(lv_obj_t *img, int kind, int w, int h) {
    const glass_img_t *g = glass_get(wp_cur, kind, w, h);
    if (lv_img_get_src(img) == &g->dsc) return;
    lv_img_set_src(img, &g->dsc);
    lv_obj_set_pos(img, -g->ox, -g->oy);
}

static void cc_refresh(void) {
    if (ui.zone != Z_CC || !overlay) return;
    char b[32];
    int f = cc.listing ? -1 : cc_focused_id();
    for (int id = 0; id < CC_N; id++) {
        int x, y, w;
        cc_rect(id, &x, &y, &w);
        cc_set_glass(cc.glass[id], G_CCM + id * 2 + (id == f), w, CC_U);
        if (cc.sub[id]) lv_label_set_text(cc.sub[id], cc_sub_text(id, b, sizeof(b)));
        bool on = cc_is_on(id);
        lv_obj_set_style_img_recolor_opa(cc.icon[id], on ? LV_OPA_COVER : 0, 0);
        lv_obj_set_style_img_opa(cc.icon[id], on ? LV_OPA_COVER : 0xe6, 0);
    }
    focus_obj(f >= 0 ? cc.mod[f] : cc.opt[cc.opt_sel]);
    lv_obj_set_width(cc.fill, (3 * CC_U + 2 * CC_GAP) * tv.backlight / 100);
    snprintf(b, sizeof(b), "%d", tv.backlight);
    lv_label_set_text(cc.num, b);
    /* .cc__caption: the focused circle's name */
    const char *cap = f == CC_ART ? "Screensaver" : f == CC_HOME ? "LG Home" : f == CC_SCREENOFF ? "Screen off"
                    : f == CC_SETTINGS ? "Settings" : f == CC_EYE ? (tv.eye ? "Eye Comfort: On" : "Eye Comfort: Off") : "";
    lv_label_set_text(cc.caption, cap);
    if (cc.listing)
        for (int i = 0; i < cc.nopt; i++) cc_set_glass(cc.opt_glass[i], G_CCO + i * 2 + (i == cc.opt_sel), CC_W, CC_OPT_H);
}

/* A glass module: a moving group (zoom 1.1 on focus needs the ext-draw margin for its
 * shadow) holding the pre-composited glass image, then glyph and text. */
static lv_obj_t *cc_module(lv_obj_t *parent, int x, int y, int w, int h, int zoom, void *tag, lv_obj_t **glass) {
    lv_obj_t *m = group(parent, x, y, w, h, 40);
    lv_obj_set_style_transform_pivot_x(m, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(m, h / 2, 0);
    lv_obj_set_style_transform_zoom(m, zoom, LV_STATE_FOCUSED);
    lv_obj_set_style_transition(m, &st_tr, 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(m, tag);
    *glass = image(m, NULL);
    return m;
}

static lv_obj_t *cc_glyph(lv_obj_t *parent, const lv_img_dsc_t *ic) {
    lv_obj_t *i = image(parent, ic);
    lv_obj_set_style_img_recolor(i, lv_color_hex(CC_ON), 0);     /* .cc__mod--on svg */
    return i;
}

static void cc_tile(lv_obj_t *grid, int id, const lv_img_dsc_t *ic, const char *name) {
    int x, y, w;
    cc_rect(id, &x, &y, &w);
    lv_obj_t *m = cc_module(grid, x, y, w, CC_U, 282, TAG(Z_CC, 0, id), &cc.glass[id]);
    /* .cc__tile: padding 0 14px 0 20px; 40px glyph, 14px gap; name 21/600, sub 21 .7 */
    cc.icon[id] = cc_glyph(m, ic);
    lv_obj_align(cc.icon[id], LV_ALIGN_LEFT_MID, 20, 0);
    int lh = 25, ty = (CC_U - 2 * lh) / 2;
    lv_obj_set_pos(label(m, &geist_21s, 0xFFFFFF, name), 20 + 40 + 14, ty);
    cc.sub[id] = label(m, &geist_21m, 0xFFFFFF, "");
    lv_obj_set_style_text_opa(cc.sub[id], 0xb3, 0);
    lv_obj_set_pos(cc.sub[id], 20 + 40 + 14, ty + lh);
    cc.mod[id] = m;
}

static void cc_circle(lv_obj_t *grid, int id, const lv_img_dsc_t *ic) {
    int x, y, w;
    cc_rect(id, &x, &y, &w);
    lv_obj_t *m = cc_module(grid, x, y, CC_U, CC_U, 282, TAG(Z_CC, 0, id), &cc.glass[id]);
    cc.icon[id] = cc_glyph(m, ic);
    lv_obj_center(cc.icon[id]);
    cc.mod[id] = m;
}

static void cc_build_grid(lv_obj_t *p) {
    cc.grid = group(p, 0, 0, CC_W, 4 * (CC_U + CC_GAP) + 30, 60);
    cc_tile(cc.grid, CC_SOUND, &ic_speaker_40, "Sound");
    cc_tile(cc.grid, CC_PICTURE, &ic_picture_40, "Picture");
    cc_tile(cc.grid, CC_SLEEP, &ic_power_40, "Sleep timer");
    cc_circle(cc.grid, CC_ART, &ic_screensaver_44);
    cc_circle(cc.grid, CC_HOME, &ic_house_44);
    cc_circle(cc.grid, CC_SCREENOFF, &ic_power_44);
    cc_circle(cc.grid, CC_SETTINGS, &ic_gear_44);
    cc_circle(cc.grid, CC_EYE, &ic_eye_44);

    /* .cc__slider: 3 units wide; .cc__fill a brighter glass bar from the left; sun + Backlight + value */
    int x, y, w;
    cc_rect(CC_BACKLIGHT, &x, &y, &w);
    lv_obj_t *s = cc_module(cc.grid, x, y, w, CC_U, 282, TAG(Z_CC, 0, CC_BACKLIGHT), &cc.glass[CC_BACKLIGHT]);
    cc.fill = box(s, 0, 0, 0, CC_U);
    lv_obj_set_style_radius(cc.fill, CC_U / 2, 0);
    lv_obj_set_style_bg_color(cc.fill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cc.fill, 0x45, 0);
    lv_obj_set_style_bg_opa(cc.fill, 0x6a, LV_STATE_FOCUSED);
    cc.icon[CC_BACKLIGHT] = cc_glyph(s, &ic_sun_34);
    lv_obj_align(cc.icon[CC_BACKLIGHT], LV_ALIGN_LEFT_MID, 22, 0);
    lv_obj_align(label(s, &geist_21s, 0xFFFFFF, "Backlight"), LV_ALIGN_LEFT_MID, 22 + 34 + 12, 0);
    cc.num = label(s, &geist_21m, 0xFFFFFF, "");
    lv_obj_align(cc.num, LV_ALIGN_RIGHT_MID, -26, 0);
    cc.mod[CC_BACKLIGHT] = s;

    cc.caption = label(cc.grid, &geist_20s, 0xFFFFFF, "");
    lv_obj_set_width(cc.caption, CC_W);
    lv_obj_set_style_text_align(cc.caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(cc.caption, 0, 4 * (CC_U + CC_GAP) + 2);
}

static int cc_opts(int cell, const char **labels) {
    switch (cell) {
        case CC_SOUND: for (int i = 0; i < COUNT(SND_LABELS); i++) labels[i] = SND_LABELS[i]; return COUNT(SND_LABELS);
        case CC_PICTURE: for (int i = 0; i < COUNT(PIC_LABELS); i++) labels[i] = PIC_LABELS[i]; return COUNT(PIC_LABELS);
        default: {
            static const char *S[] = { "Off", "15 minutes", "30 minutes", "60 minutes", "90 minutes", "120 minutes" };
            for (int i = 0; i < COUNT(S); i++) labels[i] = S[i];
            return COUNT(S);
        }
    }
}

static int cc_opt_current(int cell) {
    if (cell == CC_SOUND) return idx_of(SND_MODES, COUNT(SND_MODES), tv.sound);
    if (cell == CC_PICTURE) return idx_of(PIC_MODES, COUNT(PIC_MODES), tv.picture);
    for (int i = 0; i < COUNT(SLEEP_STEPS); i++) if (SLEEP_STEPS[i] == tv.sleep_min) return i;
    return 0;
}

/* OK on a tile: lumen morphs the tile into a list of its values (330 ms, 300 ms back). The
 * grid fades and shrinks to .85 while the list fades in where the grid was. */
static void cc_list_open(int cell) {
    const char *labels[8];
    cc.nopt = cc_opts(cell, labels);
    cc.opt_cell = cell;
    cc.opt_sel = cc_opt_current(cell);
    if (cc.opt_sel < 0) cc.opt_sel = 0;
    static const lv_img_dsc_t *const ICONS[] = { &ic_speaker_34, &ic_picture_34, &ic_screensaver_34, &ic_power_34 };
    cc.list = group(panel_root, 0, 0, CC_W, cc.nopt * CC_OPT_STEP, 40);
    for (int i = 0; i < cc.nopt; i++) {
        lv_obj_t *o = cc_module(cc.list, 0, i * CC_OPT_STEP, CC_W, CC_OPT_H, 269, TAG(Z_CC, 1, i), &cc.opt_glass[i]);
        cc.opt_icon[i] = cc_glyph(o, ICONS[cell]);
        lv_obj_set_style_img_recolor_opa(cc.opt_icon[i], i == cc_opt_current(cell) ? LV_OPA_COVER : 0, 0);
        lv_obj_align(cc.opt_icon[i], LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_align(label(o, &geist_21s, 0xFFFFFF, labels[i]), LV_ALIGN_LEFT_MID, 20 + 34 + 14, 0);
        cc.opt[i] = o;
    }
    cc.listing = true;
    lv_obj_set_style_opa(cc.list, 0, 0);
    anim(cc.list, ax_opa, 0, LV_OPA_COVER, 330, path_lumen);
    anim(cc.grid, ax_opa, lv_obj_get_style_opa(cc.grid, 0), 0, 330, path_lumen);
}

static void cc_list_done(lv_anim_t *a) { lv_obj_del((lv_obj_t *) a->var); }

static void cc_apply(int cell, int i);
static void cc_list_close(bool apply) {
    if (!cc.listing) return;
    if (apply) cc_apply(cc.opt_cell, cc.opt_sel);
    cc.listing = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cc.list);
    lv_anim_set_exec_cb(&a, ax_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, 0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_ready_cb(&a, cc_list_done);
    lv_anim_start(&a);
    cc.list = NULL;
    anim(cc.grid, ax_opa, lv_obj_get_style_opa(cc.grid, 0), LV_OPA_COVER, 300, path_lumen);
}

static void open_cc(void) {
    lv_obj_t *p = overlay_open(-1, CC_W, 4 * (CC_U + CC_GAP) + 30);
    memset(&cc, 0, sizeof(cc));
    cc_build_grid(p);
    ui.zone = Z_CC;
    ui.cc_r = ui.cc_x = 0;
    cc_refresh();
    /* lumen opens it from the pill: scale(0.6) -> none, children fade in, 260 ms */
    lv_obj_set_style_transform_pivot_x(p, PILL_X[1] + PILL / 2 - CC_X, 0);
    lv_obj_set_style_transform_pivot_y(p, PILL_Y + PILL / 2 - CC_Y, 0);
    anim(p, ax_zoom_obj, 154, 256, 260, path_lumen);
    lv_obj_set_style_opa(p, 0, 0);
    anim(p, ax_opa, 0, LV_OPA_COVER, 260, path_lumen);

    /* lumen reads the live values every time the panel opens */
    rq_send(RQ_PIC, false, "luna://com.webos.settingsservice/getSystemSettings", "{\"category\":\"picture\",\"keys\":[\"pictureMode\",\"backlight\",\"eyeComfortMode\"]}", NULL);
    rq_send(RQ_SND, false, "luna://com.webos.settingsservice/getSystemSettings", "{\"category\":\"sound\",\"keys\":[\"soundMode\"]}", NULL);
    rq_send(RQ_SLEEP, false, "luna://com.webos.service.tvReservationAgent/timer/sleep/getRemainTime", "{}", NULL);
}

static void root_exec(const char *uri, const char *payload, const char *fail) {
    rq_send(RQ_ACT, true, uri, payload, fail);
}

static lv_obj_t *cc_focus_target(void) {
    return cc.listing ? cc.opt[cc.opt_sel] : cc.mod[cc_focused_id()];
}

static void set_picture_setting(const char *key, const char *value, const char *fail) {
    char p[160];
    snprintf(p, sizeof(p), "{\"category\":\"picture\",\"settings\":{\"%s\":\"%s\"}}", key, value);
    rq_send(RQ_ACT, false, "luna://com.webos.settingsservice/setSystemSettings", p, fail);
}

/* Backlight writes wait 300 ms after the last step (lumen Ts) so a held arrow is one call. */
static void cc_backlight(int v) {
    tv.backlight = clampi(v, 0, 100);
    cc.bl_due = SDL_GetTicks() + 300;
    cc_refresh();
}
static void cc_backlight_flush(void) {
    if (!cc.bl_due || SDL_GetTicks() < cc.bl_due) return;
    cc.bl_due = 0;
    char v[8];
    snprintf(v, sizeof(v), "%d", tv.backlight);
    set_picture_setting("backlight", v, "Couldn't change the backlight");
}

/* ---- screensavers (screensaver.c draws them) ---- */

/* Photos in Lumen may dim the TV (photoBrightness): the live backlight is read first, so the
 * value put back on the closing key is the user's, not one a slow reply left in `tv`. */
static int saver_bl_saved = -1;
static bool saver_dim_pending;

static void saver_dim_apply(int current) {
    saver_dim_pending = false;
    if (!saver_active()) return;
    saver_bl_saved = current;
    set_picture_setting("backlight", prefs.ph_bright == 1 ? "30" : "10", NULL);
}

/* LVGL looks for a covering object only on the active screen, so under an opaque saver on the
 * top layer it kept redrawing the home screen (the wallpaper, the dock) every saver frame.
 * The home layers are hidden once the saver has faded in. */
static uint32_t home_hidden;    /* bit n: child n of the screen was hidden here */
static void home_show(bool on) {
    uint32_t n = lv_obj_get_child_cnt(scr);
    for (uint32_t i = 0; i < n && i < 32; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, (int32_t) i);
        if (!on && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); home_hidden |= 1u << i; }
        else if (on && (home_hidden >> i & 1)) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    if (on) home_hidden = 0;
}
static void saver_cover_cb(lv_timer_t *t) {
    (void) t;
    if (saver_active()) home_show(false);
}

static void saver_closed(void) {
    home_show(true);
    saver_dim_pending = false;
    if (saver_bl_saved < 0) return;
    char v[12];
    snprintf(v, sizeof(v), "%d", saver_bl_saved);
    set_picture_setting("backlight", v, "Couldn't put the backlight back");
    saver_bl_saved = -1;
}

/* SDL-webOS answers the TV's screensaver request with "no" while SDL's screensaver is disabled
 * (its default). With the TV's own screensaver the TV keeps its idle timer, so myTV lets it run;
 * with myTV's own, myTV holds it off and runs its idle timer instead. */
static void saver_policy_apply(void) {
    if (prefs.ss_type == 0) SDL_EnableScreenSaver(); else SDL_DisableScreenSaver();
}

/* The screensaver of the chosen type: the TV's own, the photo slideshow or StandBy. */
static void saver_start(void) {
    if (prefs.ss_type == 0) {
        root_exec("luna://com.webos.service.tvpower/power/turnOnScreenSaver", "{}", "Couldn't start the screensaver (it needs the Homebrew Channel)");
        return;
    }
    saver_env_t env = { .h12 = clock_fmt == 1 || (clock_fmt == 0 && sys_12h), .ampm = prefs.show_ampm,
                        .night = prefs.sb_night, .photo_seconds = prefs.ph_secs, .print = prefs.ph_print,
                        .frosted = !prefs.pc_solid, .info = prefs.pc_info, .dim = night_now, .pc_pos = prefs.pc_pos,
                        .pc_size = prefs.pc_size, .reduce_motion = prefs.reduce_motion };
    if (prefs.show_weather) {
        pthread_mutex_lock(&wx_mtx);
        snprintf(env.weather, sizeof(env.weather), "%s", wx_text);
        pthread_mutex_unlock(&wx_mtx);
    }
    for (int i = 0; i < WP_COUNT; i++) if (wp_ready(i) && wp_px(i)) env.photos[env.nphotos++] = &wp_dsc[i];
    lv_timer_set_repeat_count(lv_timer_create(saver_cover_cb, 600, NULL), 1);   /* after the .5 s fade-in */
    if (prefs.ss_type == 2) { standby_open(&env, prefs.sb_face ? prefs.sb_face - 1 : prefs.sb_last, prefs.sb_color); return; }
    photos_open(&env);
    if (prefs.ph_bright) {
        saver_dim_pending = true;
        rq_send(RQ_PIC, false, "luna://com.webos.settingsservice/getSystemSettings", "{\"category\":\"picture\",\"keys\":[\"backlight\"]}", NULL);
    }
}

/* Any key but StandBy's arrows closes a screensaver. The face and colour left on are kept. */
static void saver_handle(int k) {
    int sk = k == K_LEFT ? SV_LEFT : k == K_RIGHT ? SV_RIGHT : k == K_UP ? SV_UP : k == K_DOWN ? SV_DOWN : SV_OTHER;
    if (!saver_key(sk)) { saver_closed(); return; }
    prefs.sb_last = standby_face();
    prefs.sb_color = standby_color();
    state_save();
}

static void cc_apply(int cell, int i) {
    char p[160];
    switch (cell) {
        case CC_SOUND:
            snprintf(tv.sound, sizeof(tv.sound), "%s", SND_MODES[i]);
            snprintf(p, sizeof(p), "{\"category\":\"sound\",\"settings\":{\"soundMode\":\"%s\"}}", tv.sound);
            rq_send(RQ_ACT, false, "luna://com.webos.settingsservice/setSystemSettings", p, "This TV didn't accept that sound mode");
            break;
        case CC_PICTURE:
            snprintf(tv.picture, sizeof(tv.picture), "%s", PIC_MODES[i]);
            set_picture_setting("pictureMode", tv.picture, "Couldn't change the picture mode");
            break;
        case CC_SLEEP:
            tv.sleep_min = SLEEP_STEPS[i];
            snprintf(p, sizeof(p), "{\"sleepTimeMinute\":%d}", tv.sleep_min);
            rq_send(RQ_ACT, false, "luna://com.webos.service.tvReservationAgent/timer/sleep/add", p, "Couldn't set the sleep timer");
            break;
    }
}

static void open_settings(void);
static void ui_close_all(void);

static void cc_activate(int id) {
    switch (id) {
        case CC_SOUND: case CC_PICTURE: case CC_SLEEP: cc_list_open(id); break;
        case CC_HOME: {
            /* the hook's poll mode would bring us back over LG Home: this asks it to let it stay */
            FILE *f = fopen("/tmp/mytv-allow-lg-home", "w");
            if (f) fclose(f);
            ui_close_all(); launch_app_id("com.webos.app.home"); return;
        }
        case CC_SETTINGS: ui_close_all(); open_settings(); return;
        case CC_ART: ui_close_all(); saver_start(); return;
        case CC_SCREENOFF:
            ui_close_all();
            root_exec("luna://com.webos.service.tvpower/power/turnOffScreen", "{}", "Couldn't turn the screen off (it needs the Homebrew Channel)");
            return;
        case CC_EYE:
            tv.eye = !tv.eye;
            set_picture_setting("eyeComfortMode", tv.eye ? "on" : "off", "Couldn't change Eye Comfort");
            break;
        default: break;
    }
    cc_refresh();
}

/* lumen Ws key handler. Returns false when the panel closed. */
static void cc_key(int k) {
    if (cc.listing) {
        if (k == K_BACK) cc_list_close(false);
        else if (k == K_OK) cc_list_close(true);
        else if (k == K_UP || k == K_DOWN) cc.opt_sel = clampi(cc.opt_sel + (k == K_UP ? -1 : 1), 0, cc.nopt - 1);
        return;
    }
    int id = cc_focused_id();
    int start = 0;                                    /* first unit of the focused cell */
    for (int c = 0, u = 0; c < CC_ROW_N[ui.cc_r]; u += CC_ROWS[ui.cc_r][c++].span)
        if (CC_ROWS[ui.cc_r][c].id == id) start = u;
    if (k == K_BACK) { overlay_close(); ui.zone = Z_TOP; return; }
    if (k == K_OK) { cc_activate(id); return; }
    if (id == CC_BACKLIGHT && (k == K_LEFT || k == K_RIGHT)) { cc_backlight(tv.backlight + (k == K_LEFT ? -5 : 5)); return; }
    if (k == K_LEFT || k == K_RIGHT) {
        int c = 0;
        while (CC_ROWS[ui.cc_r][c].id != id) c++;
        c = clampi(c + (k == K_LEFT ? -1 : 1), 0, CC_ROW_N[ui.cc_r] - 1);
        ui.cc_x = 0;
        for (int i = 0; i < c; i++) ui.cc_x += CC_ROWS[ui.cc_r][i].span;
    } else if (k == K_UP && ui.cc_r == 0) { overlay_close(); ui.zone = Z_TOP; return; }
    else if (k == K_DOWN && ui.cc_r == 3) {
        uint32_t now = SDL_GetTicks();                /* a second Down within 1 s leaves to the rows */
        if (now - cc.last_down < 1000) { overlay_close(); ui.zone = Z_ROWS; return; }
        cc.last_down = now;
    } else if (k == K_UP || k == K_DOWN) {
        ui.cc_r = clampi(ui.cc_r + (k == K_UP ? -1 : 1), 0, 3);
        ui.cc_x = start;
    }
}

/* pointer: hover focuses a module or a list option */
static void cc_pointer(int level, int i) {
    if (level == 1) { if (cc.listing) cc.opt_sel = i; return; }
    if (cc.listing) return;
    for (int r = 0; r < 4; r++)
        for (int c = 0, u = 0; c < CC_ROW_N[r]; u += CC_ROWS[r][c++].span)
            if (CC_ROWS[r][c].id == i) { ui.cc_r = r; ui.cc_x = u; }
}

static void clock_tick(lv_timer_t *t);

/* ================================================================ settings */

/* lumen 1.1.4 Settings: a tvOS-style stack of full-screen pages. The root lists Apps, Sources
 * and the option groups; a group lists its options; a multi-value option opens a page of
 * choices with ticks. Left half: a big glyph and the focused row's description; right half:
 * 920x70 pill rows at an 84 px pitch. Only what the TV's own settings do not already offer:
 * language, the 12/24-hour clock and the time zone stay in the TV menu. */

enum { SG_WALLPAPER, SG_HOME, SG_SCREENSAVER, SG_WEATHER, SG_APPEARANCE, SG_CLOCK, SG_SYSTEM, SG_N };
static const struct { const char *label; const lv_img_dsc_t *glyph; } SGROUPS[SG_N] = {
    { "Wallpaper", &ic_image_192 }, { "Home Screen", &ic_house_192 }, { "Screensaver", &ic_screensaver_192 },
    { "Weather", &ic_cloud_192 },
    { "Appearance", &ic_contrast_192 }, { "Clock", &ic_timer_192 }, { "System", &ic_gear_192 } };

enum { OT_TOGGLE, OT_CHOICE, OT_PAGE, OT_ACTION };
enum { O_WP_TYPE, O_WP_CHOOSE, O_WP_NEW, O_WP_AUTO, O_WP_DIM,
       O_HIDE_NAMES, O_APPS_ROW, O_DOCK_DIM, O_DOCK_HIDE, O_RECENTS,
       O_SS_TYPE, O_IDLE, O_PH_BRIGHT, O_PC_POS, O_PC_SIZE, O_PC_INFO, O_PC_LOOK, O_PH_SECS, O_PH_PRINT,
       O_SB_FACE, O_SB_COLOR, O_SB_NIGHT, O_SS_START,
       O_WEATHER, O_COUNTRY, O_CITY,
       O_FOCUS, O_MOTION,
       O_CLK_STYLE, O_CLK_COLOR, O_CLK_LOOK, O_CLK_ORDER, O_CLK_LAYOUT, O_CLK_SIZE, O_CLK_WEIGHT, O_CLK_POS,
       O_SHOW_DATE, O_AMPM,
       O_SYS_APPS, O_HOME_KEY, O_N };
static const struct { int group, type; const char *label, *desc; } OPTS[O_N] = {
    { SG_WALLPAPER, OT_CHOICE, "Wallpaper type", "Pictures: still pictures, the bundled paintings. Plain: no wallpaper picture." },
    { SG_WALLPAPER, OT_PAGE, "Choose wallpaper", NULL },
    { SG_WALLPAPER, OT_TOGGLE, "New picture on startup", "With several pictures ticked in Choose wallpaper, show a different one each time myTV starts. Off keeps the picture shown last." },
    { SG_WALLPAPER, OT_CHOICE, "Change wallpaper automatically", "With a Shuffle wallpaper chosen, pick a new picture every 15, 30 or 60 minutes." },
    { SG_WALLPAPER, OT_CHOICE, "Dim the wallpaper", "A darker wallpaper, easier on the eyes in a dark room. At night: between 7 pm and 6 am." },
    { SG_HOME, OT_TOGGLE, "Hide app names", "No names under the apps on the home screen, not even the one in focus. Settings still shows them." },
    { SG_HOME, OT_CHOICE, "Apps per row", "How many apps the glass bar and each row below it hold. Fewer apps per row leaves more room around each." },
    { SG_HOME, OT_TOGGLE, "Dim when the dock is hidden", "With the dock away, the wallpaper fades darker. It brightens again when the dock comes back." },
    { SG_HOME, OT_CHOICE, "Hide the dock", "Down arrow: Down on the last row slides the dock away. After idle: it hides itself after 8 seconds without a key press. Any key brings it back." },
    { SG_HOME, OT_TOGGLE, "Show recents indicator", "A dot over the apps that are open in the background." },
    { SG_SCREENSAVER, OT_CHOICE, "Screensaver type", "The TV's own: LG's screensaver, on the timer in the TV's settings; the options below are off. Photos in myTV: the paintings with the clock and weather. StandBy: a full-screen clock, after Apple's StandBy." },
    { SG_SCREENSAVER, OT_CHOICE, "Screensaver after idle", "Starts the screensaver after this long without a key press, while myTV is in front." },
    { SG_SCREENSAVER, OT_CHOICE, "Brightness", "How bright the TV is while the photo screensaver shows. Dim and Very dim lower the TV's backlight and put it back when you press a key." },
    { SG_SCREENSAVER, OT_CHOICE, "Clock position", "Where the clock sits on the photo screensaver: bottom, top or centre, left, middle or right, or off." },
    { SG_SCREENSAVER, OT_CHOICE, "Clock size on photos", "How large the clock is on the photo screensaver." },
    { SG_SCREENSAVER, OT_TOGGLE, "Date and weather on photos", "The date and the weather under the clock on the photo screensaver." },
    { SG_SCREENSAVER, OT_CHOICE, "Clock look on photos", "Frosted: see-through white digits, like the iOS lock screen. Solid: plain white digits." },
    { SG_SCREENSAVER, OT_CHOICE, "Time on each photo", "How long each picture stays on screen before the next one fades in." },
    { SG_SCREENSAVER, OT_TOGGLE, "Print look", "Softer blacks and slightly greyer whites, like a print: good for paintings." },
    { SG_SCREENSAVER, OT_CHOICE, "StandBy face", "The face StandBy opens on: Digital, Analog, Calendar, Solar, Photos, Weather, Fade or Flow, or the one you used last. Left and Right change it while StandBy is up." },
    { SG_SCREENSAVER, OT_CHOICE, "StandBy colour", "The colour of the clock figures and accents. Up and Down change it while StandBy is up." },
    { SG_SCREENSAVER, OT_CHOICE, "Night Mode", "At night StandBy dims the screen and turns the figures red, as Apple's StandBy does in a dark room. Auto is 7 pm to 6 am." },
    { SG_SCREENSAVER, OT_ACTION, "Start the screensaver now", "Show the photo screensaver or StandBy full screen at once, whichever is chosen above. Press OK to return." },
    { SG_WEATHER, OT_TOGGLE, "Date and weather", "Show today's date and the temperature at the top left of the home screen. The weather comes from open-meteo.com." },
    { SG_WEATHER, OT_CHOICE, "Weather country", "Pick your country, then the city below. Auto finds your area from your internet connection (ip-api.com), which can be a few towns off." },
    { SG_WEATHER, OT_CHOICE, "Weather city", "Auto finds your area from your internet connection (ip-api.com). OK steps through a list of cities if Auto guesses wrong." },
    { SG_APPEARANCE, OT_CHOICE, "Focus highlight", "How much the focused app grows. Normal is the default; Subtle grows less; Strong grows more." },
    { SG_APPEARANCE, OT_TOGGLE, "Reduce motion", "Focus, rows and panels change at once, without sliding, fading or zooming." },
    { SG_CLOCK, OT_CHOICE, "Clock style", "How the clock looks: digital, with the date, with seconds, an analog dial, or the dial next to the digits." },
    { SG_CLOCK, OT_CHOICE, "Clock colour", "The colour of the clock." },
    { SG_CLOCK, OT_CHOICE, "Clock look", "Solid: plain digits. Frosted: see-through white digits, like the iOS lock screen." },
    { SG_CLOCK, OT_CHOICE, "Stacked clock order", "For the Stacked layout: the big time above the date and weather, or the date and weather first and the time below them." },
    { SG_CLOCK, OT_CHOICE, "Clock layout", "One row (date and time side by side), or Stacked: a big time with the long date under it and the weather with its icon below." },
    { SG_CLOCK, OT_CHOICE, "Clock size", "How large the date, time and weather block at the top is." },
    { SG_CLOCK, OT_CHOICE, "Clock weight", "How thick the letters of the date and time are." },
    { SG_CLOCK, OT_CHOICE, "Clock position", "Where the date, time and weather block sits at the top of the home screen." },
    { SG_CLOCK, OT_TOGGLE, "Show date", "The day and date before the time." },
    { SG_CLOCK, OT_TOGGLE, "AM/PM on the clock", "Show AM or PM beside the time when the TV is set to a 12-hour clock." },
    { SG_SYSTEM, OT_TOGGLE, "Show system apps", "Offer the TV's own apps (Live TV, LG Channels, AirPlay, Screen Share, Game Optimizer, TV Settings) in Settings > Apps, so you can add them to the home screen." },
    { SG_SYSTEM, OT_TOGGLE, "Home button opens myTV", "The remote's Home button opens myTV instead of LG's home screen, from any app. Needs a rooted TV with the Homebrew Channel. LG Home stays one press away in Control Center." },
};

static int country_idx(void) {
    for (int i = 0; i < NCOUNTRIES; i++) if (!strcmp(COUNTRIES[i].code, prefs.country)) return i;
    return -1;
}

/* A screensaver option applies only to the screensaver that reads it. With the TV's own, the TV
 * keeps its timer too, so myTV's idle setting does nothing either. */
static bool opt_enabled(int o) {
    switch (o) {
        case O_IDLE: return prefs.ss_type != 0;
        case O_PH_BRIGHT: case O_PC_POS: case O_PC_SIZE: case O_PC_INFO: case O_PC_LOOK: case O_PH_SECS: case O_PH_PRINT:
            return prefs.ss_type == 1;
        case O_SB_FACE: case O_SB_COLOR: case O_SB_NIGHT: return prefs.ss_type == 2;
        default: return true;
    }
}

static bool opt_visible(int o) {
    if (o == O_WP_CHOOSE) return !prefs.wp_plain;
    if (o == O_DOCK_DIM) return prefs.dock_hide != 0;
    if (o == O_CLK_ORDER) return prefs.clock_stacked;
    return true;
}

static int opt_nchoices(int o) {
    switch (o) {
        case O_WP_TYPE: return 2;
        case O_WP_AUTO: case O_DOCK_HIDE: return 4;
        case O_WP_DIM: case O_APPS_ROW: case O_FOCUS: case O_PH_BRIGHT: case O_SS_TYPE: case O_SB_NIGHT: return 3;
        case O_PC_LOOK: case O_CLK_LOOK: case O_CLK_ORDER: case O_CLK_LAYOUT: return 2;
        case O_CLK_WEIGHT: case O_CLK_POS: return 3;
        case O_CLK_STYLE: case O_CLK_SIZE: return 5;
        case O_CLK_COLOR: return 7;
        case O_PC_SIZE: case O_IDLE: return 5;
        case O_PH_SECS: return 6;
        case O_PC_POS: case O_SB_COLOR: return 8;
        case O_SB_FACE: return 1 + SB_FACES;
        case O_COUNTRY: return 1 + NCOUNTRIES;
        case O_CITY: {
            int c = country_idx(), n = 1;
            if (c >= 0) return 1 + COUNTRIES[c].n;
            for (int i = 0; i < NCOUNTRIES; i++) n += COUNTRIES[i].n;
            return n;
        }
        default: return 0;
    }
}

/* City choices: the chosen country's, or every city when the country is Auto (lumen `Ao`). */
static const city_t *city_at(int i) {
    int c = country_idx();
    if (c >= 0) return &COUNTRIES[c].cities[i];
    for (int k = 0; k < NCOUNTRIES; k++) { if (i < COUNTRIES[k].n) return &COUNTRIES[k].cities[i]; i -= COUNTRIES[k].n; }
    return NULL;
}

static const char *opt_choice(int o, int i) {
    static const char *const WPT[] = { "Pictures", "Plain" }, *const AUTO[] = { "Off", "15 min", "30 min", "60 min" },
        *const DIM[] = { "Off", "At night", "Always" }, *const PER[] = { "4", "5", "6" },
        *const DOCK[] = { "Off", "Down arrow", "After idle", "Both" }, *const FOC[] = { "Subtle", "Normal", "Strong" };
    static const char *const BRI[] = { "As the TV is set", "Dim", "Very dim" },
        *const POS[] = { "Off", "Bottom left", "Bottom centre", "Bottom right", "Top left", "Top centre", "Top right", "Centre" },
        *const SIZE[] = { "Small", "Normal", "Large", "Extra large", "Huge" }, *const LOOK[] = { "Frosted", "Solid" },
        *const IDLE[] = { "Off", "5 min", "10 min", "20 min", "30 min" },
        *const TYPE[] = { "The TV's own", "Photos in myTV", "StandBy" },
        *const SECS[] = { "10 seconds", "20 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes" },
        *const FACE[] = { "Last used", "Digital", "Analog", "Clock and Calendar", "Solar", "Photos", "Weather", "Fade", "Flow" },
        *const COL[] = { "White", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Pink" },
        *const NIGHT[] = { "Auto", "On", "Off" };
    static const char *const CSTY[] = { "Digital", "Date and time", "With seconds", "Analog", "Analog and digital" },
        *const CCOL[] = { "Auto", "White", "Blue", "Green", "Orange", "Pink", "Yellow" },
        *const CLOOK[] = { "Solid", "Frosted" }, *const CORD[] = { "Time on top", "Time below" },
        *const CLAY[] = { "One row", "Stacked" }, *const CWT[] = { "Light", "Regular", "Bold" },
        *const CPOS[] = { "Left", "Centre", "Right" };
    switch (o) {
        case O_CLK_STYLE: return CSTY[i];
        case O_CLK_COLOR: return CCOL[i];
        case O_CLK_LOOK: return CLOOK[i];
        case O_CLK_ORDER: return CORD[i];
        case O_CLK_LAYOUT: return CLAY[i];
        case O_CLK_SIZE: return SIZE[i];
        case O_CLK_WEIGHT: return CWT[i];
        case O_CLK_POS: return CPOS[i];
        case O_PH_BRIGHT: return BRI[i];
        case O_PC_POS: return POS[i];
        case O_PC_SIZE: return SIZE[i];
        case O_PC_LOOK: return LOOK[i];
        case O_IDLE: return IDLE[i];
        case O_SS_TYPE: return TYPE[i];
        case O_PH_SECS: return SECS[i];
        case O_SB_FACE: return FACE[i];
        case O_SB_COLOR: return COL[i];
        case O_SB_NIGHT: return NIGHT[i];
        case O_WP_TYPE: return WPT[i];
        case O_WP_AUTO: return AUTO[i];
        case O_WP_DIM: return DIM[i];
        case O_APPS_ROW: return PER[i];
        case O_DOCK_HIDE: return DOCK[i];
        case O_FOCUS: return FOC[i];
        case O_COUNTRY: return i ? COUNTRIES[i - 1].name : "Auto";
        case O_CITY: return i ? city_at(i - 1)->name : "Auto";
        default: return "";
    }
}

static int opt_current(int o) {
    switch (o) {
        case O_WP_TYPE: return prefs.wp_plain;
        case O_WP_AUTO: return prefs.wp_auto_min == 15 ? 1 : prefs.wp_auto_min == 30 ? 2 : prefs.wp_auto_min == 60 ? 3 : 0;
        case O_WP_DIM: return prefs.wp_dim;
        case O_APPS_ROW: return prefs.apps_row - 4;
        case O_DOCK_HIDE: return prefs.dock_hide;
        case O_FOCUS: return prefs.focus;
        case O_CLK_STYLE: return prefs.clock_style;
        case O_CLK_COLOR: return prefs.clock_color;
        case O_CLK_LOOK: return prefs.clock_frosted;
        case O_CLK_ORDER: return prefs.clock_below;
        case O_CLK_LAYOUT: return prefs.clock_stacked;
        case O_CLK_SIZE: return prefs.clock_size;
        case O_CLK_WEIGHT: return prefs.clock_weight;
        case O_CLK_POS: return prefs.clock_pos;
        case O_PH_BRIGHT: return prefs.ph_bright;
        case O_PC_POS: return prefs.pc_pos;
        case O_PC_SIZE: return prefs.pc_size;
        case O_PC_LOOK: return prefs.pc_solid;
        case O_IDLE: { static const int M[] = { 0, 5, 10, 20, 30 }; for (int i = 0; i < 5; i++) if (M[i] == prefs.idle_min) return i; return 0; }
        case O_SS_TYPE: return prefs.ss_type;
        case O_PH_SECS: { static const int S[] = { 10, 20, 30, 60, 120, 300 }; for (int i = 0; i < 6; i++) if (S[i] == prefs.ph_secs) return i; return 2; }
        case O_SB_FACE: return prefs.sb_face;
        case O_SB_COLOR: return prefs.sb_color;
        case O_SB_NIGHT: return prefs.sb_night;
        case O_COUNTRY: return country_idx() + 1;
        case O_CITY:
            for (int i = 1; i < opt_nchoices(O_CITY); i++) if (!strcmp(city_at(i - 1)->name, prefs.city)) return i;
            return 0;
        default: return 0;
    }
}

static bool opt_on(int o) {
    switch (o) {
        case O_WP_NEW: return prefs.wp_new;
        case O_HIDE_NAMES: return prefs.hide_names;
        case O_DOCK_DIM: return prefs.dock_dim;
        case O_RECENTS: return prefs.show_recents;
        case O_WEATHER: return prefs.show_weather;
        case O_MOTION: return prefs.reduce_motion;
        case O_SHOW_DATE: return prefs.show_date;
        case O_AMPM: return prefs.show_ampm;
        case O_SYS_APPS: return prefs.sys_apps;
        case O_HOME_KEY: return homehook_on();
        case O_PC_INFO: return prefs.pc_info;
        case O_PH_PRINT: return prefs.ph_print;
        default: return false;
    }
}

static void motion_apply(void);
static void status_build(void);
static int status_dock_dy(void);

/* Apply a toggle (choice < 0) or a choice; every change takes effect at once. */
static void opt_set(int o, int choice) {
    switch (o) {
        case O_WP_TYPE:
            prefs.wp_plain = choice == 1;
            wp_switch(prefs.wp_plain ? WP_PLAIN : wp_random_pick());
            wp_timer_update();
            break;
        case O_WP_AUTO: { static const int M[] = { 0, 15, 30, 60 }; prefs.wp_auto_min = M[choice]; wp_timer_update(); break; }
        case O_WP_DIM: prefs.wp_dim = choice; break;
        case O_APPS_ROW: prefs.apps_row = 4 + choice; break;
        case O_DOCK_HIDE: prefs.dock_hide = choice; break;
        case O_FOCUS: prefs.focus = choice; styles_focus_apply(); break;
        case O_COUNTRY:
            if (!choice) prefs.country[0] = prefs.city[0] = 0;
            else {
                snprintf(prefs.country, sizeof(prefs.country), "%s", COUNTRIES[choice - 1].code);
                snprintf(prefs.city, sizeof(prefs.city), "%s", COUNTRIES[choice - 1].cities[0].name);
            }
            weather_apply_prefs();
            break;
        case O_CITY:
            if (!choice) { prefs.city[0] = 0; prefs.country[0] = 0; }
            else {
                const city_t *c = city_at(choice - 1);
                if (country_idx() < 0)          /* a city picked from the full list sets its country */
                    for (int k = 0; k < NCOUNTRIES; k++)
                        if (c >= COUNTRIES[k].cities && c < COUNTRIES[k].cities + COUNTRIES[k].n)
                            snprintf(prefs.country, sizeof(prefs.country), "%s", COUNTRIES[k].code);
                snprintf(prefs.city, sizeof(prefs.city), "%s", c->name);
            }
            weather_apply_prefs();
            break;
        case O_WP_NEW: prefs.wp_new = !prefs.wp_new; break;
        case O_HIDE_NAMES: prefs.hide_names = !prefs.hide_names; break;
        case O_DOCK_DIM: prefs.dock_dim = !prefs.dock_dim; break;
        case O_RECENTS: prefs.show_recents = !prefs.show_recents; break;
        case O_WEATHER: prefs.show_weather = !prefs.show_weather; break;
        case O_MOTION: prefs.reduce_motion = !prefs.reduce_motion; motion_apply(); break;
        case O_SHOW_DATE: prefs.show_date = !prefs.show_date; break;
        case O_AMPM: prefs.show_ampm = !prefs.show_ampm; break;
        case O_SYS_APPS: prefs.sys_apps = !prefs.sys_apps; break;
        case O_HOME_KEY:
            if (!homehook_state[0]) { show_toast("Needs a rooted TV with the Homebrew Channel", true); return; }
            if (!strcmp(homehook_state, "lginputhook")) { show_toast("LG Input Hook already handles the Home button", false); return; }
            /* the hook answers 1-2 s after it is injected: `status` in the same exec may still say none */
            rq_exec(RQ_HOMEHOOK, homehook_on() ? HOMEHOOK " uninstall; " HOMEHOOK " status" : HOMEHOOK " install; sleep 3; " HOMEHOOK " status");
            return;
        case O_CLK_STYLE: prefs.clock_style = choice; status_build(); break;
        case O_CLK_COLOR: prefs.clock_color = choice; status_build(); break;
        case O_CLK_LOOK: prefs.clock_frosted = choice; status_build(); break;
        case O_CLK_ORDER: prefs.clock_below = choice; status_build(); break;
        case O_CLK_LAYOUT: prefs.clock_stacked = choice; status_build(); break;
        case O_CLK_SIZE: prefs.clock_size = choice; status_build(); break;
        case O_CLK_WEIGHT: prefs.clock_weight = choice; status_build(); break;
        case O_CLK_POS: prefs.clock_pos = choice; status_build(); break;
        case O_PH_BRIGHT: prefs.ph_bright = choice; break;
        case O_PC_POS: prefs.pc_pos = choice; break;
        case O_PC_SIZE: prefs.pc_size = choice; break;
        case O_PC_LOOK: prefs.pc_solid = choice == 1; break;
        case O_IDLE: { static const int M[] = { 0, 5, 10, 20, 30 }; prefs.idle_min = M[choice]; break; }
        case O_SS_TYPE: prefs.ss_type = choice; saver_policy_apply(); break;
        case O_PH_SECS: { static const int S[] = { 10, 20, 30, 60, 120, 300 }; prefs.ph_secs = S[choice]; break; }
        case O_SB_FACE: prefs.sb_face = choice; break;
        case O_SB_COLOR: prefs.sb_color = choice; break;
        case O_SB_NIGHT: prefs.sb_night = choice; break;
        case O_PC_INFO: prefs.pc_info = !prefs.pc_info; break;
        case O_PH_PRINT: prefs.ph_print = !prefs.ph_print; break;
    }
    state_save();
    clock_tick(NULL);   /* date, weather, AM/PM and wallpaper dimming apply at once */
}

static const char *opt_value(int o) {
    if (o == O_HOME_KEY) {
        if (!homehook_state[0]) return "Needs root";
        if (!strcmp(homehook_state, "lginputhook")) return "LG Input Hook";
        return homehook_on() ? "On" : "Off";
    }
    if (OPTS[o].type == OT_TOGGLE) return opt_on(o) ? "On" : "Off";
    if (OPTS[o].type == OT_CHOICE) return opt_choice(o, opt_current(o));
    if (OPTS[o].type == OT_ACTION) return "Start";
    return "";
}

/* ---- page stack ---- */

static void grid_move(int *sel, int n, int cols, int k);

enum { P_ROOT, P_GROUP, P_CHOICE, P_APPS, P_SOURCES, P_WALLPAPER };
typedef struct { int kind, arg, focus; } page_t;
static page_t pages[6];
static int npages;
#define PG (&pages[npages - 1])

static lv_obj_t *set_page, *set_track, *set_label, *set_desc, *set_glyph, *set_chip;
static lv_obj_t *set_pill, *set_lens;    /* list pages: the focus highlight and its dark copy of the rows */
static int set_pill_y;                  /* the pill offset in the track: it follows the focus row */
static void pill_place(void);
static int list_top(int f);
#define SET_MAX 256      /* rows a settings page holds: all apps, or every city when the country is Auto */
static lv_obj_t *set_items[SET_MAX];
static int set_n, set_ids[SET_MAX];     /* list rows: what each row stands for */
static int set_level;                          /* grid/wallpaper pages: 0 head/chips, 1 content */

/* Root rows: Apps, Sources, then the groups (each has at least one visible option). */
static int root_rows(int *ids) {
    int n = 0;
    ids[n++] = -1; ids[n++] = -2;
    for (int g = 0; g < SG_N; g++) ids[n++] = g;
    return n;
}
static int group_rows(int g, int *ids) {
    int n = 0;
    for (int o = 0; o < O_N; o++) if (OPTS[o].group == g && opt_visible(o)) ids[n++] = o;
    return n;
}

static const char *row_label(int kind, int id) {
    if (kind == P_ROOT) return id == -1 ? "Apps" : id == -2 ? "Sources" : SGROUPS[id].label;
    if (kind == P_GROUP) return OPTS[id].label;
    return opt_choice(PG->arg, id);
}

static const char *row_desc(int kind, int id) {
    if (kind == P_ROOT) return "";
    int o = kind == P_GROUP ? id : PG->arg;
    if (o == O_WP_CHOOSE) {
        static const char *const T[WP_COUNT] = {
            "View of Lake Pepin, Minnesota - Robert S. Duncanson", "View near Newport - John Frederick Kensett",
            "Sunset on the River Oise - Charles-Francois Daubigny", "Landscape - Jasper F. Cropsey",
            "Corn Fields - Felix Vallotton" };
        return wp_cur < WP_COUNT ? T[wp_cur] : "";
    }
    return OPTS[o].desc;
}

static const lv_img_dsc_t *page_glyph(void) {
    if (PG->kind == P_ROOT) { int id = set_ids[PG->focus]; return id == -1 ? &ic_grid_192 : id == -2 ? &ic_inputs_192 : SGROUPS[id].glyph; }
    if (PG->kind == P_GROUP) return SGROUPS[PG->arg].glyph;
    return SGROUPS[OPTS[PG->arg].group].glyph;
}

/* .slist__row: 920x70 pill, 32px/500 label, value at .55 and a chevron on the right; focused:
 * white with #111 text, scale 1.03 (focus highlight 1.015 / 1.05) and 0 14px 28px #0000004d.
 * The focused look is not a row state: set_pill glides over the rows and shows `lit` copies of
 * them, so the highlight stays still while a held key scrolls the list (see settings_refresh). */
static lv_obj_t *list_row(lv_obj_t *track, int i, const char *text, const char *value, bool chevron, int tick, bool lit) {
    uint32_t ink = lit ? 0x111111 : 0xFFFFFF;
    lv_obj_t *r = group(track, 50, i * 84, 920, 70, 30);
    lv_obj_set_style_radius(r, 35, 0);
    lv_obj_set_style_bg_color(r, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(r, lit ? 0 : 0x1c, 0);
    if (!lit) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(r, TAG(Z_SETTINGS, 0, i));
    }
    int x = 34;
    if (tick >= 0) {                                   /* .slist__tick: 30px, before the label */
        lv_obj_t *t = image(r, &ic_tick_30);
        if (lit) {
            lv_obj_set_style_img_recolor(t, lv_color_hex(ink), 0);
            lv_obj_set_style_img_recolor_opa(t, LV_OPA_COVER, 0);
        }
        lv_obj_align(t, LV_ALIGN_LEFT_MID, x, 0);
        if (!tick) lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        x += 30 + 18;
    }
    /* .slist__label flex 1 with ellipsis: one line, everything the value and chevron leave */
    int vw = value[0] ? lv_txt_get_width(value, strlen(value), &geist_32m, 0, 0) : 0;
    if (vw > 380) vw = 380;
    lv_obj_t *l = label(r, &geist_32m, ink, text);
    lv_obj_set_style_text_opa(l, 0xf0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, 920 - x - 34 - (chevron ? 34 : 0) - (vw ? vw + 24 : 0), lv_font_get_line_height(&geist_32m));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
    int right = 34;
    if (chevron) {                                     /* .slist__chevron 14x24 at .5 */
        lv_obj_t *c = image(r, &ic_chevron_right);
        lv_obj_set_style_img_opa(c, lit ? 0x73 : 0x80, 0);
        if (lit) {
            lv_obj_set_style_img_recolor(c, lv_color_hex(ink), 0);
            lv_obj_set_style_img_recolor_opa(c, LV_OPA_COVER, 0);
        }
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -right, 0);
        right += 14 + 20;
    }
    if (value[0]) {
        lv_obj_t *v = label(r, &geist_32m, ink, value);
        lv_obj_set_style_text_opa(v, 0x8c, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_size(v, vw, lv_font_get_line_height(&geist_32m));
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -right, 0);
    }
    return r;
}

/* A moving object invalidates only its own area, not children hanging outside it: a track
 * shorter than its rows leaves the rows that scroll in from below unredrawn. */
static void track_fit(int view_h, int content_h) {
    lv_obj_set_height(set_track, content_h > view_h ? content_h : view_h);
}

static void fill_list(void) {
    static const char *const TITLE_ROOT = "Settings";
    const char *title = PG->kind == P_ROOT ? TITLE_ROOT : PG->kind == P_GROUP ? SGROUPS[PG->arg].label : OPTS[PG->arg].label;
    /* .slist__title: top 56, 52px/600 at .62, centred */
    lv_obj_t *t = label(set_page, &geist_52s, 0xFFFFFF, title);
    lv_obj_set_style_text_opa(t, 0x9e, 0);
    lv_obj_set_width(t, SCR_W);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, 0, 56 + (72 - lv_font_get_line_height(&geist_52s)) / 2);
    /* .slist__side: a 480px rounded glyph box and the description, centred in x 110-750 */
    int side_y = (SCR_H - (480 + 40 + 140)) / 2;
    lv_obj_t *ib = box(set_page, 110 + (640 - 480) / 2, side_y, 480, 480);
    lv_obj_set_style_radius(ib, 110, 0);
    lv_obj_set_style_bg_color(ib, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ib, 0x38, 0);
    set_glyph = image(ib, &ic_grid_192);
    lv_obj_set_style_img_recolor(set_glyph, lv_color_hex(0x0f121c), 0);
    lv_obj_set_style_img_recolor_opa(set_glyph, LV_OPA_COVER, 0);
    lv_obj_set_style_img_opa(set_glyph, 0xbf, 0);
    lv_obj_center(set_glyph);
    set_desc = label(set_page, &geist_26m, 0xFFFFFF, "");
    lv_obj_set_style_text_opa(set_desc, 0x9e, 0);
    lv_obj_set_style_text_line_space(set_desc, 11, 0);           /* line-height 1.45 */
    lv_obj_set_style_text_align(set_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(set_desc, 540);
    lv_obj_set_pos(set_desc, 110 + 50, side_y + 480 + 40);
    /* .slist__view: x 810, 840 tall from y 170, rows from y 182 */
    lv_obj_t *view = box(set_page, 810, 170, 1020, 840);
    lv_obj_clear_flag(view, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_all(view, 0, 0);
    set_track = group(view, 0, 12, 1020, 840, 30);

    int ids[SET_MAX];
    if (PG->kind == P_ROOT) set_n = root_rows(ids);
    else if (PG->kind == P_GROUP) set_n = group_rows(PG->arg, ids);
    else {
        set_n = opt_nchoices(PG->arg);
        if (set_n > SET_MAX) { LOG("option %d: %d choices, showing %d\n", PG->arg, set_n, SET_MAX); set_n = SET_MAX; }
        for (int i = 0; i < set_n; i++) ids[i] = i;
    }
    /* the pill comes after the track in `view`, so it draws above the rows it glides over */
    static const int ROW_ZOOM[3] = { 260, 264, 269 };
    set_pill = box(view, 50, 0, 920, 70);
    lv_obj_clear_flag(set_pill, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_radius(set_pill, 35, 0);
    lv_obj_set_style_bg_color(set_pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(set_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(set_pill, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(set_pill, 28, 0);
    lv_obj_set_style_shadow_ofs_y(set_pill, 14, 0);
    lv_obj_set_style_shadow_opa(set_pill, 0x4d, 0);
    lv_obj_set_style_transform_pivot_x(set_pill, 460, 0);
    lv_obj_set_style_transform_pivot_y(set_pill, 35, 0);
    lv_obj_set_style_transform_zoom(set_pill, ROW_ZOOM[prefs.focus], 0);
    set_lens = box(set_pill, -50, 0, 1020, set_n * 84);

    int cur = PG->kind == P_CHOICE ? opt_current(PG->arg) : -1;
    for (int i = 0; i < set_n; i++) {
        set_ids[i] = ids[i];
        const char *text = row_label(PG->kind, ids[i]), *value = PG->kind == P_GROUP ? opt_value(ids[i]) : "";
        bool chev = PG->kind == P_ROOT || (PG->kind == P_GROUP && OPTS[ids[i]].type != OT_TOGGLE);
        int tick = PG->kind == P_CHOICE ? i == cur : -1;
        set_items[i] = list_row(set_track, i, text, value, chev, tick, false);
        lv_obj_t *lit = list_row(set_lens, i, text, value, chev, tick, true);
        if (PG->kind == P_GROUP && !opt_enabled(ids[i])) {
            lv_obj_set_style_opa_layered(set_items[i], 0x59, 0);
            lv_obj_set_style_opa_layered(lit, 0x59, 0);
        }
    }
    track_fit(840, set_n * 84);
    /* in place before the first refresh: a page that code opens, not a key, would show the pill at 0 */
    int f = clampi(PG->focus, 0, set_n - 1);
    lv_obj_set_y(set_track, 12 - list_top(f) * 84);
    set_pill_y = f * 84;
    pill_place();
    if (!set_n) lv_obj_add_flag(set_pill, LV_OBJ_FLAG_HIDDEN);
}

/* ---- Apps / Sources grid (carried over from 1.0.9, 1.1.4 metrics) ---- */

static bool sys_app(const char *id) {
    static const char *const SYS[] = { "com.webos.app.livetv", "com.webos.app.lgchannels", "airplay",
                                       "com.webos.app.miracast", "com.webos.app.gameoptimizer", "com.palm.app.settings" };
    for (int i = 0; i < COUNT(SYS); i++) if (!strcmp(id, SYS[i])) return true;
    return false;
}

static int grid_item(int i) { return set_ids[i]; }   /* app index (Apps) or input index (Sources) */

static bool set_item_on(int i) {
    return PG->kind == P_APPS ? !hidden[grid_item(i)] : !src_is_hidden(inputs[grid_item(i)].app_id);
}

static void set_tile_state(int i) {
    lv_obj_t *t = set_items[i];
    bool on = set_item_on(i);
    /* .tile--dimmed .75, layered as CSS opacity: plain `opa` fades the icon over its already faded
     * tile, and the icon's square shows in another shade (the same fix as the home rows) */
    lv_obj_set_style_opa_layered(t, on ? LV_OPA_COVER : 0xbf, 0);
    lv_obj_t *badge = lv_obj_get_child(t, -1);
    if (on) lv_obj_clear_flag(badge, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
}

/* .tile__badge / .wp__tick: a white circle with a dark check mark, top-right */
static lv_obj_t *check_badge(lv_obj_t *parent, int x, int size) {
    lv_obj_t *b = box(parent, x, 12, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *ck = image(b, &ic_check_20);
    lv_obj_set_style_img_recolor(ck, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(ck, LV_OPA_COVER, 0);
    lv_obj_center(ck);
    return b;
}

/* .settings__hint: centred, 22px at .55, in the 72px head */
static void page_hint(const char *text) {
    lv_obj_t *h = label(set_page, &geist_22m, 0xFFFFFF, text);
    lv_obj_set_style_text_opa(h, 0x8c, 0);
    lv_obj_set_width(h, SCR_W);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h, 0, 84 + (72 - lv_font_get_line_height(&geist_22m)) / 2);
}

static void fill_grid(void) {
    page_hint("OK adds or removes an item \xC2\xB7 Back returns");
    lv_obj_t *view = box(set_page, 0, 84 + 72 + 18, SCR_W, 720);
    lv_obj_clear_flag(view, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    set_track = box(view, 0, 0, SCR_W, 720);
    set_n = 0;
    if (PG->kind == P_APPS) { for (int i = 0; i < norder; i++) if (prefs.sys_apps || !sys_app(apps[order[i]].id)) set_ids[set_n++] = order[i]; }
    else for (int i = 0; i < ninputs; i++) set_ids[set_n++] = i;
    const int x0 = (SCR_W - (6 * TILE_STEP - TILE_GAP)) / 2;
    for (int i = 0; i < set_n; i++) {
        ref_t r = PG->kind == P_APPS ? (ref_t) { IT_APP, set_ids[i] } : (ref_t) { IT_INPUT, set_ids[i] };
        set_items[i] = make_tile(set_track, r, x0 + (i % 6) * TILE_STEP, 16 + (i / 6) * 242, false, TAG(Z_SETTINGS, 1, i));
        check_badge(set_items[i], TILE_W - 12 - 34, 34);
        set_tile_state(i);
    }
    track_fit(720, 16 + (set_n + 5) / 6 * 242);
    set_label = label(set_track, &geist_19s, C_TEXT, "");
    lv_obj_set_width(set_label, TILE_W + TILE_GAP - 8);
    lv_obj_set_style_text_align(set_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_opa(set_label, 0, 0);
}

static void grid_toggle(int i) {
    if (PG->kind == P_APPS) hidden[grid_item(i)] = !hidden[grid_item(i)];
    else {
        const char *id = inputs[grid_item(i)].app_id;
        int k;
        for (k = 0; k < nsrc_hidden && strcmp(src_hidden[k], id); k++) {}
        if (k < nsrc_hidden) { memmove(src_hidden[k], src_hidden[k + 1], (size_t) (nsrc_hidden - k - 1) * 48); nsrc_hidden--; }
        else if (nsrc_hidden < 16) snprintf(src_hidden[nsrc_hidden++], 48, "%s", id);
    }
    state_save();
    set_tile_state(i);
}

/* ---- Choose wallpaper: the Defaults chip, Select all and the five bundled paintings ---- */

#define WP_ITEMS (1 + WP_COUNT)
#define WP_ITEM_DX 427                     /* 360 + 66.67 */
#define WP_ITEM_DY 273
static lv_obj_t *wp_ticks[WP_ITEMS], *wp_titles[WP_ITEMS];

static void wp_ticks_update(void) {
    for (int i = 0; i < WP_ITEMS; i++) {
        bool on = i ? prefs.picks >> (i - 1) & 1 : prefs.picks == (1 << WP_COUNT) - 1;
        if (on) lv_obj_clear_flag(wp_ticks[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(wp_ticks[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void fill_wallpaper(void) {
    page_hint("Left and right change category \xC2\xB7 OK sets the wallpaper \xC2\xB7 Back returns");
    int top = 84 + 72 + 18;
    /* .wp__chip--on: the only category natively (the cloud ones need https) */
    set_chip = box(set_page, EDGE, top, lv_txt_get_width("Defaults", 8, &geist_22m, 0, 0) + 52, lv_font_get_line_height(&geist_22m) + 20);
    lv_obj_t *cl = label(set_chip, &geist_22m, C_TEXT, "Defaults");
    lv_obj_center(cl);
    lv_obj_set_style_radius(set_chip, 999, 0);
    lv_obj_set_style_bg_color(set_chip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(set_chip, 0x38, 0);
    lv_obj_set_style_bg_opa(set_chip, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(cl, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_add_flag(set_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(set_chip, TAG(Z_SETTINGS, 2, 0));

    lv_obj_t *view = box(set_page, 0, top + lv_obj_get_height(set_chip) + 26, SCR_W, 700);
    lv_obj_clear_flag(view, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    set_track = box(view, 0, 0, SCR_W, 700);
    static const char *const T[WP_COUNT] = {
        "View of Lake Pepin, Minnesota - Robert S. Duncanson", "View near Newport - John Frederick Kensett",
        "Sunset on the River Oise - Charles-Francois Daubigny", "Landscape - Jasper F. Cropsey",
        "Corn Fields - Felix Vallotton" };
    set_n = WP_ITEMS;
    for (int i = 0; i < WP_ITEMS; i++) {
        int x = EDGE + 16 + (i % 4) * WP_ITEM_DX, y = 20 + (i / 4) * WP_ITEM_DY, c = i - 1;
        /* .wp__thumb: 360x203 r22; focus = scale(1.06) + 0 0 0 4px #fff */
        lv_obj_t *t = group(set_track, x, y, 360, 203, 24);
        lv_obj_set_style_radius(t, 22, 0);
        lv_obj_set_style_outline_color(t, lv_color_white(), 0);
        lv_obj_set_style_outline_width(t, 4, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(t, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_transform_pivot_x(t, 180, 0);
        lv_obj_set_style_transform_pivot_y(t, 101, 0);
        lv_obj_set_style_transform_zoom(t, 271, LV_STATE_FOCUSED);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(t, TAG(Z_SETTINGS, 1, i));
        if (c >= 0 && wp_ready(c)) image(t, &wps[c].thumb);
        else {
            lv_obj_set_style_bg_color(t, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(t, 0x1a, 0);
            lv_obj_center(label(t, &geist_28m, 0xAAAAAA, c < 0 ? "Select all" : "..."));
        }
        wp_ticks[i] = check_badge(t, 360 - 12 - 38, 38);
        lv_obj_t *l = label(set_track, &geist_20m, 0xAAAAAA, c < 0 ? "Select all" : T[c]);
        lv_obj_set_width(l, 360);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(l, x, y + 203 + 14);
        wp_titles[i] = l;
        set_items[i] = t;
    }
    wp_ticks_update();
}

/* OK on a picture: it alone (lumen "picks"); on Select all: all of them, or back to the one on
 * screen when they are all ticked already. */
static void wp_pick(int i) {
    if (i == 0) prefs.picks = prefs.picks == (1 << WP_COUNT) - 1 ? 1 << (wp_cur < WP_COUNT ? wp_cur : 0) : (1 << WP_COUNT) - 1;
    else { prefs.picks = 1 << (i - 1); wp_switch(i - 1); }
    wp_timer_update();
    state_save();
    wp_ticks_update();
}

/* The painting under the focus in Choose wallpaper, -1 when none: OK on it would switch to it. */
static int wp_focused(void) {
    if (!settings_layer || !npages || PG->kind != P_WALLPAPER || !set_level) return -1;
    int c = PG->focus - 1;
    return c >= 0 && c != wp_cur ? c : -1;
}

/* ---- shell ---- */

/* A page opens already scrolled to its focus: chasing there from the top flew past a long list
 * (the country list at 466 px/frame). Moves inside the page still glide. */
static bool set_page_new;
/* The pointer's row is already on screen: scrolling it to the focus slot would slide another row
 * under the pointer, which would take the focus in turn. */
static bool set_by_pointer;

static void settings_render(void) {
    if (set_page) { focus_forget(set_page); lv_obj_del(set_page); }
    set_pill = set_lens = NULL;
    set_page = group(settings_layer, 0, 0, SCR_W, SCR_H, 40);
    set_page_new = true;
    set_label = set_chip = set_glyph = set_desc = NULL;
    set_n = 0;
    switch (PG->kind) {
        case P_APPS: case P_SOURCES: fill_grid(); break;
        case P_WALLPAPER: fill_wallpaper(); break;
        default: fill_list(); break;
    }
    if (PG->focus >= set_n) PG->focus = set_n ? set_n - 1 : 0;
    /* settings-in: opacity 0 -> 1 and translateX(36px) -> none, .22s ease-out */
    lv_obj_set_style_opa(set_page, 0, 0);
    anim(set_page, ax_opa, 0, LV_OPA_COVER, 220, lv_anim_path_ease_out);
    anim(set_page, ax_x, 36, 0, 220, lv_anim_path_ease_out);
}

static void settings_push(int kind, int arg) {
    if (npages >= 6) return;
    pages[npages++] = (page_t) { kind, arg, kind == P_CHOICE ? opt_current(arg) : 0 };
    set_level = kind == P_WALLPAPER ? 0 : 1;
    settings_render();
}

static void open_settings(void) {
    overlay_close();
    settings_layer = box(lv_layer_top(), 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(settings_layer, lv_color_hex(0x181c28), 0);
    lv_obj_set_style_bg_opa(settings_layer, LV_OPA_COVER, 0);
    zoom_full(image(settings_layer, &wps[wp_cur].settings_bg));
    lv_obj_add_flag(rows_cont, LV_OBJ_FLAG_HIDDEN);   /* html.in-settings: home is not drawn */
    npages = 0;
    set_page = set_pill = set_lens = NULL;
    ui.zone = Z_SETTINGS;
    settings_push(P_ROOT, 0);
    homehook_query();
}

static void settings_close(void) {
    if (!settings_layer) return;
    focus_forget(settings_layer);
    lv_obj_del(settings_layer);
    settings_layer = set_page = set_pill = set_lens = NULL;
    npages = 0;
    lv_obj_clear_flag(rows_cont, LV_OBJ_FLAG_HIDDEN);
    rows_build();
    panels_prefetch();
}

static void settings_pop(void) {
    if (npages <= 1) { settings_close(); ui.zone = Z_ROWS; return; }
    npages--;
    set_level = 1;
    settings_render();
}

static lv_obj_t *settings_focus_obj(void) {
    if ((PG->kind == P_WALLPAPER) && set_level == 0) return set_chip;
    return set_n ? set_items[PG->focus] : NULL;
}

/* The pill sits at track y + set_pill_y. Its spring and the track's start on the same key with
 * equal and opposite steps, so mid-list their lags cancel and the pill holds still on screen. */
static void pill_place(void) {
    if (!set_pill) return;
    lv_obj_set_y(set_pill, lv_obj_get_style_y(set_track, LV_PART_MAIN) + set_pill_y);
    lv_obj_set_y(set_lens, -set_pill_y);
}
static void ax_list_y(void *o, int32_t v) { lv_obj_set_y(o, v); pill_place(); }
static int list_top(int f) { return clampi(f - 4, 0, set_n - 10); }   /* the focus sits in the 5th slot */
static void ax_pill(void *o, int32_t v) { LV_UNUSED(o); set_pill_y = v; pill_place(); }

/* Scroll to the focused row and refresh the side panel (or the name under a grid tile). */
static void settings_refresh(void) {
    int f = PG->focus, y = 0;
    bool scroll = set_page_new || !set_by_pointer;
    switch (PG->kind) {
        case P_ROOT: case P_GROUP: case P_CHOICE: {
            if (scroll) anim_scroll(set_track, ax_list_y, lv_obj_get_y(set_track), 12 - list_top(f) * 84, !set_page_new);
            anim_scroll(set_pill, ax_pill, set_pill_y, f * 84, !set_page_new);
            set_page_new = false;
            const lv_img_dsc_t *g = page_glyph();
            if (lv_img_get_src(set_glyph) != g) lv_img_set_src(set_glyph, g);
            /* on a choice page every row has the same text: a held key would re-lay it each step */
            const char *d = set_n ? row_desc(PG->kind, set_ids[f]) : "";
            if (set_n && strcmp(lv_label_get_text(set_desc), d)) lv_label_set_text(set_desc, d);
            return;
        }
        case P_WALLPAPER: {
            int r = f / 4;
            if (set_level) y = -(r > 1 ? (r - 1) * WP_ITEM_DY : 0);
            for (int i = 0; i < WP_ITEMS; i++)
                lv_obj_set_style_text_color(wp_titles[i], lv_color_hex(set_level && i == f ? C_TEXT : 0xAAAAAA), 0);
            break;
        }
        default: {
            int r = f / 6;
            y = -(r > 0 ? r * 242 - (720 - 242) / 2 : 0);
            if (y > 0) y = 0;
            if (set_label && set_n) {
                /* from the index: a tile made this frame has no laid-out position yet */
                int tx = (SCR_W - (6 * TILE_STEP - TILE_GAP)) / 2 + (f % 6) * TILE_STEP, ty = 16 + (f / 6) * 242;
                ref_t r0 = PG->kind == P_APPS ? (ref_t) { IT_APP, set_ids[f] } : (ref_t) { IT_INPUT, set_ids[f] };
                lv_label_set_text(set_label, ref_title(r0));
                lv_obj_set_pos(set_label, tx - (TILE_GAP - 8) / 2, ty + TILE_H + 26);
                lv_obj_move_foreground(set_label);
                lv_obj_set_style_opa(set_label, 0, 0);
                anim_opa(set_label, LV_OPA_COVER, FOCUS_MS);
            }
        }
    }
    if (scroll) anim_scroll(set_track, ax_y, lv_obj_get_y(set_track), y, !set_page_new);
    set_page_new = false;
}

static void settings_activate(void) {
    int f = PG->focus, id = set_ids[f];
    switch (PG->kind) {
        case P_ROOT:
            if (id == -1) settings_push(P_APPS, 0);
            else if (id == -2) settings_push(P_SOURCES, 0);
            else settings_push(P_GROUP, id);
            break;
        case P_GROUP:
            if (!opt_enabled(id)) break;
            if (OPTS[id].type == OT_PAGE) settings_push(P_WALLPAPER, 0);
            else if (OPTS[id].type == OT_CHOICE) settings_push(P_CHOICE, id);
            else if (OPTS[id].type == OT_ACTION) saver_start();
            else { opt_set(id, -1); settings_render(); }               /* the value text changes */
            break;
        case P_CHOICE: {                                               /* choosing pops at once */
            int o = PG->arg;
            opt_set(o, f);
            npages--;
            int ids[O_N], n = group_rows(PG->arg, ids);     /* a choice can show or hide rows above it */
            for (int i = 0; i < n; i++) if (ids[i] == o) PG->focus = i;
            settings_render();
            break;
        }
        case P_WALLPAPER: wp_pick(f); break;
        default: grid_toggle(f); break;
    }
}

static void settings_key(int k) {
    set_by_pointer = false;
    if (k == K_BACK) { settings_pop(); return; }
    int *f = &PG->focus;
    switch (PG->kind) {
        case P_ROOT: case P_GROUP: case P_CHOICE:
            if (k == K_UP || k == K_DOWN) *f = clampi(*f + (k == K_UP ? -1 : 1), 0, set_n - 1);
            else if (k == K_OK) settings_activate();
            break;
        case P_WALLPAPER:
            if (!set_level) { if (k == K_DOWN || k == K_OK) set_level = 1; }
            else if (k == K_UP && *f < 4) set_level = 0;
            else if (k == K_OK) settings_activate();
            else grid_move(f, set_n, 4, k);
            break;
        default:
            if (k == K_OK) settings_activate();
            else grid_move(f, set_n, 6, k);
            break;
    }
}

/* pointer: 0 list row, 1 grid / wallpaper item, 2 the wallpaper chip */
static void settings_pointer(int level, int i) {
    set_by_pointer = true;
    if (level == 2) { set_level = 0; return; }
    set_level = 1;
    PG->focus = i;
}

/* Reduce motion: focus, rows and panels change at once (html.reduce-motion: no transitions). */
static void motion_apply(void) {
    motion_off = prefs.reduce_motion;
    st_tr.time = motion_off ? 0 : FOCUS_MS;
    lv_obj_report_style_change(&st_tile);
    lv_obj_report_style_change(&st_ptile);
}

/* ================================================================ model edits */

static void recent_push(int a) {
    int n = 0, out[RECENT_MAX];
    out[n++] = a;
    for (int i = 0; i < nrecent && n < RECENT_MAX; i++) if (recent[i] != a) out[n++] = recent[i];
    memcpy(recent, out, sizeof(int) * n);
    nrecent = n;
    state_save();
}

static void launch_ref(ref_t r) {
    if (r.kind == IT_APP) {
        launch_app_id(apps[r.ref].id);
        recent_push(r.ref);
    } else if (r.kind == IT_INPUT) {
        launch_app_id(inputs[r.ref].app_id);
    }
}

static int order_pos(int a) {
    for (int i = 0; i < norder; i++) if (order[i] == a) return i;
    return -1;
}

/* Move a Home app one visible slot left/right (lumen onEdit "move"): swap with the nearest
 * visible neighbour in `order`, skipping hidden and foldered apps. */
static bool move_app(int a, int dir) {
    int p = order_pos(a);
    for (int q = p + dir; q >= 0 && q < norder; q += dir) {
        int b = order[q];
        if (hidden[b] || folder_of[b] >= 0) continue;
        order[q] = a; order[p] = b;
        return true;
    }
    return false;
}

static void folder_add(int a, int k) {
    if (folder_of[a] >= 0) return;
    folder_of[a] = k;
    folder_items[k][folder_n[k]++] = a;
}
static void folder_take_out(int a) {
    int k = folder_of[a];
    if (k < 0) return;
    int j = 0;
    for (int i = 0; i < folder_n[k]; i++) if (folder_items[k][i] != a) folder_items[k][j++] = folder_items[k][i];
    folder_n[k] = j;
    folder_of[a] = -1;
}

/* ================================================================ zone transitions */

static bool ui_busy(void) { return overlay || settings_layer || switcher_layer || ui.moving; }

static void ui_close_all(void) {
    overlay_close();
    switcher_close();
    settings_close();
    ui.zone = Z_ROWS;
    ui.moving = false;
    lv_obj_add_flag(movehint, LV_OBJ_FLAG_HIDDEN);
}

/* Apply the focus model to the screen: which object wears FOCUSED, labels, pills, rows. */
static void ui_refresh(bool animate) {
    lv_obj_t *f = NULL;
    switch (ui.zone) {
        case Z_ROWS: if (ui.engaged) f = cur_tile(); break;
        case Z_TOP: break;
        case Z_PANEL: if (panel_n) f = panel_obj[ui.panel]; break;
        case Z_FOLDER: if (panel_n) f = panel_obj[ui.folder_sel]; break;
        case Z_CC: f = cc_focus_target(); break;
        case Z_CTX: f = ctx_rows[ui.ctx]; break;
        case Z_SWITCHER: f = !sw_n ? NULL : sw_on_all ? sw_all : sw_cards[sw_sel]; break;
        case Z_SETTINGS: f = settings_focus_obj(); break;
    }
    if (ui.zone == Z_ROWS) focus_rows(); else focus_obj(f);
    labels_update();
    dots_update();
    topbar_update();
    rows_layout(animate);
    /* status slides off with the dock (.screen--dockhidden .status translateY(830px)) */
    anim(status_cont, ax_ty, lv_obj_get_style_translate_y(status_cont, 0), ui.dock_hidden ? status_dock_dy() : 0, animate ? 320 : 0, path_lumen);
    for (int p = 0; p < 2; p++) {
        anim_opa(layers[L_PILL0 + p].img[layers[L_PILL0 + p].front], ui.dock_hidden ? 0 : LV_OPA_COVER, animate ? 220 : 0);
        anim_opa(pill_icon[p], ui.dock_hidden ? 0 : LV_OPA_COVER, animate ? 220 : 0);
    }
    anim_opa(dockhint, ui.dock_hidden ? 0x9e : 0, animate ? 260 : 0);
    if (ui.zone == Z_CC) cc_refresh();
    if (ui.zone == Z_SETTINGS) settings_refresh();
}

static void ui_rest(void) {
    ui_close_all();
    ui.engaged = false;
    ui.row = 0;
    ui.dock_hidden = false;
    ui_refresh(false);
}

/* ================================================================ input */


static void grid_move(int *sel, int n, int cols, int k) {
    if (k == K_LEFT) *sel = clampi(*sel - 1, 0, n - 1);
    else if (k == K_RIGHT) *sel = clampi(*sel + 1, 0, n - 1);
    else if (k == K_UP && *sel - cols >= 0) *sel -= cols;
    else if (k == K_DOWN && *sel + cols <= n - 1) *sel += cols;
}

static char uninstall_title[64];

/* lumen Vr: remove through appInstallService (dev/remove for developer installs, then remove),
 * then report whether getAppInfo still knows the app. */
static void uninstall_app(int a) {
    const char *id = apps[a].id;
    char probe[200], cmd[1000];
    snprintf(probe, sizeof(probe), "luna-send -n 1 luna://com.webos.applicationManager/getAppInfo '{\"id\":\"%s\"}' </dev/null 2>/dev/null | grep -q '\"returnValue\":true'", id);
    snprintf(cmd, sizeof(cmd),
             "if %s; then luna-send -n 1 luna://com.webos.appInstallService/dev/remove '{\"id\":\"%s\",\"subscribe\":false}' </dev/null >/dev/null 2>&1; sleep 2; fi; "
             "if %s; then luna-send -n 1 luna://com.webos.appInstallService/remove '{\"id\":\"%s\",\"subscribe\":false}' </dev/null >/dev/null 2>&1; sleep 2; fi; "
             "if %s; then echo kept; else echo gone; fi", probe, id, probe, id, probe);
    snprintf(uninstall_title, sizeof(uninstall_title), "%s", apps[a].title);
    char t[96];
    snprintf(t, sizeof(t), "Uninstalling %s...", apps[a].title);
    show_toast(t, false);
    rq_exec(RQ_UNINSTALL, cmd);
}

static void open_switcher(void);

static void ctx_activate(int i) {
    row_t *R = &rows[ui.row];
    int id = ctx_ids[i];
    if (id == CI_UNINSTALL && !ctx_confirm) {          /* the first OK only asks */
        ctx_confirm = true;
        lv_label_set_text(ctx_labels[i], "Press OK again to uninstall");
        return;
    }
    overlay_close();
    if (id == CI_TAKEOUT) {
        folder_take_out(folder_items[ui.folder][ui.folder_sel]);
        state_save();
        rows_build();
        if (folder_n[ui.folder]) { open_folder(ui.folder); return; }
        ui.zone = Z_ROWS;
        return;
    }
    ui.zone = Z_ROWS;
    ref_t r = R->it[R->col];
    switch (id) {
        case CI_RECENT: open_switcher(); return;
        case CI_MOVE: ui.moving = true; lv_obj_clear_flag(movehint, LV_OBJ_FLAG_HIDDEN); return;
        case CI_UNINSTALL: uninstall_app(r.ref); return;
        case CI_REMOVE: hidden[r.ref] = true; break;
        default: folder_add(r.ref, id - CI_ADD0); break;
    }
    state_save();
    rows_build();
    panels_prefetch();
}

static void move_end(void) {
    ui.moving = false;
    lv_obj_add_flag(movehint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *t = cur_tile();
    if (t) lv_obj_remove_local_style_prop(t, LV_STYLE_TRANSFORM_ZOOM, LV_STATE_FOCUSED);
    state_save();
}

/* Moving re-orders the model and rebuilds, then re-finds the carried app (it may cross from
 * the tray row into More apps, as in lumen). */
/* `dir` is a number of visible slots: +-1 along the row, +-appsPerRow to the row above/below. */
static void move_step(int dir) {
    row_t *R = &rows[ui.row];
    ref_t r = R->it[R->col];
    if (r.kind != IT_APP) return;
    bool moved = false;
    for (int i = 0; i < abs(dir); i++) moved |= move_app(r.ref, dir < 0 ? -1 : 1);
    if (!moved) return;
    rows_build();
    for (int ri = 0; ri < nrows; ri++)
        for (int c = 0; c < rows[ri].n; c++)
            if (rows[ri].kind != 2 && rows[ri].it[c].kind == IT_APP && rows[ri].it[c].ref == r.ref) { ui.row = ri; rows[ri].col = c; }
}

static void rows_activate(void) {
    row_t *R = &rows[ui.row];
    if (!R->n) return;
    ref_t r = R->it[R->col];
    if (r.kind == IT_FOLDER) { open_folder(r.ref); return; }
    launch_ref(r);
}

static uint32_t last_input;     /* idle timers: the dock's and the screensaver's */

static void handle(int k) {
    last_input = SDL_GetTicks();
    if (saver_active()) { saver_handle(k); return; }
    if (ui.dock_hidden) { ui.dock_hidden = false; ui_refresh(true); return; }
    /* number keys 1-6 launch the tray's tiles from rows or the top bar (lumen 1.1.4) */
    if (k >= K_NUM1) {
        int c = k - K_NUM1;
        if ((ui.zone == Z_ROWS || ui.zone == Z_TOP) && !ui.moving && c < rows[0].n) {
            ui.engaged = true; ui.zone = Z_ROWS; ui.row = 0; rows[0].col = c;
            ui_refresh(true);
            rows_activate();
        }
        return;
    }
    if (!ui.engaged) { ui.engaged = true; ui_refresh(true); return; }   /* first key only wakes the UI */

    switch (ui.zone) {
        case Z_ROWS: {
            row_t *R = &rows[ui.row];
            if (ui.moving) {
                if (k == K_LEFT || k == K_RIGHT) move_step(k == K_LEFT ? -1 : 1);
                else if (k == K_UP || k == K_DOWN) move_step(k == K_UP ? -prefs.apps_row : prefs.apps_row);
                else if (k == K_OK || k == K_BACK) move_end();
                break;
            }
            if (k == K_BACK && ui.row >= 1) { rows[0].col = clampi(R->col, 0, rows[0].n - 1); ui.row = 0; break; }
            if (k == K_LEFT) R->col = clampi(R->col - 1, 0, R->n - 1);
            else if (k == K_RIGHT) R->col = clampi(R->col + 1, 0, R->n - 1);
            else if (k == K_UP) { if (ui.row == 0) ui.zone = Z_TOP; else { rows[ui.row - 1].col = clampi(R->col, 0, rows[ui.row - 1].n - 1); ui.row--; } }
            else if (k == K_DOWN) {
                if (ui.row >= nrows - 1) { if (prefs.dock_hide & 1) ui.dock_hidden = true; }   /* "Down arrow" / "Both" */
                else { rows[ui.row + 1].col = clampi(R->col, 0, rows[ui.row + 1].n - 1); ui.row++; }
            } else if (k == K_OK) rows_activate();
            else if (k == K_HOLD && R->kind != 2 && R->n && R->it[R->col].kind == IT_APP) open_ctx(false);
            break;
        }
        case Z_TOP:
            if (k == K_LEFT) ui.top = 0;
            else if (k == K_RIGHT) ui.top = 1;
            else if (k == K_DOWN || k == K_BACK) ui.zone = Z_ROWS;
            else if (k == K_OK) { if (ui.top == 0) open_inputs(); else open_cc(); }
            break;
        case Z_PANEL:
            if (k == K_BACK) { overlay_close(); ui.zone = Z_TOP; }
            else if (k == K_OK) { if (panel_n) { ref_t r = panel_ref[ui.panel]; overlay_close(); ui.zone = Z_TOP; launch_ref(r); } }
            else if (panel_n) grid_move(&ui.panel, panel_n, 3, k);
            break;
        case Z_FOLDER:
            if (k == K_BACK) { overlay_close(); ui.zone = Z_ROWS; }
            else if (k == K_OK) { if (panel_n) { ref_t r = panel_ref[ui.folder_sel]; overlay_close(); ui.zone = Z_ROWS; launch_ref(r); } }
            else if (k == K_HOLD && panel_n) open_ctx(true);
            else if (panel_n) grid_move(&ui.folder_sel, panel_n, 3, k);
            break;
        case Z_CTX:
            if (k == K_BACK) {
                overlay_close();
                if (ui.ctx_from_folder) open_folder(ui.folder); else ui.zone = Z_ROWS;
            } else if (k == K_UP) { ctx_confirm_reset(); ui.ctx = clampi(ui.ctx - 1, 0, ctx_n - 1); }
            else if (k == K_DOWN) { ctx_confirm_reset(); ui.ctx = clampi(ui.ctx + 1, 0, ctx_n - 1); }
            else if (k == K_OK) ctx_activate(ui.ctx);
            break;
        case Z_CC: cc_key(k); break;
        case Z_SETTINGS: settings_key(k); break;
        case Z_SWITCHER: switcher_key(k); break;
    }
    ui_refresh(true);
}

/* Pointer: hovering focuses (lumen mode "pointer"), clicking activates what is hovered. */
static void pointer_focus(int x, int y, bool click) {
    __typeof__(ui) before = ui;
    lv_point_t pt = { x, y };
    lv_obj_t *root = overlay ? overlay : settings_layer ? settings_layer : scr;
    lv_obj_t *o = lv_indev_search_obj(root, &pt);
    while (o && !lv_obj_get_user_data(o)) o = lv_obj_get_parent(o);
    intptr_t tag = o ? (intptr_t) lv_obj_get_user_data(o) : 0;
    static intptr_t last_tag = -1;
    if (tag != last_tag) { LOG("pointer %d,%d -> tag %lx\n", x, y, (long) tag); last_tag = tag; }
    if (!o) {
        /* lumen: leaving a row or the top bar drops the focus to rest (a gap between tiles is inside
         * the row). The band reaches 100 px above the tiles: a tile engages only on its own area, so
         * the gap between the two edges keeps a hand's jitter from toggling the focus. */
        bool in_row = false;
        for (int r = 0; r < nrows; r++) {
            lv_area_t band = rows[r].cont->coords;
            band.y1 -= 100;
            in_row |= rows[r].shown && _lv_area_is_point_on(&band, &pt, 0);
        }
        if (root == scr && ui.engaged && !ui.moving && (ui.zone == Z_ROWS || ui.zone == Z_TOP) && !in_row) {
            LOG("pointer rest at %d,%d\n", x, y);
            ui.engaged = false;
            ui_refresh(true);
        }
        return;
    }
    if (o == overlay || (void *) tag == TAG_BACKDROP) {
        if (click) { overlay_close(); ui.zone = ui.zone == Z_FOLDER || ui.zone == Z_CTX ? Z_ROWS : Z_TOP; ui_refresh(true); }
        return;
    }
    zone_t z = (zone_t) ((tag >> 16) - 1);
    int a = (tag >> 8) & 0xff, b = tag & 0xff;
    bool outside_changed = false;   /* a row's column, the settings focus and the CC option live outside `ui` */
    ui.engaged = true;
    if (ui.dock_hidden) ui.dock_hidden = false;
    switch (z) {
        case Z_ROWS:
            /* while moving, focus stays on the carried app: hovering another tile drags it there */
            if (ui.moving) { if (ui.zone == Z_ROWS && rows[a].kind != 2) move_step((a - ui.row) * prefs.apps_row + b - rows[ui.row].col); }
            else if (ui.zone == Z_ROWS || ui.zone == Z_TOP) { outside_changed = rows[a].col != b; ui.zone = Z_ROWS; ui.row = a; rows[a].col = b; }
            break;
        case Z_TOP: if (ui.zone == Z_ROWS || ui.zone == Z_TOP) { ui.zone = Z_TOP; ui.top = b; } break;
        case Z_PANEL: ui.panel = b; break;
        case Z_FOLDER: ui.folder_sel = b; break;
        case Z_CTX: ui.ctx = b; break;
        case Z_CC: {
            int s0 = cc.opt_sel;
            cc_pointer(a, b);
            outside_changed = cc.opt_sel != s0;
            break;
        }
        case Z_SETTINGS: {
            int f0 = PG->focus, l0 = set_level;
            settings_pointer(a, b);
            outside_changed = PG->focus != f0 || set_level != l0;
            break;
        }
        case Z_SWITCHER: break;
    }
    /* motion events are a stream: only restyle when the hovered target actually changed */
    if (outside_changed || memcmp(&before, &ui, sizeof(ui))) ui_refresh(true);
    if (click) {
        if (z == Z_CC && a == 0 && b == CC_BACKLIGHT) { cc_backlight((x - CC_X) * 100 / (3 * CC_U + 2 * CC_GAP)); return; }
        handle(K_OK);
    }
}

/* pills are images, not tagged boxes: hit-test them by geometry */
static int pill_at(int x, int y) {
    for (int p = 0; p < 2; p++) if (x >= PILL_X[p] && x < PILL_X[p] + PILL && y >= PILL_Y && y < PILL_Y + PILL) return p;
    return -1;
}

/* ================================================================ Luna replies */

static cJSON *reply_json(const reply_t *r, bool *ok) {
    cJSON *j = cJSON_Parse(r->json);
    *ok = false;
    if (!j) return NULL;
    if (r->root) {
        cJSON *rv = cJSON_GetObjectItem(j, "returnValue"), *out = cJSON_GetObjectItem(j, "stdoutString");
        cJSON *inner = cJSON_IsString(out) ? cJSON_Parse(out->valuestring) : NULL;
        bool exec_ok = !cJSON_IsFalse(rv);
        cJSON_Delete(j);
        j = inner;
        if (!j) return NULL;
        *ok = exec_ok && !cJSON_IsFalse(cJSON_GetObjectItem(j, "returnValue"));
        return j;
    }
    *ok = !cJSON_IsFalse(cJSON_GetObjectItem(j, "returnValue"));
    return j;
}

static const char *settings_value(cJSON *j, const char *key) {
    cJSON *s = cJSON_GetObjectItem(j, "settings"), *v = s ? cJSON_GetObjectItem(s, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void apps_loaded(cJSON *j) {
    cJSON *lps = cJSON_GetObjectItem(j, "launchPoints"), *lp;
    if (!cJSON_IsArray(lps)) return;
    for (int i = 0; i < napps; i++) { free(apps[i].id); free(apps[i].title); free(apps[i].icon); }   /* a reload */
    napps = 0;
    cJSON_ArrayForEach(lp, lps) {
        cJSON *id = cJSON_GetObjectItem(lp, "id"), *title = cJSON_GetObjectItem(lp, "title"), *ic = cJSON_GetObjectItem(lp, "largeIcon");
        if (!cJSON_IsString(ic) || !ic->valuestring[0]) ic = cJSON_GetObjectItem(lp, "icon");   /* 80 px: enlarged on the tile */
        if (!cJSON_IsString(id) || !strcmp(id->valuestring, SELF_ID) || napps >= MAX_APPS) continue;
        app_t *a = &apps[napps++];
        memset(a, 0, sizeof(*a));
        a->id = strdup(id->valuestring);
        a->title = strdup(cJSON_IsString(title) ? title->valuestring : id->valuestring);
        a->icon = cJSON_IsString(ic) ? strdup(ic->valuestring) : NULL;
    }
    uint32_t t0 = SDL_GetTicks();
    for (int i = 0; i < napps; i++) app_prepare(&apps[i]);
    LOG("apps: %d, icons prepared in %u ms\n", napps, SDL_GetTicks() - t0);
    icons_copy();
    state_load();
    rows_build();
    panels_prefetch();
}

static void inputs_loaded(cJSON *j) {
    cJSON *devs = cJSON_GetObjectItem(j, "devices"), *d;
    if (!cJSON_IsArray(devs)) return;
    input_t tmp[12];
    int n = 0, ports[12];
    cJSON_ArrayForEach(d, devs) {
        cJSON *app = cJSON_GetObjectItem(d, "appId"), *lab = cJSON_GetObjectItem(d, "label"), *port = cJSON_GetObjectItem(d, "port");
        if (!cJSON_IsString(app) || !cJSON_IsString(lab) || n >= 11) continue;
        snprintf(tmp[n].app_id, 48, "%s", app->valuestring);
        snprintf(tmp[n].label, 48, "%s", lab->valuestring);
        ports[n++] = cJSON_IsNumber(port) ? port->valueint : 99;
    }
    for (int a = 0; a < n; a++)        /* by port, like lumen */
        for (int b = a + 1; b < n; b++)
            if (ports[b] < ports[a]) { input_t t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; int p = ports[a]; ports[a] = ports[b]; ports[b] = p; }
    ninputs = 1;                       /* Live TV first */
    memcpy(&inputs[1], tmp, sizeof(input_t) * n);
    ninputs += n;
    LOG("inputs: %d from eim\n", n);
    panels_prefetch();
}

static void replies_drain(void) {
    pthread_mutex_lock(&rq_mtx);
    reply_t q[64];
    int n = rq_len;
    memcpy(q, rq_queue, sizeof(reply_t) * n);
    rq_len = 0;
    pthread_mutex_unlock(&rq_mtx);
    for (int i = 0; i < n; i++) {
        rq_t k = q[i].kind;
        if (k == RQ_UNINSTALL) {                 /* raw exec: "gone" / "kept" on stdout */
            cJSON *o = cJSON_Parse(q[i].json), *out = o ? cJSON_GetObjectItem(o, "stdoutString") : NULL;
            char t[96];
            bool gone = cJSON_IsString(out) && strstr(out->valuestring, "gone");
            snprintf(t, sizeof(t), gone ? "%s uninstalled" : "%s is still installed", uninstall_title);
            show_toast(t, !gone);
            if (gone) rq_send(RQ_APPS, false, "luna://com.webos.applicationManager/listLaunchPoints", "{}", NULL);
            cJSON_Delete(o);
            free(q[i].json);
            continue;
        }
        if (k == RQ_HOMEHOOK) {                  /* raw exec: the hook's state on stdout */
            cJSON *j = cJSON_Parse(q[i].json);
            cJSON *out = j && !cJSON_IsFalse(cJSON_GetObjectItem(j, "returnValue")) ? cJSON_GetObjectItem(j, "stdoutString") : NULL;
            char was[sizeof(homehook_state)];
            memcpy(was, homehook_state, sizeof(was));
            snprintf(homehook_state, sizeof(homehook_state), "%s", cJSON_IsString(out) ? out->valuestring : "");
            homehook_state[strcspn(homehook_state, "\n")] = 0;
            LOG("homehook: %s\n", homehook_state[0] ? homehook_state : "(no root)");
            if (strcmp(was, homehook_state) && settings_layer && PG->kind == P_GROUP && PG->arg == SG_SYSTEM) {
                settings_render();
                ui_refresh(false);
            }
            cJSON_Delete(j);
            free(q[i].json);
            continue;
        }
        if (k == RQ_ICONS) {                     /* raw exec: whatever it printed, look at the files */
            LOG("icons exec: %.200s\n", q[i].json);
            icons_copied();
            free(q[i].json);
            continue;
        }
        bool ok;
        cJSON *j = reply_json(&q[i], &ok);
        if (!ok) {
            LOG("luna %d (%s) failed: %.200s\n", k, q[i].root ? "root" : "direct", q[i].json);
            if (!q[i].root) rq_send_path(k, true);                  /* retry as root */
            else if (rq_fail[k][0]) show_toast(rq_fail[k], true);
        } else switch (k) {
            case RQ_APPS: apps_loaded(j); break;
            case RQ_PIC: {
                const char *v = settings_value(j, "pictureMode"), *bl = settings_value(j, "backlight"), *eye = settings_value(j, "eyeComfortMode");
                if (v) snprintf(tv.picture, sizeof(tv.picture), "%s", v);
                if (bl && !cc.bl_due) tv.backlight = atoi(bl);
                if (bl && saver_dim_pending) saver_dim_apply(atoi(bl));
                if (eye) tv.eye = !strcmp(eye, "on");
                break;
            }
            case RQ_SND: { const char *v = settings_value(j, "soundMode"); if (v) snprintf(tv.sound, sizeof(tv.sound), "%s", v); break; }
            case RQ_SLEEP: {
                cJSON *s = cJSON_GetObjectItem(j, "sleepTimeSecond");
                int m = cJSON_IsNumber(s) ? (s->valueint + 59) / 60 : 0;
                tv.sleep_min = 0;
                for (int t = 1; m > 0 && t < COUNT(SLEEP_STEPS); t++) if (SLEEP_STEPS[t] >= m) { tv.sleep_min = SLEEP_STEPS[t]; break; }
                if (m > 0 && !tv.sleep_min) tv.sleep_min = SLEEP_STEPS[COUNT(SLEEP_STEPS) - 1];
                break;
            }
            case RQ_INPUTS: inputs_loaded(j); break;
            case RQ_RUNNING: {
                cJSON *arr = cJSON_GetObjectItem(j, "running"), *e;
                nrunning = 0;
                cJSON_ArrayForEach(e, arr) {
                    cJSON *id = cJSON_GetObjectItem(e, "id");
                    if (cJSON_IsString(id) && nrunning < 32) snprintf(running_ids[nrunning++], 96, "%s", id->valuestring);
                }
                dots_update();
                break;
            }
            case RQ_TIMEFMT: { cJSON *t = cJSON_GetObjectItem(j, "timeFormat"); sys_12h = cJSON_IsString(t) && !strcmp(t->valuestring, "HH12"); break; }
            default: break;
        }
        cJSON_Delete(j);
        free(q[i].json);
    }
    if (n) cc_refresh();
}

/* ================================================================ clock */

/* lumen picks the .wicon by matching words in the weather text; sun turns to moon 19:00-06:00 */
static void weather_icon_update(void) {
    char t[160];
    snprintf(t, sizeof(t), "%s", lv_label_get_text(status_wx_text));
    for (char *c = t; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    time_t now = time(NULL);
    int h = localtime(&now)->tm_hour;
    bool night = h >= 19 || h < 6;
    const lv_img_dsc_t *ic =
        strstr(t, "thunder") || strstr(t, "storm") ? &wx_storm
        : strstr(t, "snow") || strstr(t, "sleet") || strstr(t, "ice") ? &wx_snow
        : strstr(t, "rain") || strstr(t, "drizzle") || strstr(t, "shower") ? &wx_rain
        : strstr(t, "fog") || strstr(t, "mist") || strstr(t, "haze") || strstr(t, "smoke") || strstr(t, "dust") ? &wx_fog
        : strstr(t, "partly") || strstr(t, "mostly clear") || strstr(t, "few clouds") || strstr(t, "scattered") ? (night ? &wx_partly_moon : &wx_partly_sun)
        : strstr(t, "cloud") || strstr(t, "overcast") ? &wx_cloud
        : night ? &wx_moon : &wx_sun;
    if (lv_img_get_src(status_wx_icon) != ic) lv_img_set_src(status_wx_icon, ic);
}

/* .screen > .pill.status, shaped by Settings > Clock. One row: "Tue Oct 27" then the clock, the
 * weather under them. Stacked: a 104 px time, the long date, the weather without the city. The
 * clock is digits, digits with the weekday or seconds, a 50 px dial, or dial and digits. */
enum { CS_DIGITAL, CS_DATE, CS_SECONDS, CS_ANALOG, CS_BOTH };
static const uint32_t CLOCK_COLORS[7] = { C_TEXT, 0xffffff, 0x5ac8fa, 0x7dde6b, 0xff9f43, 0xff6fa8, 0xffd84a };
static lv_obj_t *clk_date, *clk_cdate, *clk_time, *clk_sec, *clk_period, *clk_hand[3];
static lv_point_t clk_hand_pt[3][2];

static lv_obj_t *flex_box(lv_obj_t *p, lv_flex_flow_t flow, lv_flex_align_t cross, int gap) {
    lv_obj_t *o = box(p, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, cross, cross);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *clk_line(lv_obj_t *p, lv_point_t *pt, int w, uint32_t color, lv_opa_t opa) {
    lv_obj_t *l = lv_line_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, 50, 50);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_line_set_points(l, pt, 2);
    return l;
}

/* .clock__dial: lumen's 40-unit SVG drawn 50 px (x1.25): ring at .35, four ticks at .7, hands */
static void clk_dial_build(lv_obj_t *p, uint32_t color) {
    static lv_point_t tick[4][2] = { { { 25, 4 }, { 25, 8 } }, { { 46, 25 }, { 42, 25 } },
                                     { { 25, 46 }, { 25, 42 } }, { { 4, 25 }, { 8, 25 } } };
    lv_obj_t *d = box(p, 0, 0, 50, 50);
    lv_obj_t *ring = box(d, 2, 2, 46, 46);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(ring, 0x59, 0);
    for (int i = 0; i < 4; i++) clk_line(d, tick[i], 2, color, 0xb3);
    static const int WD[3] = { 3, 2, 1 };
    for (int i = 0; i < 3; i++) clk_hand[i] = clk_line(d, clk_hand_pt[i], WD[i], color, LV_OPA_COVER);
    lv_obj_t *hub = box(d, 23, 23, 4, 4);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
}

static void clk_dial_update(const struct tm *lt) {
    if (!clk_hand[0]) return;
    static const float LEN[3] = { 9 * 1.25f, 13.5f * 1.25f, 15 * 1.25f };
    float s = lt->tm_sec, m = lt->tm_min + s / 60, h = lt->tm_hour % 12 + m / 60;
    float deg[3] = { h * 30, m * 6, s * 6 };
    for (int i = 0; i < 3; i++) {
        float a = deg[i] * (float) M_PI / 180;
        clk_hand_pt[i][0] = (lv_point_t) { 25, 25 };
        clk_hand_pt[i][1] = (lv_point_t) { (lv_coord_t) lroundf(25 + LEN[i] * sinf(a)), (lv_coord_t) lroundf(25 - LEN[i] * cosf(a)) };
        lv_line_set_points(clk_hand[i], clk_hand_pt[i], 2);
    }
}

/* html.clock-small/-large/-xl/-huge zoom the whole block, its offsets from the edges included */
static const int CLOCK_ZOOM[5] = { 205, 256, 320, 384, 563 };
static int status_dock_dy(void) { return prefs.clock_stacked ? 730 : 830; }

/* Centred and right-aligned blocks change width with the time, so they are placed on every change. */
static void status_place(void) {
    int z = CLOCK_ZOOM[prefs.clock_size];
    lv_obj_update_layout(status_cont);
    int w = lv_obj_get_width(status_cont);
    int x = prefs.clock_pos == 0 ? EDGE * z / 256 : prefs.clock_pos == 1 ? (SCR_W - w) / 2 : SCR_W - 340 * z / 256 - w;
    lv_obj_set_pos(status_cont, x, 64 * z / 256);
    lv_obj_set_style_transform_pivot_x(status_cont, prefs.clock_pos == 0 ? 0 : prefs.clock_pos == 1 ? w / 2 : w, 0);
}

static void status_wx_set(const char *t) {
    if (prefs.clock_stacked) {                         /* .status__city is hidden when stacked */
        const char *dot = strstr(t, " \xC2\xB7 ");
        if (dot) t = dot + 4;
    }
    lv_label_set_text(status_wx_text, t);
}

static void status_build(void) {
    uint32_t idx = 0;
    if (status_cont) { idx = lv_obj_get_index(status_cont); lv_obj_del(status_cont); }
    bool stacked = prefs.clock_stacked;
    int st = prefs.clock_style, w = prefs.clock_weight;
    lv_flex_align_t cross = prefs.clock_pos == 0 ? LV_FLEX_ALIGN_START : prefs.clock_pos == 1 ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_END;
    status_cont = flex_box(scr, LV_FLEX_FLOW_COLUMN, cross, 4);
    if (idx) lv_obj_move_to_index(status_cont, (int32_t) idx);
    uint32_t color = CLOCK_COLORS[prefs.clock_color];
    lv_opa_t look = prefs.clock_frosted ? 0xa6 : LV_OPA_COVER;     /* frosted: see-through white */
    const lv_font_t *tf = stacked ? (w == 0 ? &clk_104l : w == 2 ? &clk_104b : &clk_104s)
                                  : (w == 0 ? &geist_30l : w == 2 ? &geist_30b : &geist_30m);
    const lv_font_t *small = stacked ? &clk_84s : &geist_24m;

    lv_obj_t *row = stacked ? status_cont : flex_box(status_cont, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, 12);
    if (!stacked) clk_date = label(row, tf, C_TEXT, "");               /* .status__row .status__date */
    lv_obj_t *clock = flex_box(row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, 12);
    clk_cdate = clk_time = clk_sec = clk_period = NULL;
    memset(clk_hand, 0, sizeof(clk_hand));
    if (st == CS_ANALOG || st == CS_BOTH) clk_dial_build(clock, color);
    if (st != CS_ANALOG) {
        lv_obj_t *digits = flex_box(clock, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_END, 0);
        if (st == CS_DATE) {                                            /* .clock__date .82em .85, 14px gap */
            clk_cdate = label(digits, small, color, "");
            lv_obj_set_style_text_opa(clk_cdate, (lv_opa_t) (look * 0xd9 / 255), 0);
            lv_obj_set_style_pad_right(clk_cdate, 14, 0);
        }
        clk_time = label(digits, tf, color, "");
        lv_obj_set_style_text_opa(clk_time, look, 0);
        if (st == CS_SECONDS) {                                         /* .clock__sec .8em .7 */
            clk_sec = label(digits, small, color, "");
            lv_obj_set_style_text_opa(clk_sec, 0xb3, 0);
            lv_obj_set_style_pad_left(clk_sec, 2, 0);
        }
        const lv_font_t *pf = stacked ? &geist_36m : &geist_30m;       /* .clock__period: align-self centre */
        clk_period = label(digits, pf, color, "");
        lv_obj_set_style_text_opa(clk_period, prefs.clock_frosted ? 0xb3 : LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(clk_period, 6, 0);
        lv_obj_set_style_pad_bottom(clk_period, (lv_font_get_line_height(tf) - lv_font_get_line_height(pf)) / 2, 0);
    }
    if (stacked) {                                                      /* .status__date--long 32/500 */
        clk_date = label(status_cont, &geist_32m, C_TEXT, "");
        lv_obj_set_style_pad_left(clk_date, 4, 0);
    }
    lv_obj_set_style_text_opa(clk_date, look, 0);

    /* .status__weather: 22px/500 at .82 led by the .wicon glyph (10px gap); 28px when stacked */
    status_wx = flex_box(status_cont, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, 10);
    if (stacked) lv_obj_set_style_pad_left(status_wx, 4, 0);
    status_wx_icon = image(status_wx, &wx_cloud);
    lv_obj_set_style_img_opa(status_wx_icon, 0xd1, 0);
    status_wx_text = label(status_wx, stacked ? &geist_28m : &geist_22m, C_TEXT, "");
    lv_obj_set_style_text_opa(status_wx_text, 0xd1, 0);
    pthread_mutex_lock(&wx_mtx);
    status_wx_set(wx_text);
    pthread_mutex_unlock(&wx_mtx);
    lv_obj_set_style_opa(status_wx, lv_label_get_text(status_wx_text)[0] ? LV_OPA_COVER : 0, 0);
    if (stacked && prefs.clock_below) {                                 /* .clock-timebelow: date, weather, time */
        lv_obj_move_to_index(clock, -1);
        lv_obj_set_style_pad_top(clock, 8, 0);
    }

    int z = CLOCK_ZOOM[prefs.clock_size];
    lv_obj_set_style_transform_pivot_y(status_cont, 0, 0);
    lv_obj_set_style_transform_zoom(status_cont, (lv_coord_t) z, 0);
    lv_obj_set_style_translate_y(status_cont, ui.dock_hidden ? status_dock_dy() : 0, 0);
    clock_tick(NULL);
}

static void clock_tick(lv_timer_t *t) {
    (void) t;
    static const char *WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *WDL[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *MO[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *MOL[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                 "September", "October", "November", "December" };
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    bool h12 = clock_fmt == 1 || (clock_fmt == 0 && sys_12h), changed = false;
    char s[48];
    /* stacked shows the long date; one row "Tue Oct 27" */
    if (prefs.clock_stacked) snprintf(s, sizeof(s), "%s, %s %d", WDL[lt->tm_wday], MOL[lt->tm_mon], lt->tm_mday);
    else snprintf(s, sizeof(s), "%s %s %d", WD[lt->tm_wday], MO[lt->tm_mon], lt->tm_mday);
    bool date_on = prefs.show_weather && prefs.show_date;
    if (date_on == lv_obj_has_flag(clk_date, LV_OBJ_FLAG_HIDDEN)) {
        if (date_on) lv_obj_clear_flag(clk_date, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(clk_date, LV_OBJ_FLAG_HIDDEN);
        changed = true;
    }
    if (strcmp(lv_label_get_text(clk_date), s)) { lv_label_set_text(clk_date, s); changed = true; }
    if (clk_time) {
        if (h12) snprintf(s, sizeof(s), "%d:%02d", lt->tm_hour % 12 ? lt->tm_hour % 12 : 12, lt->tm_min);
        else snprintf(s, sizeof(s), "%02d:%02d", lt->tm_hour, lt->tm_min);
        if (strcmp(lv_label_get_text(clk_time), s)) { lv_label_set_text(clk_time, s); changed = true; }
        const char *p = h12 && prefs.show_ampm ? (lt->tm_hour < 12 ? "AM" : "PM") : "";
        if (strcmp(lv_label_get_text(clk_period), p)) { lv_label_set_text(clk_period, p); changed = true; }
        if (clk_sec) {
            snprintf(s, sizeof(s), ":%02d", lt->tm_sec);
            lv_label_set_text(clk_sec, s);
            changed |= prefs.clock_pos != 0;                            /* digits are not tabular */
        }
        if (clk_cdate) {
            snprintf(s, sizeof(s), "%s %d", WD[lt->tm_wday], lt->tm_mday);
            if (strcmp(lv_label_get_text(clk_cdate), s)) { lv_label_set_text(clk_cdate, s); changed = true; }
        }
    }
    clk_dial_update(lt);
    if (prefs.show_weather == lv_obj_has_flag(status_wx, LV_OBJ_FLAG_HIDDEN)) {
        if (prefs.show_weather) lv_obj_clear_flag(status_wx, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(status_wx, LV_OBJ_FLAG_HIDDEN);
        changed = true;
    }
    if (changed || !t) status_place();

    if (lt->tm_sec == 0) weather_icon_update();
    bool night = prefs.wp_dim == 2 || (prefs.wp_dim == 1 && (lt->tm_hour >= 19 || lt->tm_hour < 6));
    if (night != night_now) {
        night_now = night;
        if (!lv_anim_get(layers, fade_exec)) anim_opa(layers[L_WP].img[layers[L_WP].front], layer_rest_opa(L_WP), 600);
    }
}

/* ================================================================ build */

static void build_ui(void) {
    scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x10131c), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    dual_create(L_WP, scr);
    dual_create(L_VEIL, scr);
    for (int i = 0; i < 2; i++) zoom_full(layers[L_VEIL].img[i]);
    for (int i = 0; i < 2; i++) lv_obj_set_style_opa(layers[L_VEIL].img[i], 0, 0);
    /* .dockdim: the wallpaper fades darker while the dock is away (Settings > Home Screen) */
    dockdim = box(scr, 0, 0, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(dockdim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dockdim, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(dockdim, 0, 0);

    /* rows: absolute containers sized to their content; y is animated (lower / dock hide).
     * Margins: the tray image's baked shadow (~90px), a focused tile's zoom + lift + shadow. */
    rows_cont = group(scr, 0, ROWS_TOP, SCR_W, 3 * ROW_PITCH, 120);
    lv_obj_t *tray_holder = group(rows_cont, 0, 0, SCR_W, TILE_H, 120);
    dual_create(L_TRAY, tray_holder);
    for (int r = 0; r < 3; r++) {
        rows[r].cont = group(rows_cont, 0, r * ROW_PITCH, SCR_W, ROW_PITCH, 80);
        rows[r].track = group(rows[r].cont, 0, 0, 1, ROW_PITCH, 80);
        rows[r].shown = r == 0;
        if (r) lv_obj_set_style_opa(rows[r].cont, 0, 0);
        rows[r].label = label(rows[r].track, &geist_19s, C_TEXT, "");
    }

    status_build();

    /* .topbar: Inputs and Control Center pills, labels under them on focus */
    static const char *names[2] = { "Inputs", "Control Center" };
    for (int p = 0; p < 2; p++) {
        dual_create(L_PILL0 + p, scr);
        pill_icon[p] = icon(scr, p == 0 ? &ic_inputs_28 : &ic_sliders_28);
        lv_obj_set_pos(pill_icon[p], PILL_X[p] + (PILL - 28) / 2, PILL_Y + (PILL - 28) / 2);
        lv_img_set_pivot(pill_icon[p], 14, 14);
        topbar_lbl[p] = label(scr, &geist_20m, C_TEXT, names[p]);
        lv_obj_update_layout(topbar_lbl[p]);
        /* centred under its pill; the last pill's label ends at the pill's right edge */
        int lx = p == 1 ? PILL_X[p] + PILL - lv_obj_get_width(topbar_lbl[p]) : PILL_X[p] + PILL / 2 - lv_obj_get_width(topbar_lbl[p]) / 2;
        lv_obj_set_pos(topbar_lbl[p], lx, PILL_Y + 72);
        lv_obj_set_style_opa(topbar_lbl[p], 0, 0);
    }

    dockhint = image(scr, &ic_chevron_up);
    lv_obj_set_pos(dockhint, SCR_W / 2 - 26, SCR_H - 34 - 30);
    lv_obj_set_style_opa(dockhint, 0, 0);

    movehint = label(scr, &geist_26m, C_TEXT, "Move with the arrows \xC2\xB7 OK to drop");
    lv_obj_set_width(movehint, SCR_W);
    lv_obj_set_style_text_align(movehint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(movehint, 0, 120);
    lv_obj_add_flag(movehint, LV_OBJ_FLAG_HIDDEN);

    /* .toast: bottom 56px, pill, --color-tile-bg-focus */
    toast = box(lv_layer_sys(), 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(toast, 999, 0);
    lv_obj_set_style_bg_color(toast, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(toast, 0x2e, 0);
    lv_obj_set_style_pad_hor(toast, 28, 0);
    lv_obj_set_style_pad_ver(toast, 14, 0);
    label(toast, &geist_24m, 0xAAAAAA, "");
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    clock_tick(NULL);
    lv_timer_create(clock_tick, 1000, NULL);
}

/* ================================================================ test hook + bench */

static uint32_t bf_n, bf_sum, bf_max, bf_o16, bf_o33;

#ifdef MYTV_DEBUG
/* `echo "<n> <cmd>" > /tmp/launcher_cmd` (root, over telnet). The launcher cannot delete a
 * root-owned file in sticky /tmp, so a command runs once per new sequence number. */
static int bench_step;
static const int BENCH[] = { K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_RIGHT, K_LEFT, K_LEFT, K_DOWN, K_RIGHT, K_RIGHT, K_DOWN,
                             K_UP, K_UP, K_UP, K_RIGHT, K_OK, K_DOWN, K_DOWN, K_DOWN, K_RIGHT, K_BACK, K_LEFT, K_OK, K_RIGHT,
                             K_BACK, K_DOWN, K_DOWN, K_DOWN, K_LEFT };

static void bench_cb(lv_timer_t *t) {
    if (bench_step == 0) { ui_rest(); bench_on = true; bf_n = bf_sum = bf_max = bf_o16 = bf_o33 = 0; }
    if (bench_step <= COUNT(BENCH)) handle(bench_step == 0 ? K_RIGHT : BENCH[bench_step - 1]);
    if (++bench_step > COUNT(BENCH) + 1) {
        bench_on = false;
        LOG("BENCH frames=%u avg=%ums max=%ums >16ms=%u >33ms=%u\n", bf_n, bf_n ? bf_sum / bf_n : 0, bf_max, bf_o16, bf_o33);
        ui_rest();
        lv_timer_del(t);
    }
}

/* Test hook `scroll <ms> <n>`: |n| arrow presses <ms> apart, as a held key sends them (n < 0: Up). */
static int scroll_left, scroll_ms;
static void scroll_cb(lv_timer_t *t) {
    if (!scroll_left) { lv_timer_del(t); return; }
    handle(scroll_left > 0 ? K_DOWN : K_UP);
    scroll_left += scroll_left > 0 ? -1 : 1;
}

static int cmd_read(char c[32]) {
    FILE *f = fopen("/tmp/launcher_cmd", "r");
    if (!f) return -1;
    int seq = -1;
    if (fscanf(f, "%d %31[^\n]", &seq, c) != 2) seq = -1;   /* the rest of the line: hooks take arguments */
    fclose(f);
    return seq;
}

static int cmd_last_seq = -1;   /* the file left by an earlier run is read once at start-up */
#endif
/* Home while we are in front: inputhook's Home bind re-launches us, and SDL-webOS only raises
 * the window it already has, sending no event. The one trace is its lifecycle log line. */
static SDL_LogOutputFunction sdl_log_default;
static void *sdl_log_default_data;
static volatile int relaunched;
static void sdl_log(void *data, int category, SDL_LogPriority priority, const char *msg) {
    if (category == SDL_LOG_CATEGORY_SYSTEM && strstr(msg, "lifecycleEvent") && strstr(msg, "relaunch")) {
        __atomic_store_n(&relaunched, 1, __ATOMIC_RELEASE);
        main_wake();
    }
    if (priority >= SDL_LOG_PRIORITY_WARN || category != SDL_LOG_CATEGORY_SYSTEM) sdl_log_default(sdl_log_default_data, category, priority, msg);
}

#ifdef MYTV_DEBUG
static int hover_x, hover_y;

static void cmd_cb(lv_timer_t *t) {
    (void) t;
    char c[32] = "";
    int seq = cmd_read(c);
    if (seq < 0 || seq == cmd_last_seq) return;
    cmd_last_seq = seq;
    LOG("cmd %d %s\n", seq, c);
    static const char *K[] = { "left", "right", "up", "down", "ok", "back", "hold" };
    for (int i = 0; i < COUNT(K); i++) if (!strcmp(c, K[i])) { handle(i); return; }
    if (!strcmp(c, "rest")) ui_rest();
    else if (!strcmp(c, "rotate")) wp_switch(wp_next_take());
    else if (!strcmp(c, "saver")) saver_start();
    else if (!strcmp(c, "wpchoose")) { if (!settings_layer) open_settings(); settings_push(P_WALLPAPER, 0); }
    else if (!strcmp(c, "countries")) { if (!settings_layer) open_settings(); settings_push(P_CHOICE, O_COUNTRY); }
    else if (sscanf(c, "scroll %d %d", &scroll_ms, &scroll_left) == 2) lv_timer_create(scroll_cb, (uint32_t) scroll_ms, NULL);
    else if (sscanf(c, "hover %d %d", &hover_x, &hover_y) == 2) pointer_focus(hover_x, hover_y, false);
    else if (sscanf(c, "dump %d %d %d %d %d", &dump_left, &dump_rect.x, &dump_rect.y, &dump_rect.w, &dump_rect.h) == 5) dump_idx = 0;
    else if (!strcmp(c, "standby")) { int t = prefs.ss_type; prefs.ss_type = 2; saver_start(); prefs.ss_type = t; }   /* not saved */
    else if (!strcmp(c, "statbeg")) { bench_on = true; bf_n = bf_sum = bf_max = bf_o16 = bf_o33 = 0; }
    else if (!strcmp(c, "statend")) {
        bench_on = false;
        LOG("STAT frames=%u avg=%ums max=%ums >16ms=%u >33ms=%u\n", bf_n, bf_n ? bf_sum / bf_n : 0, bf_max, bf_o16, bf_o33);
    }
    else if (!strcmp(c, "bench")) { bench_step = 0; lv_timer_create(bench_cb, 350, NULL); }
}
#endif

/* ================================================================ main */

/* myTV is in front again after another app or an input. */
static void back_in_front(void) {
    ui_rest();     /* lumen: coming back to the launcher = rest state, dock shown */
    last_input = SDL_GetTicks();
    rq_send(RQ_TIMEFMT, false, "luna://com.webos.service.systemservice/getPreferences", "{\"keys\":[\"timeFormat\"]}", NULL);
    if (app_away && !prefs.wp_plain && prefs.wp_new) wp_switch(wp_next_take());
    __atomic_store_n(&app_away, false, __ATOMIC_RELEASE);
    worker_wake();        /* the pictures wp_px_shed dropped */
    running_poll(NULL);
}

int main(void) {
    SDL_SetMainReady();
#ifdef MYTV_DEBUG
    lg = fopen("/tmp/launcher.log", "w");
#endif
    if (mkdir(DATA_DIR, 0777) && errno != EEXIST) LOG("cannot create " DATA_DIR ": %s\n", strerror(errno));
    /* Back reaches us instead of webOS's own handling, as SDL_SCANCODE_WEBOS_BACK (the remote's
     * key has no keycode, so match it by scancode). */
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    SDL_LogGetOutputFunction(&sdl_log_default, &sdl_log_default_data);
    SDL_LogSetOutputFunction(sdl_log, NULL);
    SDL_LogSetPriority(SDL_LOG_CATEGORY_SYSTEM, SDL_LOG_PRIORITY_INFO);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { LOG("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("launcher", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       SCR_W, SCR_H, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    lv_init();
    display_init(ren, SCR_W, SCR_H);
    bez_init();
    styles_init();
    prefs_load();
    styles_focus_apply();
    motion_apply();
    saver_policy_apply();

    srand((unsigned) time(NULL));
    weather_apply_prefs();
    uint32_t t0 = SDL_GetTicks();
    wp_cur = WP_COUNT;           /* nothing on screen yet: any pick may come first */
    wp_cur = prefs.wp_plain ? WP_PLAIN : prefs.wp_new ? wp_random_pick() : __builtin_ctz(prefs.picks);
    wp_decode(wp_cur);
    if (!wp_dsc[wp_cur].data) wp_decode(wp_cur = WP_PLAIN);   /* a missing picture must not leave a hole */
    wp_prepare(wp_cur);
    LOG("wallpaper %d decoded + glass in %u ms\n", wp_cur, SDL_GetTicks() - t0);
    /* fixed-size panels are rendered ahead for every wallpaper; data-sized ones follow later */
    for (int m = 0; m < 2 * CC_N; m++) { int mx, my, mw; cc_rect(m / 2, &mx, &my, &mw); glass_job(G_CCM + m, mw, CC_U); }
    for (int o = 0; o < 16; o++) glass_job(G_CCO + o, CC_W, CC_OPT_H);
    for (int n = 1; n <= 7; n++) glass_job(G_CTX, 440, ctx_height(n));
    pthread_t wt;
    pthread_create(&wt, NULL, glass_worker, NULL);

    /* Live TV is always offered first; eim fills in the HDMI ports */
    snprintf(inputs[0].app_id, 48, "com.webos.app.livetv"); snprintf(inputs[0].label, 48, "Live TV");
    for (int i = 1; i <= 4; i++) { snprintf(inputs[i].app_id, 48, "com.webos.app.hdmi%d", i); snprintf(inputs[i].label, 48, "HDMI %d", i); }
    ninputs = 5;

    build_ui();
    ui_rest();
    wp_timer = lv_timer_create(wp_rotate_cb, 60000, NULL);
    wp_timer_update();
#ifdef MYTV_DEBUG
    char stale[32];
    cmd_last_seq = cmd_read(stale);
    lv_timer_create(cmd_cb, 200, NULL);
#endif
    lv_timer_t *rp = lv_timer_create(running_poll, 2500, NULL);   /* lumen: first look after 2.5 s */
    lv_timer_set_repeat_count(rp, 1);
    lv_timer_create(running_poll, 5000, NULL);
    rq_send(RQ_APPS, false, "luna://com.webos.applicationManager/listLaunchPoints", "{}", NULL);
    rq_send(RQ_INPUTS, false, "luna://com.webos.service.eim/getAllInputStatus", "{}", NULL);
    rq_send(RQ_TIMEFMT, false, "luna://com.webos.service.systemservice/getPreferences", "{\"keys\":[\"timeFormat\"]}", NULL);
    pthread_t wxt;
    pthread_create(&wxt, NULL, weather_thread, NULL);

    uint32_t last_key = 0, ok_down_at = 0;
    bool ok_down = false, ok_long = false;
    int last_k = -1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_HIDDEN)) {
                LOG("window event %d: away\n", e.window.event);
                __atomic_store_n(&app_away, true, __ATOMIC_RELEASE);
                ok_down = false;      /* its key-up goes to whoever took focus */
            } else if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_SHOWN)) {
                LOG("window event %d: back to rest\n", e.window.event);
                back_in_front();
            } else if (e.type == SDL_KEYDOWN) {
                last_input = SDL_GetTicks();
                SDL_Keycode s = e.key.keysym.sym;
                int k = e.key.keysym.scancode == SDL_SCANCODE_WEBOS_BACK ? K_BACK : s == SDLK_LEFT ? K_LEFT : s == SDLK_RIGHT ? K_RIGHT : s == SDLK_UP ? K_UP : s == SDLK_DOWN ? K_DOWN
                      : (s == SDLK_RETURN || s == SDLK_KP_ENTER) ? K_OK
                      : (s == SDLK_ESCAPE || s == SDLK_BACKSPACE || s == SDLK_AC_BACK) ? K_BACK
                      : (s >= SDLK_1 && s <= SDLK_6) ? K_NUM1 + (int) (s - SDLK_1)
                      : (s >= SDLK_KP_1 && s <= SDLK_KP_6) ? K_NUM1 + (int) (s - SDLK_KP_1) : -1;
                if (k < 0) continue;
                uint32_t now = SDL_GetTicks();
                if (bench_on) LOG("key %d %s %u\n", k, e.key.repeat ? "rep" : "new", now);
                if (e.key.repeat && k == last_k && now - last_key < KEY_REPEAT_MS) continue;
                last_key = now; last_k = k;
                if (k == K_OK && ok_down) continue;   /* repeats of a held OK must not click what the hold opened */
                /* OK in rows and folders is decided on release: a tap activates, a hold opens the menu */
                if (k == K_OK && !saver_active() && ui.engaged && !ui.dock_hidden && !ui.moving && (ui.zone == Z_ROWS || ui.zone == Z_FOLDER)) {
                    if (!e.key.repeat && !ok_down) { ok_down = true; ok_long = false; ok_down_at = now; }
                    continue;
                }
                handle(k);
            } else if (e.type == SDL_KEYUP) {
                SDL_Keycode s = e.key.keysym.sym;
                if ((s == SDLK_RETURN || s == SDLK_KP_ENTER) && ok_down) {
                    ok_down = false;
                    if (!ok_long) handle(K_OK);
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                last_input = SDL_GetTicks();
                if (saver_active()) continue;             /* the pointer must not steer what is hidden under it */
                if (ui.dock_hidden) { ui.dock_hidden = false; ui_refresh(true); }   /* any pointer move brings the dock back */
                int p = pill_at(e.motion.x, e.motion.y);
                if (p >= 0 && !overlay && !settings_layer) { ui.engaged = true; ui.zone = Z_TOP; ui.top = p; ui.dock_hidden = false; ui_refresh(true); }
                else pointer_focus(e.motion.x, e.motion.y, false);
            } else if (e.type == SDL_MOUSEBUTTONDOWN && !saver_active() && pill_at(e.button.x, e.button.y) < 0) {
                /* lumen rows onMouseDown: holding the pointer's OK on a tile opens its menu, as the key does */
                pointer_focus(e.button.x, e.button.y, false);
                if (ui.engaged && !ui.dock_hidden && !ui.moving && (ui.zone == Z_ROWS || ui.zone == Z_FOLDER)) {
                    ok_down = true; ok_long = false; ok_down_at = SDL_GetTicks();
                }
            } else if (e.type == SDL_MOUSEBUTTONUP && saver_active()) {
                handle(K_BACK);
            } else if (e.type == SDL_MOUSEBUTTONUP) {
                bool held = ok_down && ok_long;
                ok_down = false;
                if (held) continue;      /* the hold opened the menu: the release must not click in it */
                int p = pill_at(e.button.x, e.button.y);
                if (p >= 0 && !overlay && !settings_layer) { ui.engaged = true; ui.zone = Z_TOP; ui.top = p; handle(K_OK); }
                else pointer_focus(e.button.x, e.button.y, true);
            }
        }
        if (__atomic_exchange_n(&relaunched, 0, __ATOMIC_ACQUIRE)) {
            ok_down = false;
            /* A launch raises us, but after a wake where bootd showed an input first no focus
             * event follows: without this the launcher would stay "away" in front. */
            if (app_away) { LOG("relaunched from away: back to rest\n"); back_in_front(); }
            else {
                LOG("relaunched in front: home\n");
                if (saver_active()) handle(K_BACK);
                ui_rest();
            }
        }
        if (ok_down && !ok_long && SDL_GetTicks() - ok_down_at >= LONG_PRESS_MS) { ok_long = true; handle(K_HOLD); }
        /* "After idle": the dock slides away after 8 s without input on the home screen */
        if ((prefs.dock_hide & 2) && !ui.dock_hidden && ui.zone == Z_ROWS && !ui_busy() && SDL_GetTicks() - last_input > IDLE_HIDE_MS) {
            ui.dock_hidden = true;
            ui_refresh(true);
        }
        /* lumen idle-screensaver: only while the launcher is in front */
        if (prefs.ss_type != 0 && prefs.idle_min && !saver_active() && !app_away && SDL_GetTicks() - last_input > (uint32_t) prefs.idle_min * 60000) {
            last_input = SDL_GetTicks();
            saver_start();
        }
        if (wp_pending >= 0 && wp_warm(wp_pending) && !lv_anim_get(layers, fade_exec)) wp_switch(wp_pending);
        /* nothing may still draw a picture it drops: no cross-fade, no photo saver */
        if (app_away && !saver_active() && !lv_anim_get(layers, fade_exec)) wp_px_shed();
        /* Textures needed soon, uploaded while nothing moves (the saver's faces always move, so
         * it takes one small piece per pass instead) */
        if (saver_active()) {
            uint32_t w0 = SDL_GetTicks();
            if (saver_warm() && SDL_GetTicks() - w0 > 8) LOG("saver texture uploaded ahead in %u ms\n", SDL_GetTicks() - w0);
        } else if (!app_away && !lv_anim_count_running() && !scrolling()) {
            /* the focused painting goes at once: OK on it follows within a second */
            int f = wp_focused();
            if (f >= 0) wp_upload(f);
            else if (SDL_GetTicks() - last_input > 1500) wp_upload_ahead();
        }
        replies_drain();
        cc_backlight_flush();
        if (wx_ready) {
            pthread_mutex_lock(&wx_mtx);
            status_wx_set(wx_text);
            weather_icon_update();
            status_place();
            if (saver_active() && prefs.show_weather) saver_set_weather(wx_text);
            wx_ready = false;
            pthread_mutex_unlock(&wx_mtx);
            if (!lv_obj_get_style_opa(status_wx, 0)) anim_opa(status_wx, LV_OPA_COVER, 600);   /* status-in .6s */
        }
        if (toast_until && SDL_GetTicks() > toast_until) { lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN); toast_until = 0; }
        bool vsync_paced = scroll_frame_due();
        uint32_t f0 = SDL_GetTicks();
        uint32_t idle = lv_timer_handler();
        if (vsync_paced) idle = 0;
        uint32_t dt = SDL_GetTicks() - f0;
        static bool first_frame = true;
        if (first_frame) { first_frame = false; LOG("first frame at %u ms\n", SDL_GetTicks()); }
        if (dt > 20) LOG("[%u] slow frame %u ms\n", f0, dt);
        if (bench_on) {
            bf_n++; bf_sum += dt;
            if (dt > bf_max) bf_max = dt;
            if (dt > 16) bf_o16++;
            if (dt > 33) bf_o33++;
        }
        /* Sleep until the next LVGL timer or an event; keys and thread results (main_wake) cut it
         * short. The cap bounds the polled checks above: long press, idle timers, toasts. */
        SDL_WaitEventTimeout(NULL, (int) (idle < 50 ? idle : 50));
    }
    running = false;
    SDL_Quit();
    return 0;
}
