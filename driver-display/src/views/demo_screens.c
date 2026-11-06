#include "demo_screens.h"

#include "driver_view.h"

#define WORKSHOP_VIEWS 3
#define MARGIN 24
#define GAP 16

static const char *const names[DEMO_SCREEN_COUNT] = {
    [DEMO_DESIGN_1] = "design-1",
    [DEMO_DESIGN_2] = "design-2",
    [DEMO_DESIGN_3] = "design-3",
    [DEMO_NO_DATA] = "no-data",
    [DEMO_WORKSHOP_LIVE] = "workshop-live",
    [DEMO_WORKSHOP_TRENDS] = "workshop-trends",
    [DEMO_WORKSHOP_CAN] = "workshop-can",
};

const char *demo_screen_name(demo_screen_t screen)
{
    return screen < DEMO_SCREEN_COUNT ? names[screen] : "unknown";
}

/* Values read off the three designs. */
typedef struct {
    int32_t front_brake, rear_brake; /* percent of the bar */
    dd_level_t front_level;
    int32_t rpm;
    int32_t cvt, lap, engine;
    dd_level_t cvt_level;
    int32_t speed;
    int32_t lap_time_ms, delta_ms, last_lap_ms;
    int32_t fuel;
    bool alert;
} design_t;

static const design_t designs[] = {
    [DEMO_DESIGN_1] = {10, 60, DD_LEVEL_CRITICAL, 1600, 104, 37, 118, DD_LEVEL_WARNING, 29, 0, 0, 0, 22, true},
    [DEMO_DESIGN_2] = {0, 0, DD_LEVEL_NORMAL, 3900, 83, 37, 109, DD_LEVEL_NORMAL, 51, 134600, 360, 222800, 58, false},
    [DEMO_DESIGN_3] = {50, 40, DD_LEVEL_NORMAL, 3100, 78, 37, 104, DD_LEVEL_NORMAL, 38, 134600, -840, 222800, 62, false},
};

static lv_obj_t *create_design(demo_screen_t screen)
{
    const design_t *d = &designs[screen];
    driver_view_t view;
    driver_view_create(&view);

    dd_bar_group_set_value(view.brakes, DRIVER_VIEW_FRONT, d->front_brake, d->front_level);
    dd_bar_group_set_value(view.brakes, DRIVER_VIEW_REAR, d->rear_brake, DD_LEVEL_NORMAL);
    dd_rpm_bar_set_rpm(view.rpm, d->rpm);
    dd_readout_set_int(view.cvt, d->cvt, "°");
    dd_readout_set_level(view.cvt, d->cvt_level);
    dd_readout_set_int(view.lap, d->lap, NULL);
    dd_readout_set_int(view.engine, d->engine, "°");
    dd_hero_value_set_int(view.speed, d->speed);
    dd_segment_gauge_set_percent(view.fuel, d->fuel);

    char text[16];
    dd_format_lap_time(text, sizeof(text), d->lap_time_ms);
    dd_stat_set_text(view.lap_time, text, DD_LEVEL_NORMAL);
    dd_format_delta(text, sizeof(text), d->delta_ms);
    dd_stat_set_text(view.delta, text, dd_delta_level(d->delta_ms));
    dd_format_lap_time(text, sizeof(text), d->last_lap_ms);
    dd_stat_set_text(view.last_lap, text, DD_LEVEL_NORMAL);

    if (d->alert) {
        driver_view_show_alert(&view, DD_LEVEL_CRITICAL, "FRONT BRAKE CIRCUIT", "140 vs 860 PSI", "PIT NOW");
    }
    return view.screen;
}

/* Every component starts unavailable, so an empty view shows what the driver sees
 * when the CAN bus is silent. */
static lv_obj_t *create_no_data(void)
{
    driver_view_t view;
    driver_view_create(&view);
    dd_bar_group_set_unavailable(view.brakes, DRIVER_VIEW_FRONT);
    dd_bar_group_set_unavailable(view.brakes, DRIVER_VIEW_REAR);
    dd_rpm_bar_set_unavailable(view.rpm);
    return view.screen;
}

static lv_obj_t *create_workshop_screen(const char *title, uint32_t page, lv_obj_t **content)
{
    lv_obj_t *screen = dd_screen_create();
    lv_obj_t *header = dd_header_create(screen, title, "WORKSHOP");
    dd_header_set_page(header, page, WORKSHOP_VIEWS);

    *content = dd_box_create(screen);
    lv_obj_set_pos(*content, MARGIN, 72 + MARGIN);
    lv_obj_set_size(*content, 1024 - 2 * MARGIN, 600 - 72 - 2 * MARGIN);
    return screen;
}

