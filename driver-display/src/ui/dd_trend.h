#ifndef DD_TREND_H
#define DD_TREND_H

#include "dd_theme.h"

/* A framed line graph of a value's recent history, with the current value in the
 * corner and an optional dashed threshold line. Each new point shifts the graph
 * left. Missing points leave a gap. Size it with lv_obj_set_size(). */

typedef struct {
    const char *caption;  /* "FRONT BRAKE" */
    const char *unit;     /* "PSI" */
    int32_t min;          /* bottom of the graph */
    int32_t max;          /* top of the graph */
    uint32_t points;      /* how many values fit across */
    bool has_threshold;
    int32_t threshold;    /* drawn as a dashed amber line */
} dd_trend_config_t;

lv_obj_t *dd_trend_create(lv_obj_t *parent, const dd_trend_config_t *config);

/* Adds a value at the right and updates the current value. The level colours the
 * current value; the line stays white so its history stays readable. */
void dd_trend_push(lv_obj_t *trend, int32_t value, dd_level_t level);

/* Adds a gap and shows the current value as unavailable. */
void dd_trend_push_unavailable(lv_obj_t *trend);

#endif /* DD_TREND_H */
