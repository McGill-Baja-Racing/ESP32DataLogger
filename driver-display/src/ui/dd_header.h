#ifndef DD_HEADER_H
#define DD_HEADER_H

#include "dd_theme.h"

/* The title bar of a workshop view: the view name on the left, and on the right the
 * mode and one dot per view with the current one lit. Full width, 72 px high;
 * place it at the top of the screen. */

lv_obj_t *dd_header_create(lv_obj_t *parent, const char *title, const char *mode);

/* index counts from 0. */
void dd_header_set_page(lv_obj_t *header, uint32_t index, uint32_t count);

#endif /* DD_HEADER_H */
