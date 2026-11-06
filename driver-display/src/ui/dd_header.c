#include "dd_header.h"

#define DOT_SIZE 12

typedef struct {
    lv_obj_t *dots;
} header_t;

lv_obj_t *dd_header_create(lv_obj_t *parent, const char *title, const char *mode)
{
    lv_obj_t *obj = dd_box_create(parent);
    header_t *header = dd_state_create(obj, sizeof(header_t));
    lv_obj_set_size(obj, lv_pct(100), 72);
    lv_obj_set_style_pad_hor(obj, 24, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(DD_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 16, 0);

    lv_obj_t *title_label = dd_text_create(obj, DD_TEXT_ACTION, DD_COLOR_TEXT);
    lv_label_set_text(title_label, title);
    lv_obj_set_flex_grow(title_label, 1);

    lv_obj_t *mode_label = dd_text_create(obj, DD_TEXT_LABEL, DD_COLOR_WARNING);
    lv_label_set_text(mode_label, mode);

    header->dots = dd_box_create(obj);
    lv_obj_set_size(header->dots, LV_SIZE_CONTENT, DOT_SIZE);
    lv_obj_set_flex_flow(header->dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header->dots, 8, 0);
    return obj;
}

void dd_header_set_page(lv_obj_t *obj, uint32_t index, uint32_t count)
{
    header_t *header = DD_STATE(obj, header_t);
    lv_obj_clean(header->dots);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *dot = dd_box_create(header->dots);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i == index ? DD_COLOR_TEXT : DD_COLOR_TRACK), 0);
    }
}
