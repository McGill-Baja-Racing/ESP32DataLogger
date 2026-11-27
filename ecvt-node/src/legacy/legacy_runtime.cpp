#include "legacy_runtime.h"
#include "app/modules_internal.h"
#include <atomic>

namespace {
bool sd_available;
std::atomic<bool> replay_alive{false};
void replay_task(void *)
{
    read_sample_data(nullptr);
    replay_alive.store(false);
    vTaskDelete(nullptr);
}
esp_err_t run_automatic(void *)
{
    s_latest_throttle = 0; s_latest_engine_rpm = 0; s_latest_wheel_rpm = 0; s_latest_brake_state = false;
    xQueueReset(twai_rx_sample_queue);
    s_sample_rpm_enabled = true;
    if (sd_available) {
        replay_alive.store(true);
        if (xTaskCreate(replay_task, "csv_replay", 8192, nullptr, 5, nullptr) != pdPASS) {
            replay_alive.store(false); return ESP_ERR_NO_MEM;
        }
    }
    calc_task(nullptr);
    return ESP_OK;
}
void stop_inputs(void *) { s_sample_rpm_enabled = false; }
void wait_inputs(void *) { while (replay_alive.load()) vTaskDelay(pdMS_TO_TICKS(10)); }
void record_command(void *, float position, float rpm)
{
    queue_controller_command_log(position, rpm, s_latest_throttle, s_latest_engine_rpm,
        s_latest_wheel_rpm, s_target_engine_rpm_running, s_latest_brake_state);
}
esp_err_t start_recording(void *) { return sd_available ? logger_session_start() : ESP_ERR_INVALID_STATE; }
void stop_recording(void *) { logger_session_stop(); }
bool recording_stopped(void *) { return stop_logging.load(); }
uint32_t dropped_records(void *) { return logger_dropped_records(); }
}
MotorCoreServices legacy_motor_services(bool storage_available)
{
    sd_available = storage_available;
    MotorCoreServices services;
    services.run_automatic=run_automatic;
    services.stop_inputs=stop_inputs;
    services.wait_inputs=wait_inputs;
    services.record_command=record_command;
    services.start_recording=start_recording;
    services.stop_recording=stop_recording;
    services.recording_stopped=recording_stopped;
    services.dropped_records=dropped_records;
    return services;
}
