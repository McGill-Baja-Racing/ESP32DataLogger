#ifndef DD_STAT_H
#define DD_STAT_H

#include "dd_theme.h"

/* A caption above a large value, such as LAP TIME over 2:14.6. The level colours the
 * value: green for good, amber for warning, red for critical. It sizes itself to its
 * text; place it with lv_obj_align(). */

lv_obj_t *dd_stat_create(lv_obj_t *parent, const char *caption, lv_text_align_t align);

void dd_stat_set_text(lv_obj_t *stat, const char *value, dd_level_t level);

void dd_stat_set_unavailable(lv_obj_t *stat);

#endif /* DD_STAT_H */
