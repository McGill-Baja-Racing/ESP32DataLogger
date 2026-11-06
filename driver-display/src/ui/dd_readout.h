#ifndef DD_READOUT_H
#define DD_READOUT_H

#include "dd_theme.h"

/* A secondary value with its caption on the same line, such as "CVT 104°".
 * At warning or critical level it turns into a solid amber or red pill with black
 * text, which stands out without moving anything. It sizes itself to its text;
 * place it with lv_obj_align(). */

lv_obj_t *dd_readout_create(lv_obj_t *parent, const char *caption);

void dd_readout_set_text(lv_obj_t *readout, const char *value);

/* Shows the value followed by `suffix`, for example dd_readout_set_int(r, 104, "°"). */
void dd_readout_set_int(lv_obj_t *readout, int32_t value, const char *suffix);

void dd_readout_set_level(lv_obj_t *readout, dd_level_t level);

void dd_readout_set_unavailable(lv_obj_t *readout);

#endif /* DD_READOUT_H */
