#ifndef DD_HERO_VALUE_H
#define DD_HERO_VALUE_H

#include "dd_theme.h"

/* The main value of the driver view, very large, with its unit under it. Used for
 * speed. It sizes itself to its text; centre it with lv_obj_align(). */

lv_obj_t *dd_hero_value_create(lv_obj_t *parent, const char *unit);

void dd_hero_value_set_int(lv_obj_t *hero, int32_t value);

void dd_hero_value_set_unavailable(lv_obj_t *hero);

#endif /* DD_HERO_VALUE_H */
