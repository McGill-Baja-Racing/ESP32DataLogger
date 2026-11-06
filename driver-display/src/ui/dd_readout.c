#include "dd_readout.h"

typedef struct {
    lv_obj_t *caption;
    lv_obj_t *value;
    dd_level_t level;
    bool available;
} readout_t;

static void paint(lv_obj_t *obj)
{
    readout_t *readout = DD_STATE(obj, readout_t);
    bool pill = readout->level == DD_LEVEL_WARNING || readout->level == DD_LEVEL_CRITICAL;

    lv_obj_set_style_bg_opa(obj, pill ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(obj, dd_level_color(readout->level, DD_COLOR_BG), 0);

    uint32_t caption_color = pill ? DD_COLOR_BG : DD_COLOR_LABEL_DIM;
    uint32_t value_color = pill ? DD_COLOR_BG : DD_COLOR_TEXT_DIM;
    if (!readout->available && !pill) {
        value_color = DD_COLOR_LABEL_DIM;
    }
    lv_obj_set_style_text_color(readout->caption, lv_color_hex(caption_color), 0);
    lv_obj_set_style_text_color(readout->value, lv_color_hex(value_color), 0);
}

lv_obj_t *dd_readout_create(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *obj = dd_box_create(parent);
    readout_t *readout = dd_state_create(obj, sizeof(readout_t));
    lv_obj_set_size(obj, LV_SIZE_CONTENT, 56);
    lv_obj_set_style_pad_hor(obj, 16, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 14, 0);

    readout->caption = dd_text_create(obj, DD_TEXT_LABEL, DD_COLOR_LABEL_DIM);
    lv_label_set_text(readout->caption, caption);
    readout->value = dd_text_create(obj, DD_TEXT_VALUE, DD_COLOR_TEXT_DIM);

    /* Both texts are centred in the row. Move the caption down so the bottoms of
     * the letters line up, as in the design. */
    const lv_font_t *caption_font = dd_font(DD_TEXT_LABEL);
    const lv_font_t *value_font = dd_font(DD_TEXT_VALUE);
    int32_t value_baseline = (lv_font_get_line_height(value_font) / 2) - value_font->base_line;
    int32_t caption_baseline = (lv_font_get_line_height(caption_font) / 2) - caption_font->base_line;
    lv_obj_set_style_translate_y(readout->caption, value_baseline - caption_baseline, 0);

    dd_readout_set_unavailable(obj);
    return obj;
}

void dd_readout_set_text(lv_obj_t *obj, const char *value)
{
    readout_t *readout = DD_STATE(obj, readout_t);
    lv_label_set_text(readout->value, value);
    readout->available = true;
    paint(obj);
}

void dd_readout_set_int(lv_obj_t *obj, int32_t value, const char *suffix)
{
    readout_t *readout = DD_STATE(obj, readout_t);
    lv_label_set_text_fmt(readout->value, "%d%s", (int)value, suffix != NULL ? suffix : "");
    readout->available = true;
    paint(obj);
}

void dd_readout_set_level(lv_obj_t *obj, dd_level_t level)
{
    DD_STATE(obj, readout_t)->level = level;
    paint(obj);
}

void dd_readout_set_unavailable(lv_obj_t *obj)
{
    readout_t *readout = DD_STATE(obj, readout_t);
    lv_label_set_text(readout->value, DD_TEXT_UNAVAILABLE);
    readout->available = false;
    readout->level = DD_LEVEL_NORMAL;
    paint(obj);
}
