#include "app/modules_internal.h"
#include "app/ecvt_app.h"
#include "console/serial_console.h"

static_assert(sizeof(can_rx_item_t) == sizeof(ecvt_motor_can_rx_item_t), "SDO queue element layout must match");
static_assert(offsetof(can_rx_item_t, received_us) == offsetof(ecvt_motor_can_rx_item_t, received_us), "SDO metadata layout must match");


static const rpm_sensor_task_config_t s_wheel_rpm_task_cfg = {
    WHEEL_ADC_CH_USED,
    CAN_SENSOR_WHEEL_RPM,
    "Wheel RPM",
    PULSES_PER_ROTATION,
    15000000
};
static const rpm_sensor_task_config_t s_engine_rpm_task_cfg = {
    ENGINE_ADC_CH_USED,
    CAN_SENSOR_ENGINE_RPM,
    "Engine RPM",
    1,
    0
};



void app_main() {
    const bool sd_available = mount_sd_card();
    if (!sd_available) ESP_LOGW(TAG, "SD unavailable; terminal/control remain available.");

    // CAN Setup
    node_cfg.io_cfg.tx = (gpio_num_t) TX_GPIO_NUM;
    node_cfg.io_cfg.rx = (gpio_num_t) RX_GPIO_NUM;
    node_cfg.bit_timing.bitrate = BITRATE_BPS;
    node_cfg.tx_queue_depth = 8;

    cbs.on_rx_done = twai_rx_cb;

    twai_rx_raw_queue  = xQueueCreate(256, sizeof(can_rx_item_t));
    twai_rx_sample_queue  = xQueueCreate(256, sizeof(can_rx_item_t));
#if !WHEEL_RPM_TEST_MODE
    twai_rx_servo_SDO_queue  = xQueueCreate(256, sizeof(can_rx_item_t));
    log_record_queue  = xQueueCreate(256, sizeof(log_record_t));
    s_fsm_ctx.latest_tpdo1_statusword = &s_latest_tpdo1_statusword;
    s_fsm_ctx.latest_tpdo1_rx_us = &s_latest_tpdo1_rx_us;

    log_file_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(twai_rx_raw_queue && twai_rx_sample_queue && twai_rx_servo_SDO_queue &&
                    log_record_queue && log_file_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(twai_new_node_onchip(&node_cfg, &node_hdl));
    ESP_LOGI(TAG, "TWAI node created");

    ESP_ERROR_CHECK(twai_node_register_event_callbacks(node_hdl, &cbs, NULL));
    ESP_LOGI(TAG, "TWAI callbacks registered");

    ESP_ERROR_CHECK(twai_node_enable(node_hdl));
    ESP_LOGI(TAG, "TWAI enabled (bitrate=%d, NodeID=%d)", BITRATE_BPS, NODE_ID);
    ESP_ERROR_CHECK(ecvt_motor_init(node_hdl, twai_rx_servo_SDO_queue, NODE_ID, TAG));
#endif
    configure_rpm_sensor_adc_channels();

    ESP_ERROR_CHECK(xTaskCreate(sample_rpm_task, "wheel_rpm", 4096, (void *)&s_wheel_rpm_task_cfg, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(sample_rpm_task, "engine_rpm", 4096, (void *)&s_engine_rpm_task_cfg, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(rx_dispatch_task, "rx_dispatch", 4096, NULL, 7, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    if (sd_available) {
        ESP_ERROR_CHECK(xTaskCreate(sd_writer_task, "sd_writer", 8192, nullptr, 8, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    ESP_ERROR_CHECK(ecvt_app_start(sd_available));
    ESP_ERROR_CHECK(serial_console_start());
    ESP_LOGI(TAG, "Idle. No servo commands sent. Type help; configure limits before motion.");
}
