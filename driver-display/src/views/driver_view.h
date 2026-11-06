#ifndef DRIVER_VIEW_H
#define DRIVER_VIEW_H

#include "ui/dd_ui.h"

/* The driver view: the 1024 x 600 layout of the designs, built from the components.
 * Fill it through the component functions, for example
 * dd_hero_value_set_int(view.speed, 38). */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *brakes;    /* dd_bar_group: 0 front, 1 rear */
    lv_obj_t *rpm;       /* dd_rpm_bar */
    lv_obj_t *cvt;       /* dd_readout */
    lv_obj_t *lap;       /* dd_readout */
    lv_obj_t *engine;    /* dd_readout */
    lv_obj_t *speed;     /* dd_hero_value */
    lv_obj_t *lap_time;  /* dd_stat */
    lv_obj_t *delta;     /* dd_stat */
    lv_obj_t *last_lap;  /* dd_stat */
    lv_obj_t *fuel;      /* dd_segment_gauge */
    lv_obj_t *stats;     /* the divider and the three stats */
    lv_obj_t *alert;     /* dd_alert_banner, shown in place of the stats */
} driver_view_t;

enum { DRIVER_VIEW_FRONT, DRIVER_VIEW_REAR };

void driver_view_create(driver_view_t *view);

/* Replaces the lap stats with the banner and frames the screen in the alert colour. */
void driver_view_show_alert(driver_view_t *view, dd_level_t level, const char *title, const char *detail,
                            const char *action);

void driver_view_clear_alert(driver_view_t *view);

#endif /* DRIVER_VIEW_H */
