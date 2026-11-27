#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "can/can_node.h"
#include "health/node_health.h"
#include "sampler/sampler.h"
#include "time/time_sync.h"
#include "update/firmware_update.h"

#include "protocol/node_config.h"

#define SENSOR_STARTUP_DELAY_MS 3000
/* How long a booting node waits for the master's clock before sampling anyway. */
#define FIRST_BEACON_WAIT_MS (3 * CAN_MASTER_TIME_PERIOD_MS)

/*
 * Application composition root
 * ----------------------------
 * main.c only defines how the major modules are connected. CAN owns command
 * reception and recovery; sampler owns sensors and sample flow; time_sync owns
 * the master clock offset. See src/README.md for the full dependency map.
 */

/* The master started recording: health events from now on describe the recording. */
static void handle_start(void)
{
    node_health_recording_started();
}

void app_main(void)
{
    /*
     * Allow USB serial/JTAG and the host monitor time to reconnect after reset.
     * The master waits five seconds before START, leaving about two seconds for
     * this node to initialize CAN and report its boot state.
     */
    vTaskDelay(pdMS_TO_TICKS(SENSOR_STARTUP_DELAY_MS));

#if !SENSOR_SERIAL_TEST
    /* Before the sensors, so a new image that hangs in their init still rolls back. */
    ESP_ERROR_CHECK(firmware_update_init());
#endif
    /* Sensor hardware and sampling tasks exist before CAN starts. */
    ESP_ERROR_CHECK(sampler_init());

#if SENSOR_SERIAL_TEST
    /* Standalone bench mode: no CAN bus or START command is required. */
    ESP_LOGI("SensorTest",
             "Standalone serial mode enabled; CAN is disabled");
    sampler_start();
#else
    /* CAN translates protocol frames into these application-level actions.
     * Sampling no longer follows START/STOP: the node samples and sends from
     * boot, and the master decides what it logs. */
    can_node_callbacks_t callbacks = {
        .on_start = handle_start,
        .on_stop = NULL,
        .on_time_beacon = time_sync_update,
        .on_bus_off = sampler_discard_pending,
        .on_update_command = firmware_update_handle_command,
        .on_update_data = firmware_update_handle_data,
    };
    ESP_ERROR_CHECK(can_node_init(&callbacks, (uint8_t)esp_reset_reason()));
    /* The master uses this acknowledgement to register the node after boot. */
    can_node_report_state(NODE_STATE_IDLE, NODE_STATE_REASON_BOOT);
    firmware_update_report_identity();
    ESP_ERROR_CHECK(node_health_init());
    /* A node that reboots during a recording must not send samples on its own
     * clock: the master would log them. Wait briefly for the beacon, then sample
     * with or without a master (timestamps then use this node's own clock). */
    for (uint32_t waited_ms = 0;
         waited_ms < FIRST_BEACON_WAIT_MS && !time_sync_is_current(FIRST_BEACON_WAIT_MS);
         waited_ms += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    sampler_start();
    /* A new image confirms itself only once every init step above passed and the master answered. */
    while (!time_sync_is_current(3 * CAN_MASTER_TIME_PERIOD_MS)) {
        vTaskDelay(pdMS_TO_TICKS(CAN_MASTER_TIME_PERIOD_MS));
    }
    firmware_update_confirm();
#endif
}
