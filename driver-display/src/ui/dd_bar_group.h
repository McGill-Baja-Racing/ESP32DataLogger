#ifndef DD_BAR_GROUP_H
#define DD_BAR_GROUP_H

#include "dd_theme.h"

/* Vertical fill bars side by side, with a title above and a caption under each bar.
 * Used for front and rear brake pressure. A bar at warning or critical level turns
 * amber or red, and so does its caption. Size it with lv_obj_set_size(). */

lv_obj_t *dd_bar_group_create(lv_obj_t *parent, const char *title, uint32_t count,
                              const char *const captions[]);

/* Width of each bar and space between bars, in pixels. Default: 38 and 8. */
void dd_bar_group_set_bar_width(lv_obj_t *group, int32_t width, int32_t gap);

/* percent: 0 (empty) to 100 (full). */
void dd_bar_group_set_value(lv_obj_t *group, uint32_t index, int32_t percent, dd_level_t level);

/* Empties the bar and writes DD_TEXT_UNAVAILABLE in it, so it cannot be read as zero. */
void dd_bar_group_set_unavailable(lv_obj_t *group, uint32_t index);

#endif /* DD_BAR_GROUP_H */
