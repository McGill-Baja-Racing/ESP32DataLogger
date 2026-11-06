#include "driver_view.h"

/* Positions in pixels, measured on the designs. */
#define SIDE_TOP 16
#define BRAKE_HEIGHT 564
#define FUEL_HEIGHT 569
#define CENTER_LEFT 120
#define CENTER_WIDTH 784
#define READOUT_TOP 60
#define SPEED_BASELINE 388
#define BOTTOM_TOP 472
#define BOTTOM_HEIGHT 112
#define STATS_TEXT_TOP 16

static const dd_rpm_bar_config_t rpm_config = {
    .max_rpm = 4000,
    .shift_rpm = 2800,
    .limit_rpm = 3400,
    .segments = 40,
};

static const dd_segment_gauge_config_t fuel_config = {
    .title = "FUEL",
    .segments = 10,
    .warning_below = 25,
    .critical_below = 10,
};

static void create_stats(driver_view_t *view)
{
    view->stats = dd_box_create(view->screen);
    lv_obj_set_pos(view->stats, CENTER_LEFT, BOTTOM_TOP);
    lv_obj_set_size(view->stats, CENTER_WIDTH, BOTTOM_HEIGHT);
    lv_obj_set_style_border_side(view->stats, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(view->stats, lv_color_hex(DD_COLOR_DIVIDER), 0);
    lv_obj_set_style_border_width(view->stats, 1, 0);

    view->lap_time = dd_stat_create(view->stats, "LAP TIME", LV_TEXT_ALIGN_LEFT);
    lv_obj_align(view->lap_time, LV_ALIGN_TOP_LEFT, 0, STATS_TEXT_TOP);
    view->delta = dd_stat_create(view->stats, "DELTA", LV_TEXT_ALIGN_CENTER);
    lv_obj_align(view->delta, LV_ALIGN_TOP_MID, 0, STATS_TEXT_TOP);
    view->last_lap = dd_stat_create(view->stats, "LAST LAP", LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(view->last_lap, LV_ALIGN_TOP_RIGHT, 0, STATS_TEXT_TOP);
}

void driver_view_create(driver_view_t *view)
{
    view->screen = dd_screen_create();

    static const char *const brake_captions[] = {"F", "R"};
    view->brakes = dd_bar_group_create(view->screen, "BRAKE", 2, brake_captions);
    lv_obj_set_pos(view->brakes, 16, SIDE_TOP);
    lv_obj_set_size(view->brakes, 84, BRAKE_HEIGHT);

    view->fuel = dd_segment_gauge_create(view->screen, &fuel_config);
    lv_obj_set_pos(view->fuel, 924, SIDE_TOP);
    lv_obj_set_size(view->fuel, 84, FUEL_HEIGHT);

    view->rpm = dd_rpm_bar_create(view->screen, &rpm_config);
    lv_obj_set_pos(view->rpm, CENTER_LEFT, 16);
    lv_obj_set_size(view->rpm, CENTER_WIDTH, 32);

    view->cvt = dd_readout_create(view->screen, "CVT");
    lv_obj_align(view->cvt, LV_ALIGN_TOP_LEFT, CENTER_LEFT, READOUT_TOP);
    view->lap = dd_readout_create(view->screen, "LAP");
    lv_obj_align(view->lap, LV_ALIGN_TOP_MID, 0, READOUT_TOP);
    view->engine = dd_readout_create(view->screen, "ENGINE");
    lv_obj_align(view->engine, LV_ALIGN_TOP_RIGHT, -(1024 - CENTER_LEFT - CENTER_WIDTH), READOUT_TOP);

    /* Place the speed by the bottom of its digits, as in the design. */
    const lv_font_t *hero = dd_font(DD_TEXT_HERO);
    view->speed = dd_hero_value_create(view->screen, "KM/H");
    lv_obj_align(view->speed, LV_ALIGN_TOP_MID, 0,
                 SPEED_BASELINE - (lv_font_get_line_height(hero) - hero->base_line));

    create_stats(view);

    view->alert = dd_alert_banner_create(view->screen);
    lv_obj_set_pos(view->alert, CENTER_LEFT, BOTTOM_TOP);
    lv_obj_set_size(view->alert, CENTER_WIDTH, BOTTOM_HEIGHT);
}

void driver_view_show_alert(driver_view_t *view, dd_level_t level, const char *title, const char *detail,
                            const char *action)
{
    dd_alert_banner_show(view->alert, level, title, detail, action);
    lv_obj_set_hidden(view->stats, true);
    dd_screen_set_alert(view->screen, level);
}

void driver_view_clear_alert(driver_view_t *view)
{
    dd_alert_banner_hide(view->alert);
    lv_obj_set_hidden(view->stats, false);
    dd_screen_set_alert(view->screen, DD_LEVEL_NORMAL);
}
