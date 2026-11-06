#ifndef LV_CONF_H
#define LV_CONF_H

/* Only the settings we change. LVGL's defaults cover the rest.
 * Changing this file makes the next build recompile all of LVGL. */

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* The Pi's screen. Other systems only render to memory (snapshot). */
#ifdef __linux__
#define LV_USE_LINUX_DRM 1
#endif

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* The component fonts are TrueType, rendered at any size. */
#define LV_USE_TINY_TTF 1
/* Built-in font of display_poc.c. */
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_H */
