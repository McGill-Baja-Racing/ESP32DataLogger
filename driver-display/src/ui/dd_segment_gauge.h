#ifndef DD_SEGMENT_GAUGE_H
#define DD_SEGMENT_GAUGE_H

#include "dd_theme.h"

/* Vertical segmented gauge with a title above and the percentage under it. Used for
 * fuel. Below the warning level the lit segments and the percentage turn amber, and
 * below the critical level they turn red. Size it with lv_obj_set_size(). */

typedef struct {
    const char *title;         /* "FUEL" */
    uint32_t segments;         /* 10 in the design */
    int32_t warning_below;     /* percent, for example 25 */
    int32_t critical_below;    /* percent, for example 10 */
} dd_segment_gauge_config_t;

lv_obj_t *dd_segment_gauge_create(lv_obj_t *parent, const dd_segment_gauge_config_t *config);

/* percent: 0 to 100. A segment lights as soon as part of it is filled. */
void dd_segment_gauge_set_percent(lv_obj_t *gauge, int32_t percent);

void dd_segment_gauge_set_unavailable(lv_obj_t *gauge);

#endif /* DD_SEGMENT_GAUGE_H */
