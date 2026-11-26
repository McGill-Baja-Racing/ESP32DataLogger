#ifndef ECVT_MOTOR_H
#define ECVT_MOTOR_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    int64_t received_us;
} ecvt_motor_can_rx_item_t;

esp_err_t ecvt_motor_init(twai_node_handle_t node_hdl,
                          QueueHandle_t servo_sdo_queue,
                          uint8_t node_id,
                          const char *log_tag);

esp_err_t ecvt_motor_nmt_send(uint8_t cmd, uint8_t node);
esp_err_t ecvt_motor_sdo_write_u8(uint16_t idx, uint8_t sub, uint8_t v);
esp_err_t ecvt_motor_sdo_write_u16(uint16_t idx, uint8_t sub, uint16_t v);
esp_err_t ecvt_motor_sdo_write_u32(uint16_t idx, uint8_t sub, uint32_t v);
esp_err_t ecvt_motor_sdo_read_u8(uint16_t idx, uint8_t sub, uint8_t *value);
esp_err_t ecvt_motor_sdo_read_u16(uint16_t idx, uint8_t sub, uint16_t *value);
esp_err_t ecvt_motor_sdo_read_u32(uint16_t idx, uint8_t sub, uint32_t *value);

esp_err_t ecvt_motor_configure_rpdo1_map_controlword_only(void);
esp_err_t ecvt_motor_configure_rpdo2(void);
esp_err_t ecvt_motor_configure_rpdo3_map_profile_accel_decel(void);
esp_err_t ecvt_motor_configure_tpdo1_map_statusword_only(void);
esp_err_t ecvt_motor_configure_tpdo2_map_current_only(void);
esp_err_t ecvt_motor_configure_tpdo2_map_position_only(void);
esp_err_t ecvt_motor_configure_tpdo2_inhibit_100hz(void);
esp_err_t ecvt_motor_configure_tpdo2_fast_event(void);
esp_err_t ecvt_motor_configure_tpdo3_map_velocity_only(void);
esp_err_t ecvt_motor_configure_tpdo3_fast_event(void);
esp_err_t ecvt_motor_configure_tpdo4_map_current_only(void);
esp_err_t ecvt_motor_configure_tpdo4_fast_event(void);

esp_err_t ecvt_motor_send_controlword(uint16_t cw);
esp_err_t ecvt_motor_send_target_position(float pos_revs);
esp_err_t ecvt_motor_send_target_position_and_speed(float pos_revs, float speed_revs_per_sec);
esp_err_t ecvt_motor_send_profile_accel_decel(float accel_revs_per_sec2, float decel_revs_per_sec2);
esp_err_t ecvt_motor_set_actual_position_feedback(float pos_revs);
float ecvt_motor_get_actual_position_feedback(void);
esp_err_t ecvt_motor_capture_current_position_as_zero(void);
esp_err_t ecvt_motor_set_directional_profile(float accel_up_revs_per_sec2,
                                             float decel_up_revs_per_sec2,
                                             float accel_down_revs_per_sec2,
                                             float decel_down_revs_per_sec2);
esp_err_t ecvt_motor_send_target_position_speed_directional_profile(float pos_revs,
                                                                    float speed_revs_per_sec);
esp_err_t ecvt_motor_send_target_position_speed_accel_decel(float pos_revs,
                                                            float speed_revs_per_sec,
                                                            float accel_revs_per_sec2,
                                                            float decel_revs_per_sec2);
esp_err_t ecvt_motor_enable_operation_sequence_position(void);
esp_err_t ecvt_motor_enable_position(void);

#ifdef __cplusplus
}
#endif

#endif
