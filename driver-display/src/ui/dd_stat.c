#include "dd_stat.h"

typedef struct {
    lv_obj_t *value;
} stat_t;

lv_obj_t *dd_stat_create(lv_obj_t *parent, const char *caption, lv_text_align_t align)
{
    lv_obj_t *obj = dd_box_create(parent);
    stat_t *stat = dd_state_create(obj, sizeof(stat_t));
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_flex_align_t cross = LV_FLEX_ALIGN_START;
    if (align == LV_TEXT_ALIGN_CENTER) {
        cross = LV_FLEX_ALIGN_CENTER;
    } else if (align == LV_TEXT_ALIGN_RIGHT) {
        cross = LV_FLEX_ALIGN_END;
    }
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, cross, cross);
    lv_obj_set_style_pad_row(obj, 17, 0);

    lv_obj_t *caption_label = dd_text_create(obj, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(caption_label, caption);
    stat->value = dd_text_create(obj, DD_TEXT_STAT, DD_COLOR_TEXT);

    dd_stat_set_unavailable(obj);
    return obj;
}

void dd_stat_set_text(lv_obj_t *obj, const char *value, dd_level_t level)
{
    stat_t *stat = DD_STATE(obj, stat_t);
    lv_label_set_text(stat->value, value);
    lv_obj_set_style_text_color(stat->value, dd_level_color(level, DD_COLOR_TEXT), 0);
}

void dd_stat_set_unavailable(lv_obj_t *obj)
{
    stat_t *stat = DD_STATE(obj, stat_t);
    lv_label_set_text(stat->value, DD_TEXT_UNAVAILABLE);
    lv_obj_set_style_text_color(stat->value, lv_color_hex(DD_COLOR_LABEL_DIM), 0);
}
