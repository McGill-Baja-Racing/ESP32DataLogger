#ifndef DEMO_SCREENS_H
#define DEMO_SCREENS_H

#include "lvgl.h"

/* Screens filled with hardcoded values, until the display reads CAN.
 * The three designs, the driver view without data, and the workshop views. */

typedef enum {
    DEMO_DESIGN_1,      /* front brake alert, PIT NOW */
    DEMO_DESIGN_2,      /* RPM at the limit, slower lap */
    DEMO_DESIGN_3,      /* RPM in the shift window, faster lap */
    DEMO_NO_DATA,       /* driver view with every value unavailable */
    DEMO_WORKSHOP_LIVE,
    DEMO_WORKSHOP_TRENDS,
    DEMO_WORKSHOP_CAN,
    DEMO_SCREEN_COUNT,
} demo_screen_t;

/* Short name, used for file names: "design-1", "workshop-can"... */
const char *demo_screen_name(demo_screen_t screen);

/* Builds the screen. Load it with lv_screen_load(). */
lv_obj_t *demo_screen_create(demo_screen_t screen);

#endif /* DEMO_SCREENS_H */
