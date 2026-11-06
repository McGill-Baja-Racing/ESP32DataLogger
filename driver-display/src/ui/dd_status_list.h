#ifndef DD_STATUS_LIST_H
#define DD_STATUS_LIST_H

#include "dd_theme.h"

/* A list of named items, each with a detail and a status badge, for the CAN and
 * data health view. Badge colours: green (good), amber (warning), red (critical),
 * grey (normal, used for "no data"). Size it with lv_obj_set_size(). */

lv_obj_t *dd_status_list_create(lv_obj_t *parent);

/* Adds a row and returns its index. `name` is the main text, `id` a dim tag after it
 * such as "0x0B1" (can be NULL). The row starts as unavailable. */
uint32_t dd_status_list_add(lv_obj_t *list, const char *name, const char *id);

void dd_status_list_set(lv_obj_t *list, uint32_t row, dd_level_t level, const char *status,
                        const char *detail);

#endif /* DD_STATUS_LIST_H */
