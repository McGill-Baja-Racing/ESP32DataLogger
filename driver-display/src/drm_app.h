#ifndef DRM_APP_H
#define DRM_APP_H

#include <stdbool.h>

/* Starts LVGL on the Pi's screen through Linux DRM and loads the component fonts.
 * Prints the reason and returns false if no screen is connected. */
bool drm_app_init(void);

/* Runs LVGL until the program is stopped. */
void drm_app_run(void);

#endif /* DRM_APP_H */
