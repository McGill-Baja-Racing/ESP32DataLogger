#include "dd_alert_banner.h"

typedef struct {
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *action;
    lv_obj_t *action_text;
} alert_banner_t;

lv_obj_t *dd_alert_banner_create(lv_obj_t *parent)
{
    lv_obj_t *obj = dd_box_create(parent);
    alert_banner_t *banner = dd_state_create(obj, sizeof(alert_banner_t));
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_pad_hor(obj, 28, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 24, 0);

    lv_obj_t *texts = dd_box_create(obj);
    lv_obj_set_size(texts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(texts, 8, 0);
    banner->title = dd_text_create(texts, DD_TEXT_TITLE, DD_COLOR_BG);
    lv_label_set_long_mode(banner->title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(banner->title, lv_pct(100));
    banner->detail = dd_text_create(texts, DD_TEXT_BODY, DD_COLOR_BG);

    banner->action = dd_box_create(obj);
    lv_obj_set_size(banner->action, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_bg_color(banner->action, lv_color_hex(DD_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(banner->action, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner->action, 8, 0);
    lv_obj_set_style_pad_hor(banner->action, 18, 0);
    banner->action_text = dd_text_create(banner->action, DD_TEXT_ACTION, DD_COLOR_CRITICAL);
    lv_obj_center(banner->action_text);

    dd_alert_banner_hide(obj);
    return obj;
}

void dd_alert_banner_show(lv_obj_t *obj, dd_level_t level, const char *title, const char *detail,
                          const char *action)
{
    alert_banner_t *banner = DD_STATE(obj, alert_banner_t);
    lv_color_t color = dd_level_color(level, DD_COLOR_CRITICAL);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_label_set_text(banner->title, title);

    if (detail != NULL) {
        lv_label_set_text(banner->detail, detail);
        lv_obj_set_hidden(banner->detail, false);
    } else {
        lv_obj_set_hidden(banner->detail, true);
    }

    if (action != NULL) {
        lv_label_set_text(banner->action_text, action);
        lv_obj_set_style_text_color(banner->action_text, color, 0);
        lv_obj_set_hidden(banner->action, false);
    } else {
        lv_obj_set_hidden(banner->action, true);
    }
    lv_obj_set_hidden(obj, false);
}

void dd_alert_banner_hide(lv_obj_t *obj)
{
    lv_obj_set_hidden(obj, true);
}
