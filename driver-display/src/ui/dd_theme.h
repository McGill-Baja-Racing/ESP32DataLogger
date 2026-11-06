#ifndef DD_THEME_H
#define DD_THEME_H

#include <stddef.h>

#include "lvgl.h"

/* Colours, taken from the driver view designs. Pure black background with bright
 * text, so the screen stays readable in sunlight and at a glance. */
#define DD_COLOR_BG 0x000000
#define DD_COLOR_TEXT 0xF5F7FA       /* main values: speed, lap times, lit bars */
#define DD_COLOR_TEXT_DIM 0xA9B0BA   /* secondary values: lap number, temperatures */
#define DD_COLOR_LABEL 0x8D96A3      /* captions: BRAKE, FUEL, KM/H */
#define DD_COLOR_LABEL_DIM 0x5E6672  /* captions of secondary values, unavailable data */
#define DD_COLOR_TRACK 0x15181D      /* empty bars and unlit segments */
#define DD_COLOR_DIVIDER 0x1E232A    /* divider lines, panel borders */
#define DD_COLOR_GOOD 0x30D158       /* shift window, faster lap */
#define DD_COLOR_GOOD_DIM 0x0E2A17   /* unlit shift window segments */
#define DD_COLOR_WARNING 0xFF9F0A    /* needs attention soon */
#define DD_COLOR_CRITICAL 0xFF453A   /* act now */

/* Text shown in place of a value that is missing or too old. */
#define DD_TEXT_UNAVAILABLE "--"

/* How a value should be read. Each component maps these to its own colours. */
typedef enum {
    DD_LEVEL_NORMAL,
    DD_LEVEL_GOOD,
    DD_LEVEL_WARNING,
    DD_LEVEL_CRITICAL,
} dd_level_t;

/* Text styles. Each sets the font, size and letter spacing together. */
typedef enum {
    DD_TEXT_CAPTION,  /* 14 px, spaced capitals: BRAKE, FUEL, LAP TIME */
    DD_TEXT_LABEL,    /* 16 px, spaced capitals: CVT, LAP, KM/H */
    DD_TEXT_BODY,     /* 18 px: alert details */
    DD_TEXT_ROW,      /* 24 px: list rows */
    DD_TEXT_ACTION,   /* 28 px, spaced capitals: PIT NOW, view titles */
    DD_TEXT_PERCENT,  /* 29 px: fuel percentage */
    DD_TEXT_TITLE,    /* 40 px: alert title */
    DD_TEXT_VALUE,    /* 41 px: header values, trend values */
    DD_TEXT_STAT,     /* 64 px: lap times, tile values */
    DD_TEXT_HERO,     /* 330 px: speed */
    DD_TEXT_COUNT,
} dd_text_t;

/* Loads the fonts. Call once, after lv_init() and before creating any component. */
void dd_theme_init(void);

const lv_font_t *dd_font(dd_text_t text);

/* Height of capital letters and digits in pixels, used to line up text by eye. */
int32_t dd_cap_height(dd_text_t text);

/* The colour of a level, as 0xRRGGBB. DD_LEVEL_NORMAL returns `normal_color`. */
uint32_t dd_level_hex(dd_level_t level, uint32_t normal_color);

lv_color_t dd_level_color(dd_level_t level, uint32_t normal_color);

/* A full-screen black screen. Load it with lv_screen_load(). */
lv_obj_t *dd_screen_create(void);

/* Draws a coloured frame around the whole screen for warnings and critical alerts.
 * DD_LEVEL_NORMAL and DD_LEVEL_GOOD remove it. */
void dd_screen_set_alert(lv_obj_t *screen, dd_level_t level);

/* Building blocks for components. */

/* An invisible container: no background, padding, scrolling or click handling. */
lv_obj_t *dd_box_create(lv_obj_t *parent);

lv_obj_t *dd_text_create(lv_obj_t *parent, dd_text_t text, uint32_t color);

void dd_text_set_style(lv_obj_t *label, dd_text_t text);

/* Gives a component its own state, freed when the component is deleted. */
void *dd_state_create(lv_obj_t *obj, size_t size);

#define DD_STATE(obj, type) ((type *)lv_obj_get_user_data(obj))

#endif /* DD_THEME_H */
