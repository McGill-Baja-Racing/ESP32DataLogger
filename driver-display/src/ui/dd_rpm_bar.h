#ifndef DD_RPM_BAR_H
#define DD_RPM_BAR_H

#include "dd_theme.h"

/* Segmented engine RPM bar. Lit segments are white below the shift window, green
 * inside it and red from the limit. Unlit segments of the shift window are dark
 * green, so the driver sees where it starts. */

typedef struct {
    int32_t max_rpm;    /* RPM of a full bar */
    int32_t shift_rpm;  /* start of the shift window */
    int32_t limit_rpm;  /* end of the shift window, start of red */
    uint32_t segments;  /* 40 in the design */
} dd_rpm_bar_config_t;

lv_obj_t *dd_rpm_bar_create(lv_obj_t *parent, const dd_rpm_bar_config_t *config);

void dd_rpm_bar_set_rpm(lv_obj_t *bar, int32_t rpm);

/* Empties the bar and writes NO RPM DATA over it. */
void dd_rpm_bar_set_unavailable(lv_obj_t *bar);

#endif /* DD_RPM_BAR_H */
