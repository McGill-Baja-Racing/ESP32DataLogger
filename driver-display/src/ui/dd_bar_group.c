#include "dd_bar_group.h"

#define MAX_BARS 8

typedef struct {
    uint32_t count;
    lv_obj_t *bars_row;
    lv_obj_t *captions_row;
    lv_obj_t *tracks[MAX_BARS];
    lv_obj_t *fills[MAX_BARS];
    lv_obj_t *missing[MAX_BARS];
    lv_obj_t *captions[MAX_BARS];
} bar_group_t;

static lv_obj_t *create_row(lv_obj_t *parent)
{
    lv_obj_t *row = dd_box_create(parent);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    return row;
}

lv_obj_t *dd_bar_group_create(lv_obj_t *parent, const char *title, uint32_t count,
                              const char *const captions[])
{
    lv_obj_t *obj = dd_box_create(parent);
    bar_group_t *group = dd_state_create(obj, sizeof(bar_group_t));
    group->count = LV_MIN(count, MAX_BARS);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(obj, 11, 0);

    lv_obj_t *heading = dd_text_create(obj, DD_TEXT_CAPTION, DD_COLOR_LABEL);
    lv_label_set_text(heading, title);

    group->bars_row = create_row(obj);
    lv_obj_set_flex_grow(group->bars_row, 1);
    group->captions_row = create_row(obj);
    lv_obj_set_height(group->captions_row, LV_SIZE_CONTENT);
    lv_obj_set_style_translate_y(group->captions_row, 1, 0);

    for (uint32_t i = 0; i < group->count; i++) {
        lv_obj_t *track = dd_box_create(group->bars_row);
        lv_obj_set_height(track, lv_pct(100));
        lv_obj_set_style_bg_color(track, lv_color_hex(DD_COLOR_TRACK), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(track, 4, 0);
        lv_obj_set_style_clip_corner(track, true, 0);
        group->tracks[i] = track;

        lv_obj_t *fill = dd_box_create(track);
        lv_obj_set_width(fill, lv_pct(100));
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_align(fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        group->fills[i] = fill;

        group->missing[i] = dd_text_create(track, DD_TEXT_CAPTION, DD_COLOR_LABEL_DIM);
        lv_label_set_text(group->missing[i], DD_TEXT_UNAVAILABLE);
        lv_obj_center(group->missing[i]);

        group->captions[i] = dd_text_create(group->captions_row, DD_TEXT_CAPTION, DD_COLOR_LABEL);
        lv_label_set_text(group->captions[i], captions[i]);
        lv_obj_set_style_text_align(group->captions[i], LV_TEXT_ALIGN_CENTER, 0);

        dd_bar_group_set_value(obj, i, 0, DD_LEVEL_NORMAL);
    }
    dd_bar_group_set_bar_width(obj, 38, 8);
    return obj;
}

void dd_bar_group_set_bar_width(lv_obj_t *obj, int32_t width, int32_t gap)
{
    bar_group_t *group = DD_STATE(obj, bar_group_t);
    lv_obj_set_style_pad_column(group->bars_row, gap, 0);
    lv_obj_set_style_pad_column(group->captions_row, gap, 0);
    for (uint32_t i = 0; i < group->count; i++) {
        lv_obj_set_width(group->tracks[i], width);
        lv_obj_set_width(group->captions[i], width);
    }
}

void dd_bar_group_set_value(lv_obj_t *obj, uint32_t index, int32_t percent, dd_level_t level)
{
    bar_group_t *group = DD_STATE(obj, bar_group_t);
    if (index >= group->count) {
        return;
    }
    lv_color_t color = dd_level_color(level, DD_COLOR_TEXT);
    lv_obj_set_height(group->fills[index], lv_pct(LV_CLAMP(0, percent, 100)));
    lv_obj_set_style_bg_color(group->fills[index], color, 0);
    lv_obj_set_hidden(group->fills[index], false);
    lv_obj_set_hidden(group->missing[index], true);
    lv_obj_set_style_text_color(group->captions[index], dd_level_color(level, DD_COLOR_LABEL), 0);
}

void dd_bar_group_set_unavailable(lv_obj_t *obj, uint32_t index)
{
    bar_group_t *group = DD_STATE(obj, bar_group_t);
    if (index >= group->count) {
        return;
    }
    lv_obj_set_hidden(group->fills[index], true);
    lv_obj_set_hidden(group->missing[index], false);
    lv_obj_set_style_text_color(group->captions[index], lv_color_hex(DD_COLOR_LABEL_DIM), 0);
}
