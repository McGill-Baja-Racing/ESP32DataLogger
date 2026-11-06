#include "dd_segment_gauge.h"

#include "dd_segment_bar.h"

typedef struct {
    dd_segment_gauge_config_t config;
    lv_obj_t *segments;
    lv_obj_t *value;
} segment_gauge_t;

lv_obj_t *dd_segment_gauge_create(lv_obj_t *parent, const dd_segment_gauge_config_t *config)
{
    lv_obj_t *obj = dd_box_create(parent);
    segment_gauge_t *gauge = dd_state_create(obj, sizeof(segment_gauge_t));
    gauge->config = *config;
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(obj, 11, 0);

    lv_obj_t *title = dd_text_create(obj, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(title, config->title);

    gauge->segments = dd_segment_bar_create(obj, config->segments, DD_SEGMENT_BAR_VERTICAL);
    lv_obj_set_width(gauge->segments, 38);
    lv_obj_set_flex_grow(gauge->segments, 1);
    dd_segment_bar_set_shape(gauge->segments, 5, 4);

    gauge->value = dd_text_create(obj, DD_TEXT_PERCENT, DD_COLOR_TEXT);
    lv_obj_set_style_translate_y(gauge->value, 2, 0);
    dd_segment_gauge_set_unavailable(obj);
    return obj;
}

void dd_segment_gauge_set_percent(lv_obj_t *obj, int32_t percent)
{
    segment_gauge_t *gauge = DD_STATE(obj, segment_gauge_t);
    percent = LV_CLAMP(0, percent, 100);

    dd_level_t level = DD_LEVEL_NORMAL;
    if (percent < gauge->config.critical_below) {
        level = DD_LEVEL_CRITICAL;
    } else if (percent < gauge->config.warning_below) {
        level = DD_LEVEL_WARNING;
    }
    uint32_t color = dd_level_hex(level, DD_COLOR_TEXT);

    uint32_t lit = (uint32_t)(percent * (int32_t)gauge->config.segments + 99) / 100;
    dd_segment_bar_set_lit(gauge->segments, lit, color);
    lv_label_set_text_fmt(gauge->value, "%d%%", (int)percent);
    lv_obj_set_style_text_color(gauge->value, lv_color_hex(color), 0);
}

void dd_segment_gauge_set_unavailable(lv_obj_t *obj)
{
    segment_gauge_t *gauge = DD_STATE(obj, segment_gauge_t);
    dd_segment_bar_set_lit(gauge->segments, 0, DD_COLOR_TEXT);
    lv_label_set_text(gauge->value, DD_TEXT_UNAVAILABLE);
    lv_obj_set_style_text_color(gauge->value, lv_color_hex(DD_COLOR_LABEL_DIM), 0);
}
