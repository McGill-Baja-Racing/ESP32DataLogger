#include <stdio.h>
#include <stdlib.h>

#include "drm_app.h"
#include "views/demo_screens.h"

/* Shows every demo screen in turn on the Pi's screen, to check the components on
 * the real panel. Optional argument: seconds per screen (default 4). */

static lv_obj_t *screens[DEMO_SCREEN_COUNT];
static uint32_t current;

static void next_screen(lv_timer_t *timer)
{
    (void)timer;
    current = (current + 1) % DEMO_SCREEN_COUNT;
    lv_screen_load(screens[current]);
    printf("%s\n", demo_screen_name((demo_screen_t)current));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 4;
    if (!drm_app_init()) {
        return 1;
    }
    for (uint32_t i = 0; i < DEMO_SCREEN_COUNT; i++) {
        screens[i] = demo_screen_create((demo_screen_t)i);
    }
    current = DEMO_SCREEN_COUNT - 1;
    next_screen(NULL);
    lv_timer_create(next_screen, (uint32_t)(seconds > 0 ? seconds : 4) * 1000, NULL);
    drm_app_run();
    return 0;
}
