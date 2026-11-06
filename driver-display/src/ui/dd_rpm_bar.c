#include "dd_rpm_bar.h"

#include "dd_segment_bar.h"

typedef struct {
    dd_rpm_bar_config_t config;
    lv_obj_t *segments;
    lv_obj_t *unavailable;
} rpm_bar_t;

static uint32_t segment_of(const dd_rpm_bar_config_t *config, int32_t rpm)
{
    if (rpm <= 0 || config->max_rpm <= 0) {
        return 0;
    }
    int64_t index = ((int64_t)rpm * config->segments + config->max_rpm / 2) / config->max_rpm;
    return (uint32_t)LV_MIN(index, (int64_t)config->segments);
}

lv_obj_t *dd_rpm_bar_create(lv_obj_t *parent, const dd_rpm_bar_config_t *config)
{
    lv_obj_t *obj = dd_box_create(parent);
    rpm_bar_t *bar = dd_state_create(obj, sizeof(rpm_bar_t));
    bar->config = *config;

    bar->segments = dd_segment_bar_create(obj, config->segments, DD_SEGMENT_BAR_HORIZONTAL);
    lv_obj_set_size(bar->segments, lv_pct(100), lv_pct(100));
    dd_segment_bar_set_shape(bar->segments, 3, 2);
    uint32_t shift = segment_of(config, config->shift_rpm);
    uint32_t limit = segment_of(config, config->limit_rpm);
    if (limit > shift) {
        dd_segment_bar_set_zone(bar->segments, shift, limit - 1, DD_COLOR_GOOD_DIM);
    }

    bar->unavailable = dd_text_create(obj, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(bar->unavailable, "NO RPM DATA");
    lv_obj_set_style_bg_color(bar->unavailable, lv_color_hex(DD_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(bar->unavailable, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar->unavailable, 16, 0);
    lv_obj_center(bar->unavailable);
    lv_obj_set_hidden(bar->unavailable, true);
    return obj;
}

void dd_rpm_bar_set_rpm(lv_obj_t *obj, int32_t rpm)
{
    rpm_bar_t *bar = DD_STATE(obj, rpm_bar_t);
    uint32_t color = DD_COLOR_TEXT;
    if (rpm >= bar->config.limit_rpm) {
        color = DD_COLOR_CRITICAL;
    } else if (rpm >= bar->config.shift_rpm) {
        color = DD_COLOR_GOOD;
    }
    dd_segment_bar_set_lit(bar->segments, segment_of(&bar->config, rpm), color);
    lv_obj_set_hidden(bar->unavailable, true);
}

void dd_rpm_bar_set_unavailable(lv_obj_t *obj)
{
    rpm_bar_t *bar = DD_STATE(obj, rpm_bar_t);
    dd_segment_bar_set_lit(bar->segments, 0, DD_COLOR_TEXT);
    lv_obj_set_hidden(bar->unavailable, false);
}
