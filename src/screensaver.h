/* lumen 1.1.4 full-screen modes: StandBy (eight faces after Apple's StandBy) and the photo
 * screensaver ("Photos in Lumen"). Both live on lv_layer_top above everything; the launcher
 * only routes keys here while one is up and stores the face/colour the user left it on. */
#pragma once
#include <stdbool.h>
#include "lvgl/lvgl.h"

enum { SB_DIGITAL, SB_ANALOG, SB_CALENDAR, SB_SOLAR, SB_PHOTOS, SB_WEATHER, SB_FADE, SB_FLOW, SB_FACES };
enum { SV_LEFT, SV_RIGHT, SV_UP, SV_DOWN, SV_OTHER };
enum { PC_OFF, PC_BL, PC_BC, PC_BR, PC_TL, PC_TC, PC_TR, PC_C };   /* photo clock position */

typedef struct {
    bool h12, ampm;                     /* the TV's clock format; AM/PM shown in 12 h */
    char weather[160];                  /* "City · 12° Cloudy", "" when off or unknown */
    int night;                          /* StandBy Night Mode: 0 auto (19:00-06:00), 1 on, 2 off */
    const lv_img_dsc_t *photos[8];      /* full-screen pictures for the photo saver / face */
    int nphotos;
    int photo_seconds;
    bool print, frosted, info, dim;     /* print look, frosted clock, date+weather, wallpaper dim */
    int pc_pos, pc_size;                /* clock position (PC_*), size 0 small .. 4 huge */
    bool reduce_motion;
} saver_env_t;

void standby_open(const saver_env_t *env, int face, int color);
void photos_open(const saver_env_t *env);
bool saver_active(void);
/* A remote key while a saver is up. Returns false when it closed the saver. */
bool saver_key(int key);
void saver_close(void);
int standby_face(void);
int standby_color(void);
void saver_set_weather(const char *text);
/* Upload one texture the saver will need soon: the next photo, then StandBy's big glyphs one
 * by one. Call while a saver is up, once per main-loop pass. Returns 1 when it uploaded. */
int saver_warm(void);
