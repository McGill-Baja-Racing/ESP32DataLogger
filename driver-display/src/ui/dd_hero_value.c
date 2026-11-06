#include "dd_hero_value.h"

/* Space between the bottom of the digits and the top of the unit, from the design. */
#define UNIT_GAP 38

typedef struct {
    lv_obj_t *number;
} hero_value_t;

lv_obj_t *dd_hero_value_create(lv_obj_t *parent, const char *unit)
{
    lv_obj_t *obj = dd_box_create(parent);
    hero_value_t *hero = dd_state_create(obj, sizeof(hero_value_t));
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    hero->number = dd_text_create(obj, DD_TEXT_HERO, DD_COLOR_TEXT);
    lv_obj_t *unit_label = dd_text_create(obj, DD_TEXT_LABEL, DD_COLOR_LABEL);
    lv_label_set_text(unit_label, unit);

    /* A text box reaches below the digits and above the capitals, so pull the unit
     * up to keep UNIT_GAP between what you actually see. */
    const lv_font_t *unit_font = dd_font(DD_TEXT_LABEL);
    int32_t unit_space_above = lv_font_get_line_height(unit_font) - unit_font->base_line
                               - dd_cap_height(DD_TEXT_LABEL);
    lv_obj_set_style_pad_row(obj, UNIT_GAP - dd_font(DD_TEXT_HERO)->base_line - unit_space_above, 0);

    dd_hero_value_set_unavailable(obj);
    return obj;
}

void dd_hero_value_set_int(lv_obj_t *obj, int32_t value)
{
    hero_value_t *hero = DD_STATE(obj, hero_value_t);
    lv_label_set_text_fmt(hero->number, "%d", (int)value);
    lv_obj_set_style_text_color(hero->number, lv_color_hex(DD_COLOR_TEXT), 0);
}

void dd_hero_value_set_unavailable(lv_obj_t *obj)
{
    hero_value_t *hero = DD_STATE(obj, hero_value_t);
    lv_label_set_text(hero->number, DD_TEXT_UNAVAILABLE);
    lv_obj_set_style_text_color(hero->number, lv_color_hex(DD_COLOR_LABEL_DIM), 0);
}
