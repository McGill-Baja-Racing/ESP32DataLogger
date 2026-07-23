#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

#include "can/can_node.h"
#include "sampler/sampler.h"
#include "time/time_sync.h"

#ifndef SENSOR_SERIAL_TEST
#define SENSOR_SERIAL_TEST 0
#endif

/*
 * Application composition root
 * ----------------------------
 * main.c only defines how the major modules are connected. CAN owns command
 * reception and recovery; sampler owns sensors and sample flow; time_sync owns
 * the master clock offset. See src/README.md for the full dependency map.
 */

static void handle_start(void)
{
    sampler_start();
}

static void handle_stop(void)
{
    sampler_stop();
}

void app_main(void)
{
    /* Sensor hardware and sampling tasks exist before CAN can issue START. */
    ESP_ERROR_CHECK(sampler_init());

#if SENSOR_SERIAL_TEST
    /* Standalone bench mode: no CAN bus or START command is required. */
    ESP_LOGI("SensorTest",
             "Standalone serial mode enabled; CAN is disabled");
    sampler_start();
#else
    /* CAN translates protocol frames into these application-level actions. */
    can_node_callbacks_t callbacks = {
        .on_start = handle_start,
        .on_stop = handle_stop,
        .on_time_beacon = time_sync_update,
        .on_bus_off = sampler_discard_pending,
    };
    ESP_ERROR_CHECK(can_node_init(&callbacks, (uint8_t)esp_reset_reason()));

    /* The master uses this acknowledgement to register the node after boot. */
    can_node_report_state(NODE_STATE_IDLE, NODE_STATE_REASON_BOOT);
#endif
}
