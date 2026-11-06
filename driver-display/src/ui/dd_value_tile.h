#ifndef DD_VALUE_TILE_H
#define DD_VALUE_TILE_H

#include "dd_theme.h"

/* A framed tile with a caption and unit, a large value and a note line, for the
 * workshop live values view. At warning or critical level the frame and the value
 * turn amber or red. Size it with lv_obj_set_size(), or let a grid size it. */

lv_obj_t *dd_value_tile_create(lv_obj_t *parent, const char *caption, const char *unit);

void dd_value_tile_set_text(lv_obj_t *tile, const char *value, dd_level_t level);

void dd_value_tile_set_int(lv_obj_t *tile, int32_t value, dd_level_t level);

/* Small text at the bottom, such as the CAN ID or the value's range. NULL hides it. */
void dd_value_tile_set_note(lv_obj_t *tile, const char *note);

void dd_value_tile_set_unavailable(lv_obj_t *tile);

#endif /* DD_VALUE_TILE_H */
