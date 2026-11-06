#include <stdio.h>
#include <stdlib.h>

#include "drm_app.h"
#include "views/demo_screens.h"

/* The driver display. Until it reads CAN, it shows one of the designs with
 * hardcoded values: design 1 by default, or the number given as argument. */
int main(int argc, char **argv)
{
    int design = argc > 1 ? atoi(argv[1]) : 1;
    if (design < 1 || design > 3) {
        fprintf(stderr, "Usage: %s [1|2|3]   (which design to show, default 1)\n", argv[0]);
        return 2;
    }
    if (!drm_app_init()) {
        return 1;
    }
    lv_screen_load(demo_screen_create((demo_screen_t)(DEMO_DESIGN_1 + design - 1)));
    drm_app_run();
    return 0;
}
