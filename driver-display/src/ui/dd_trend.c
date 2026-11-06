#include "dd_trend.h"

typedef struct {
    dd_trend_config_t config;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *value;
    lv_obj_t *threshold_line;
    lv_point_precise_t threshold_points[2];
} trend_t;

static lv_obj_t *scale_label(lv_obj_t *parent, int32_t value, lv_align_t align)
{
    lv_obj_t *label = dd_text_create(parent, DD_TEXT_CAPTION, DD_COLOR_LABEL_DIM);
    lv_label_set_text_fmt(label, "%d", (int)value);
    lv_obj_align(label, align, 0, 0);
    return label;
}

/* The threshold line depends on the graph's height, so place it once it is known. */
static void place_threshold(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_user_data(event);
    trend_t *trend = DD_STATE(obj, trend_t);
    int32_t range = trend->config.max - trend->config.min;
    if (trend->threshold_line == NULL || range <= 0) {
        return;
    }
    int32_t width = lv_obj_get_content_width(trend->chart);
    int32_t height = lv_obj_get_content_height(trend->chart);
    int32_t y = (int32_t)((int64_t)(trend->config.max - trend->config.threshold) * height / range);
    trend->threshold_points[0] = (lv_point_precise_t){0, y};
    trend->threshold_points[1] = (lv_point_precise_t){width, y};
    lv_line_set_points(trend->threshold_line, trend->threshold_points, 2);
}

lv_obj_t *dd_trend_create(lv_obj_t *parent, const dd_trend_config_t *config)
{
    lv_obj_t *obj = dd_box_create(parent);
    trend_t *trend = dd_state_create(obj, sizeof(trend_t));
    trend->config = *config;
    lv_obj_set_style_border_color(obj, lv_color_hex(DD_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_pad_all(obj, 18, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj, 10, 0);

    /* Caption on the left, current value and unit on the right. */
    lv_obj_t *header = dd_box_create(obj);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(header, 8, 0);
    lv_obj_t *caption = dd_text_create(header, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(caption, config->caption);
    lv_obj_set_flex_grow(caption, 1);
    trend->value = dd_text_create(header, DD_TEXT_VALUE, DD_COLOR_TEXT);
    lv_obj_t *unit = dd_text_create(header, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(unit, config->unit != NULL ? config->unit : "");
    int32_t shift = dd_font(DD_TEXT_CAPTION)->base_line - dd_font(DD_TEXT_VALUE)->base_line;
    lv_obj_set_style_translate_y(caption, shift, 0);
    lv_obj_set_style_translate_y(unit, shift, 0);

    /* The graph, with the range written at its top and bottom left. */
    lv_obj_t *area = dd_box_create(obj);
    lv_obj_set_width(area, lv_pct(100));
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_style_pad_left(area, 48, 0);

    trend->chart = lv_chart_create(area);
    lv_obj_remove_style_all(trend->chart);
    lv_obj_set_size(trend->chart, lv_pct(100), lv_pct(100));
    lv_chart_set_type(trend->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(trend->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(trend->chart, config->points);
    lv_chart_set_axis_range(trend->chart, LV_CHART_AXIS_PRIMARY_Y, config->min, config->max);
    lv_chart_set_div_line_count(trend->chart, 3, 0);
    lv_obj_set_style_line_color(trend->chart, lv_color_hex(DD_COLOR_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_line_width(trend->chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(trend->chart, 4, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(trend->chart, true, LV_PART_ITEMS);
    lv_obj_set_style_size(trend->chart, 0, 0, LV_PART_INDICATOR);
    trend->series = lv_chart_add_series(trend->chart, lv_color_hex(DD_COLOR_TEXT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(trend->chart, trend->series, LV_CHART_POINT_NONE);

    lv_obj_t *top = scale_label(area, config->max, LV_ALIGN_TOP_LEFT);
    lv_obj_t *bottom = scale_label(area, config->min, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_x(top, -48);
    lv_obj_set_x(bottom, -48);

    if (config->has_threshold) {
        trend->threshold_line = lv_line_create(trend->chart);
        lv_obj_remove_style_all(trend->threshold_line);
        lv_obj_set_style_line_color(trend->threshold_line, lv_color_hex(DD_COLOR_WARNING), 0);
        lv_obj_set_style_line_width(trend->threshold_line, 2, 0);
        lv_obj_set_style_line_dash_width(trend->threshold_line, 10, 0);
        lv_obj_set_style_line_dash_gap(trend->threshold_line, 8, 0);
        lv_obj_add_event_cb(trend->chart, place_threshold, LV_EVENT_SIZE_CHANGED, obj);
    }

    lv_label_set_text(trend->value, DD_TEXT_UNAVAILABLE);
    lv_obj_set_style_text_color(trend->value, lv_color_hex(DD_COLOR_LABEL_DIM), 0);
    return obj;
}

void dd_trend_push(lv_obj_t *obj, int32_t value, dd_level_t level)
{
    trend_t *trend = DD_STATE(obj, trend_t);
    lv_chart_set_next_value(trend->chart, trend->series, value);
    lv_label_set_text_fmt(trend->value, "%d", (int)value);
    lv_obj_set_style_text_color(trend->value, dd_level_color(level, DD_COLOR_TEXT), 0);
}

void dd_trend_push_unavailable(lv_obj_t *obj)
{
    trend_t *trend = DD_STATE(obj, trend_t);
    lv_chart_set_next_value(trend->chart, trend->series, LV_CHART_POINT_NONE);
    lv_label_set_text(trend->value, DD_TEXT_UNAVAILABLE);
    lv_obj_set_style_text_color(trend->value, lv_color_hex(DD_COLOR_LABEL_DIM), 0);
}
