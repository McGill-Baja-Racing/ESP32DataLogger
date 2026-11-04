#ifndef LV_CONF_H
#define LV_CONF_H

/* Only the settings we change. LVGL's defaults cover the rest.
 * After changing this file, run "make clean" on the Pi to rebuild LVGL. */

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB

#define LV_USE_LINUX_DRM 1

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_H */
