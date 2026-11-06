#ifndef DD_ALERT_BANNER_H
#define DD_ALERT_BANNER_H

#include "dd_theme.h"

/* A solid red (critical) or amber (warning) banner with a title, one line of detail
 * and an optional instruction such as PIT NOW in a black pill. Hidden until shown.
 * Size and place it with lv_obj_set_size() and lv_obj_set_pos(). */

lv_obj_t *dd_alert_banner_create(lv_obj_t *parent);

/* detail and action can be NULL. */
void dd_alert_banner_show(lv_obj_t *banner, dd_level_t level, const char *title, const char *detail,
                          const char *action);

void dd_alert_banner_hide(lv_obj_t *banner);

#endif /* DD_ALERT_BANNER_H */
