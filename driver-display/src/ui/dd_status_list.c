#include "dd_status_list.h"

#define BADGE_WIDTH 136
#define DETAIL_WIDTH 300

/* Children of each row, in creation order. */
enum { ROW_NAME, ROW_ID, ROW_DETAIL, ROW_BADGE };

lv_obj_t *dd_status_list_create(lv_obj_t *parent)
{
    lv_obj_t *obj = dd_box_create(parent);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    return obj;
}

uint32_t dd_status_list_add(lv_obj_t *list, const char *name, const char *id)
{
    lv_obj_t *row = dd_box_create(list);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(DD_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(row, 1, 0);

    lv_obj_t *name_label = dd_text_create(row, DD_TEXT_ROW, DD_COLOR_TEXT);
    lv_label_set_text(name_label, name);

    lv_obj_t *id_label = dd_text_create(row, DD_TEXT_BODY, DD_COLOR_LABEL_DIM);
    lv_label_set_text(id_label, id != NULL ? id : "");
    lv_obj_set_flex_grow(id_label, 1);

    lv_obj_t *detail = dd_text_create(row, DD_TEXT_BODY, DD_COLOR_TEXT_DIM);
    lv_obj_set_width(detail, DETAIL_WIDTH);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *badge = dd_box_create(row);
    lv_obj_set_size(badge, BADGE_WIDTH, 40);
    lv_obj_set_style_radius(badge, 8, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_t *badge_text = dd_text_create(badge, DD_TEXT_LABEL, DD_COLOR_BG);
    lv_obj_center(badge_text);

    uint32_t index = lv_obj_get_child_count(list) - 1;
    dd_status_list_set(list, index, DD_LEVEL_NORMAL, "NO DATA", DD_TEXT_UNAVAILABLE);
    return index;
}

void dd_status_list_set(lv_obj_t *list, uint32_t index, dd_level_t level, const char *status,
                        const char *detail)
{
    lv_obj_t *row = lv_obj_get_child(list, (int32_t)index);
    if (row == NULL) {
        return;
    }
    lv_label_set_text(lv_obj_get_child(row, ROW_DETAIL), detail != NULL ? detail : "");

    /* Grey badges stay dark with grey text, so they do not look like an alert. */
    bool grey = level == DD_LEVEL_NORMAL;
    lv_obj_t *badge = lv_obj_get_child(row, ROW_BADGE);
    lv_obj_set_style_bg_color(badge, grey ? lv_color_hex(DD_COLOR_TRACK) : dd_level_color(level, 0), 0);
    lv_obj_t *badge_text = lv_obj_get_child(badge, 0);
    lv_label_set_text(badge_text, status);
    lv_obj_set_style_text_color(badge_text, lv_color_hex(grey ? DD_COLOR_LABEL : DD_COLOR_BG), 0);

    lv_obj_set_style_text_color(lv_obj_get_child(row, ROW_NAME),
                                lv_color_hex(grey ? DD_COLOR_TEXT_DIM : DD_COLOR_TEXT), 0);
}
