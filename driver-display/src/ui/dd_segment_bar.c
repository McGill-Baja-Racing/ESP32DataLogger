#include "dd_segment_bar.h"

typedef struct {
    uint32_t count;
    dd_segment_bar_direction_t direction;
    int32_t gap;
    uint32_t lit;
    uint32_t lit_color;
    uint32_t *unlit_colors;
} segment_bar_t;

static void free_colors(lv_event_t *event)
{
    segment_bar_t *bar = DD_STATE(lv_event_get_current_target(event), segment_bar_t);
    if (bar != NULL) {
        lv_free(bar->unlit_colors);
    }
}

static void paint(lv_obj_t *obj)
{
    segment_bar_t *bar = DD_STATE(obj, segment_bar_t);
    for (uint32_t i = 0; i < bar->count; i++) {
        uint32_t color = i < bar->lit ? bar->lit_color : bar->unlit_colors[i];
        lv_obj_set_style_bg_color(lv_obj_get_child(obj, (int32_t)i), lv_color_hex(color), 0);
    }
}

/* Places the segments by hand: flex layout rounds every segment up, and over 40
 * segments that adds up to several pixels. Segment i starts at i * pitch. */
static void layout(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target(event);
    segment_bar_t *bar = DD_STATE(obj, segment_bar_t);
    bool horizontal = bar->direction == DD_SEGMENT_BAR_HORIZONTAL;
    int32_t length = horizontal ? lv_obj_get_content_width(obj) : lv_obj_get_content_height(obj);
    int32_t thickness = horizontal ? lv_obj_get_content_height(obj) : lv_obj_get_content_width(obj);
    int32_t count = (int32_t)bar->count;
    if (count == 0 || length < count * (bar->gap + 1)) {
        return; /* not sized yet */
    }

    for (int32_t i = 0; i < count; i++) {
        int32_t start = (i * (length + bar->gap) + count / 2) / count;
        int32_t end = ((i + 1) * (length + bar->gap) + count / 2) / count - bar->gap;
        lv_obj_t *segment = lv_obj_get_child(obj, i);
        if (horizontal) {
            lv_obj_set_pos(segment, start, 0);
            lv_obj_set_size(segment, end - start, thickness);
        } else {
            /* Segment 0 at the bottom. */
            lv_obj_set_pos(segment, 0, length - end);
            lv_obj_set_size(segment, thickness, end - start);
        }
    }
}

lv_obj_t *dd_segment_bar_create(lv_obj_t *parent, uint32_t count, dd_segment_bar_direction_t direction)
{
    lv_obj_t *obj = dd_box_create(parent);
    /* Registered before the state, so it runs before the state is freed. */
    lv_obj_add_event_cb(obj, free_colors, LV_EVENT_DELETE, NULL);
    segment_bar_t *bar = dd_state_create(obj, sizeof(segment_bar_t));
    bar->count = count;
    bar->direction = direction;
    bar->lit_color = DD_COLOR_TEXT;
    bar->unlit_colors = lv_malloc(count * sizeof(uint32_t));
    LV_ASSERT_MALLOC(bar->unlit_colors);

    for (uint32_t i = 0; i < count; i++) {
        bar->unlit_colors[i] = DD_COLOR_TRACK;
        lv_obj_t *segment = dd_box_create(obj);
        lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(obj, layout, LV_EVENT_SIZE_CHANGED, NULL);
    dd_segment_bar_set_shape(obj, 4, 4);
    paint(obj);
    return obj;
}

void dd_segment_bar_set_shape(lv_obj_t *obj, int32_t gap, int32_t radius)
{
    DD_STATE(obj, segment_bar_t)->gap = gap;
    lv_obj_send_event(obj, LV_EVENT_SIZE_CHANGED, NULL);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_set_style_radius(lv_obj_get_child(obj, (int32_t)i), radius, 0);
    }
}

void dd_segment_bar_set_lit(lv_obj_t *obj, uint32_t lit, uint32_t color)
{
    segment_bar_t *bar = DD_STATE(obj, segment_bar_t);
    bar->lit = LV_MIN(lit, bar->count);
    bar->lit_color = color;
    paint(obj);
}

void dd_segment_bar_set_zone(lv_obj_t *obj, uint32_t first, uint32_t last, uint32_t unlit_color)
{
    segment_bar_t *bar = DD_STATE(obj, segment_bar_t);
    for (uint32_t i = first; i <= last && i < bar->count; i++) {
        bar->unlit_colors[i] = unlit_color;
    }
    paint(obj);
}