static lv_obj_t *create_workshop_live(void)
{
    lv_obj_t *grid;
    lv_obj_t *screen = create_workshop_screen("LIVE VALUES", 0, &grid);
    static const int32_t columns[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                      LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, columns, rows);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    lv_obj_set_style_pad_row(grid, GAP, 0);

    static const struct {
        const char *caption, *unit, *note;
        int32_t value;
        dd_level_t level;
    } tiles[] = {
        {"SPEED", "KM/H", "GPS · not on CAN yet", 38, DD_LEVEL_NORMAL},
        {"ENGINE", "RPM", "0x0BB · node 5", 3100, DD_LEVEL_NORMAL},
        {"GEARBOX", "RPM", "0x0B9 · node 4", 1420, DD_LEVEL_NORMAL},
        {"CVT", "°C", "warning above 100", 104, DD_LEVEL_WARNING},
        {"FRONT BRAKE", "PSI", "0x0B1 · node 1", 140, DD_LEVEL_CRITICAL},
        {"REAR BRAKE", "PSI", "0x0B2 · node 2", 860, DD_LEVEL_NORMAL},
        {"ENGINE TEMP", "°C", "warning above 120", 118, DD_LEVEL_NORMAL},
        {"FUEL", "%", "no sensor yet", 0, DD_LEVEL_NORMAL},
    };
    for (uint32_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
        lv_obj_t *tile = dd_value_tile_create(grid, tiles[i].caption, tiles[i].unit);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % 4, 1, LV_GRID_ALIGN_STRETCH, i / 4, 1);
        dd_value_tile_set_note(tile, tiles[i].note);
        if (i == 7) {
            dd_value_tile_set_unavailable(tile);
        } else {
            dd_value_tile_set_int(tile, tiles[i].value, tiles[i].level);
        }
    }
    return screen;
}

static void fill_trend(lv_obj_t *trend, uint32_t points, int32_t base, int32_t swing, uint32_t seed,
                       int32_t warn_below, bool gap)
{
    for (uint32_t i = 0; i < points; i++) {
        if (gap && i >= points - 8 && i < points - 3) {
            dd_trend_push_unavailable(trend);
            continue;
        }
        int32_t value = base + lv_trigo_sin((int32_t)(i * 9 + seed)) * swing / LV_TRIGO_SIN_MAX
                        + lv_trigo_sin((int32_t)(i * 37 + seed * 3)) * swing / (4 * LV_TRIGO_SIN_MAX);
        dd_trend_push(trend, value, value < warn_below ? DD_LEVEL_WARNING : DD_LEVEL_NORMAL);
    }
}

static lv_obj_t *create_workshop_trends(void)
{
    lv_obj_t *grid;
    lv_obj_t *screen = create_workshop_screen("TRENDS · LAST 30 S", 1, &grid);
    static const int32_t columns[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, columns, rows);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    lv_obj_set_style_pad_row(grid, GAP, 0);

    const uint32_t points = 60;
    const dd_trend_config_t configs[] = {
        {"ENGINE", "RPM", 0, 4000, points, true, 3400},
        {"SPEED", "KM/H", 0, 60, points, false, 0},
        {"FRONT BRAKE", "PSI", 0, 1000, points, true, 300},
        {"CVT", "°C", 20, 140, points, true, 100},
    };
    for (uint32_t i = 0; i < 4; i++) {
        lv_obj_t *trend = dd_trend_create(grid, &configs[i]);
        lv_obj_set_grid_cell(trend, LV_GRID_ALIGN_STRETCH, i % 2, 1, LV_GRID_ALIGN_STRETCH, i / 2, 1);
    }
    lv_obj_t *engine = lv_obj_get_child(grid, 0);
    lv_obj_t *speed = lv_obj_get_child(grid, 1);
    lv_obj_t *brake = lv_obj_get_child(grid, 2);
    lv_obj_t *cvt = lv_obj_get_child(grid, 3);
    fill_trend(engine, points, 2600, 900, 0, INT32_MIN, false);
    fill_trend(speed, points, 36, 14, 40, INT32_MIN, false);
    fill_trend(brake, points, 520, 380, 90, 300, true);
    fill_trend(cvt, points, 92, 10, 200, INT32_MIN, false);
    return screen;
}

static lv_obj_t *create_workshop_can(void)
{
    lv_obj_t *content;
    lv_obj_t *screen = create_workshop_screen("CAN AND DATA HEALTH", 2, &content);
    lv_obj_t *list = dd_status_list_create(content);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));

    static const struct {
        const char *name, *id, *status, *detail;
        dd_level_t level;
    } rows[] = {
        {"Master", "0x0A2 beacon", "OK", "1 Hz · 0.4 s ago", DD_LEVEL_GOOD},
        {"Front brake", "0x0B1 · node 1", "OK", "100 Hz · 8 ms ago", DD_LEVEL_GOOD},
        {"Rear brake", "0x0B2 · node 2", "OK", "100 Hz · 6 ms ago", DD_LEVEL_GOOD},
        {"Gearbox speed", "0x0B9 · node 4", "OK", "50 Hz · 14 ms ago", DD_LEVEL_GOOD},
        {"Engine RPM", "0x0BB · node 5", "LATE", "3 Hz · 410 ms ago", DD_LEVEL_WARNING},
        {"Generic ADC", "0x0BA · node 3", "NO DATA", "never received", DD_LEVEL_NORMAL},
        {"Bus", "1 Mbit/s · listen-only", "ERRORS", "12 lost frames", DD_LEVEL_CRITICAL},
    };
    for (uint32_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint32_t row = dd_status_list_add(list, rows[i].name, rows[i].id);
        dd_status_list_set(list, row, rows[i].level, rows[i].status, rows[i].detail);
    }
    return screen;
}

lv_obj_t *demo_screen_create(demo_screen_t screen)
{
    switch (screen) {
    case DEMO_DESIGN_1:
    case DEMO_DESIGN_2:
    case DEMO_DESIGN_3:
        return create_design(screen);
    case DEMO_NO_DATA:
        return create_no_data();
    case DEMO_WORKSHOP_LIVE:
        return create_workshop_live();
    case DEMO_WORKSHOP_TRENDS:
        return create_workshop_trends();
    case DEMO_WORKSHOP_CAN:
    default:
        return create_workshop_can();
    }
}
