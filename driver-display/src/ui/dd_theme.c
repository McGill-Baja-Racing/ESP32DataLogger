#include "dd_theme.h"

#include "fonts/dd_fonts.h"

/* Archivo capitals are 0.686 of the font size. */
#define CAP_HEIGHT_PER_MILLE 686
#define ALERT_FRAME_WIDTH 6

typedef enum { WEIGHT_SEMIBOLD, WEIGHT_BOLD, WEIGHT_EXTRABOLD } weight_t;

static const struct {
    int32_t size;
    weight_t weight;
    int32_t letter_space;
} text_styles[DD_TEXT_COUNT] = {
    [DD_TEXT_CAPTION] = {14, WEIGHT_BOLD, 2},
    [DD_TEXT_LABEL] = {16, WEIGHT_BOLD, 3},
    [DD_TEXT_BODY] = {18, WEIGHT_BOLD, 1},
    [DD_TEXT_ROW] = {24, WEIGHT_BOLD, 0},
    [DD_TEXT_ACTION] = {28, WEIGHT_EXTRABOLD, 2},
    [DD_TEXT_PERCENT] = {29, WEIGHT_EXTRABOLD, 0},
    [DD_TEXT_TITLE] = {40, WEIGHT_EXTRABOLD, 0},
    [DD_TEXT_VALUE] = {41, WEIGHT_EXTRABOLD, -1},
    [DD_TEXT_STAT] = {64, WEIGHT_EXTRABOLD, 0},
    [DD_TEXT_HERO] = {330, WEIGHT_EXTRABOLD, -10},
};

static lv_font_t *fonts[DD_TEXT_COUNT];

static lv_font_t *load_font(weight_t weight, int32_t size)
{
    const void *data = dd_archivo_bold_ttf;
    size_t length = dd_archivo_bold_ttf_len;

    if (weight == WEIGHT_SEMIBOLD) {
        data = dd_archivo_semibold_ttf;
        length = dd_archivo_semibold_ttf_len;
    } else if (weight == WEIGHT_EXTRABOLD) {
        data = dd_archivo_extrabold_ttf;
        length = dd_archivo_extrabold_ttf_len;
    }
    /* The cache holds rendered glyphs. 64 covers the characters one size uses. */
    return lv_tiny_ttf_create_data_ex(data, length, size, LV_FONT_KERNING_NORMAL, 64);
}

void dd_theme_init(void)
{
    for (int i = 0; i < DD_TEXT_COUNT; i++) {
        if (fonts[i] == NULL) {
            fonts[i] = load_font(text_styles[i].weight, text_styles[i].size);
        }
    }
}

const lv_font_t *dd_font(dd_text_t text)
{
    return fonts[text];
}

int32_t dd_cap_height(dd_text_t text)
{
    return (text_styles[text].size * CAP_HEIGHT_PER_MILLE + 500) / 1000;
}

uint32_t dd_level_hex(dd_level_t level, uint32_t normal_color)
{
    switch (level) {
    case DD_LEVEL_GOOD:
        return DD_COLOR_GOOD;
    case DD_LEVEL_WARNING:
        return DD_COLOR_WARNING;
    case DD_LEVEL_CRITICAL:
        return DD_COLOR_CRITICAL;
    case DD_LEVEL_NORMAL:
    default:
        return normal_color;
    }
}

lv_color_t dd_level_color(dd_level_t level, uint32_t normal_color)
{
    return lv_color_hex(dd_level_hex(level, normal_color));
}

typedef struct {
    dd_level_t alert;
} screen_t;

/* The alert frame is drawn after the children, so nothing hides it. It is not a
 * border style, because LVGL would then move every child inward by its width. */
static void draw_alert_frame(lv_event_t *event)
{
    lv_obj_t *screen = lv_event_get_current_target(event);
    dd_level_t alert = DD_STATE(screen, screen_t)->alert;
    if (alert != DD_LEVEL_WARNING && alert != DD_LEVEL_CRITICAL) {
        return;
    }
    lv_draw_rect_dsc_t frame;
    lv_draw_rect_dsc_init(&frame);
    frame.bg_opa = LV_OPA_TRANSP;
    frame.border_width = ALERT_FRAME_WIDTH;
    frame.border_color = dd_level_color(alert, DD_COLOR_BG);
    lv_area_t area;
    lv_obj_get_coords(screen, &area);
    lv_draw_rect(lv_event_get_layer(event), &frame, &area);
}

lv_obj_t *dd_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(DD_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    dd_state_create(screen, sizeof(screen_t));
    lv_obj_add_event_cb(screen, draw_alert_frame, LV_EVENT_DRAW_POST_END, NULL);
    return screen;
}

void dd_screen_set_alert(lv_obj_t *screen, dd_level_t level)
{
    DD_STATE(screen, screen_t)->alert = level;
    lv_obj_invalidate(screen);
}

lv_obj_t *dd_box_create(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    return box;
}

void dd_text_set_style(lv_obj_t *label, dd_text_t text)
{
    lv_obj_set_style_text_font(label, fonts[text], 0);
    lv_obj_set_style_text_letter_space(label, text_styles[text].letter_space, 0);
}

lv_obj_t *dd_text_create(lv_obj_t *parent, dd_text_t text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    dd_text_set_style(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

static void free_state(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

void *dd_state_create(lv_obj_t *obj, size_t size)
{
    void *state = lv_malloc_zeroed(size);
    LV_ASSERT_MALLOC(state);
    lv_obj_set_user_data(obj, state);
    lv_obj_add_event_cb(obj, free_state, LV_EVENT_DELETE, NULL);
    return state;
}
