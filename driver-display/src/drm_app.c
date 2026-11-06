#include "drm_app.h"

#include <stdio.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui/dd_ui.h"

#define DRM_DEVICE "/dev/dri/card0"
/* Tells LVGL to use the first connected screen. */
#define ANY_CONNECTOR -1

bool drm_app_init(void)
{
    lv_init();

    lv_display_t *display = lv_linux_drm_create();
    if (display == NULL || lv_linux_drm_set_file(display, DRM_DEVICE, ANY_CONNECTOR) != LV_RESULT_OK) {
        fprintf(stderr, "Could not open a screen on %s. Is it plugged in and powered?\n"
                        "Check with: cat /sys/class/drm/card0-HDMI-A-1/status\n",
                DRM_DEVICE);
        return false;
    }
    printf("Drawing at %dx%d. Press Ctrl+C to stop.\n",
           (int)lv_display_get_horizontal_resolution(display),
           (int)lv_display_get_vertical_resolution(display));
    fflush(stdout);

    dd_theme_init();
    return true;
}

void drm_app_run(void)
{
    for (;;) {
        usleep(lv_timer_handler() * 1000);
    }
}
