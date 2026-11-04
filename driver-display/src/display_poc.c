#include <stdio.h>
#include <unistd.h>

#include "lvgl.h"

#define DRM_DEVICE "/dev/dri/card0"
/* Tells LVGL to use the first connected screen. */
#define ANY_CONNECTOR -1

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    /* The border sits on the outermost pixels. If all four sides are visible,
     * the picture fits the panel with nothing cut off. */
    lv_obj_set_style_border_color(screen, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(screen, 4, 0);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "BAJA 2026");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_center(label);
}

int main(void)
{
    lv_init();

    lv_display_t *display = lv_linux_drm_create();
    if (display == NULL) {
        fprintf(stderr, "Could not create the LVGL display.\n");
        return 1;
    }

    if (lv_linux_drm_set_file(display, DRM_DEVICE, ANY_CONNECTOR) != LV_RESULT_OK) {
        fprintf(stderr, "Could not open a screen on %s. Is it plugged in and powered?\n"
                        "Check with: cat /sys/class/drm/card0-HDMI-A-1/status\n",
                DRM_DEVICE);
        return 1;
    }

    create_ui();

    printf("Drawing at %dx%d. Press Ctrl+C to stop.\n",
           (int)lv_display_get_horizontal_resolution(display),
           (int)lv_display_get_vertical_resolution(display));

    for (;;) {
        usleep(lv_timer_handler() * 1000);
    }
}
