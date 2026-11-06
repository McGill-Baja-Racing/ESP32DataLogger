#ifndef DD_SEGMENT_BAR_H
#define DD_SEGMENT_BAR_H

#include "dd_theme.h"

/* A row or column of equal segments, lit from the start. The base of the RPM bar
 * and the fuel gauge. A vertical bar lights from the bottom. Size it with
 * lv_obj_set_size(); the segments share the space. */

typedef enum {
    DD_SEGMENT_BAR_HORIZONTAL,
    DD_SEGMENT_BAR_VERTICAL,
} dd_segment_bar_direction_t;

lv_obj_t *dd_segment_bar_create(lv_obj_t *parent, uint32_t count, dd_segment_bar_direction_t direction);

/* Space between segments and their corner radius, in pixels. Default: 4 and 4. */
void dd_segment_bar_set_shape(lv_obj_t *bar, int32_t gap, int32_t radius);

/* Lights the first `lit` segments in `color`. The others take their unlit colour. */
void dd_segment_bar_set_lit(lv_obj_t *bar, uint32_t lit, uint32_t color);

/* Unlit colour for segments first..last, to mark a zone such as the shift window.
 * The default is DD_COLOR_TRACK. */
void dd_segment_bar_set_zone(lv_obj_t *bar, uint32_t first, uint32_t last, uint32_t unlit_color);

#endif /* DD_SEGMENT_BAR_H */
