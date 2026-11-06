#include "dd_value_tile.h"

typedef struct {
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *note;
} value_tile_t;

static void paint(lv_obj_t *obj, dd_level_t level, bool available)
{
    value_tile_t *tile = DD_STATE(obj, value_tile_t);
    bool alert = level == DD_LEVEL_WARNING || level == DD_LEVEL_CRITICAL;
    lv_obj_set_style_border_color(obj, dd_level_color(level, DD_COLOR_DIVIDER), 0);
    int32_t border = alert ? 4 : 2;
    lv_obj_set_style_border_width(obj, border, 0);
    /* LVGL counts the border as padding, so keep the content still. */
    lv_obj_set_style_pad_all(obj, 20 - border, 0);
    lv_obj_set_style_text_color(tile->value,
                                available ? dd_level_color(level, DD_COLOR_TEXT) : lv_color_hex(DD_COLOR_LABEL_DIM),
                                0);
}

lv_obj_t *dd_value_tile_create(lv_obj_t *parent, const char *caption, const char *unit)
{
    lv_obj_t *obj = dd_box_create(parent);
    value_tile_t *tile = dd_state_create(obj, sizeof(value_tile_t));
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj, 6, 0);

    /* Caption on the left, unit on the right, so the value gets the full width. */
    lv_obj_t *top = dd_box_create(obj);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *caption_label = dd_text_create(top, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(caption_label, caption);
    tile->unit = dd_text_create(top, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(tile->unit, unit != NULL ? unit : "");

    /* Pushes the value to the bottom of the tile. */
    lv_obj_t *space = dd_box_create(obj);
    lv_obj_set_flex_grow(space, 1);
    tile->value = dd_text_create(obj, DD_TEXT_STAT, DD_COLOR_TEXT);

    tile->note = dd_text_create(obj, DD_TEXT_BODY, DD_COLOR_LABEL_DIM);
    lv_obj_set_hidden(tile->note, true);

    dd_value_tile_set_unavailable(obj);
    return obj;
}

void dd_value_tile_set_text(lv_obj_t *obj, const char *value, dd_level_t level)
{
    lv_label_set_text(DD_STATE(obj, value_tile_t)->value, value);
    paint(obj, level, true);
}

void dd_value_tile_set_int(lv_obj_t *obj, int32_t value, dd_level_t level)
{
    lv_label_set_text_fmt(DD_STATE(obj, value_tile_t)->value, "%d", (int)value);
    paint(obj, level, true);
}

void dd_value_tile_set_note(lv_obj_t *obj, const char *note)
{
    value_tile_t *tile = DD_STATE(obj, value_tile_t);
    if (note == NULL) {
        lv_obj_set_hidden(tile->note, true);
        return;
    }
    lv_label_set_text(tile->note, note);
    lv_obj_set_hidden(tile->note, false);
}

void dd_value_tile_set_unavailable(lv_obj_t *obj)
{
    lv_label_set_text(DD_STATE(obj, value_tile_t)->value, DD_TEXT_UNAVAILABLE);
    paint(obj, DD_LEVEL_NORMAL, false);
}
