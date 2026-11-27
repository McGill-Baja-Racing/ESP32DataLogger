// Internal C API for CANopen drive access; no application operation scheduling.
// Only the drive owner issues transactions; CAN RX may update position feedback.
#ifndef CANOPEN_DRIVE_H
#define CANOPEN_DRIVE_H

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
} canopen_drive_can_rx_item_t;

// Attach transport resources without sending commands; resets only local position offset.
esp_err_t canopen_drive_init(twai_node_handle_t node_hdl, QueueHandle_t servo_sdo_queue,
                             uint8_t node_id, const char *log_tag);

// Owner-only NMT and expedited 8/16/32-bit SDO access. Reads require exact width
// and matching object/time; writes report acknowledgement, not independent readback.
esp_err_t canopen_drive_nmt_send(uint8_t cmd, uint8_t node);
esp_err_t canopen_drive_sdo_write_u8(uint16_t idx, uint8_t sub, uint8_t v);
esp_err_t canopen_drive_sdo_write_u16(uint16_t idx, uint8_t sub, uint16_t v);
esp_err_t canopen_drive_sdo_write_u32(uint16_t idx, uint8_t sub, uint32_t v);
esp_err_t canopen_drive_sdo_read_u8(uint16_t idx, uint8_t sub, uint8_t *value);
esp_err_t canopen_drive_sdo_read_u16(uint16_t idx, uint8_t sub, uint16_t *value);
esp_err_t canopen_drive_sdo_read_u32(uint16_t idx, uint8_t sub, uint32_t *value);

// Configure the DSY-C PDO mappings used by position-mode setup. Mapping words
// encode object index/subindex/bit width; see docs/vendor/DSY-C_*_Manual_V1.0.pdf.
esp_err_t canopen_drive_configure_rpdo1_map_controlword_only(void);
esp_err_t canopen_drive_configure_rpdo2(void);
esp_err_t canopen_drive_configure_rpdo3_map_profile_accel_decel(void);
esp_err_t canopen_drive_configure_tpdo1_map_statusword_only(void);
esp_err_t canopen_drive_configure_tpdo2_map_current_only(void);
esp_err_t canopen_drive_configure_tpdo2_map_position_only(void);
esp_err_t canopen_drive_configure_tpdo2_inhibit_100hz(void);
esp_err_t canopen_drive_configure_tpdo2_fast_event(void);
esp_err_t canopen_drive_configure_tpdo3_map_velocity_only(void);
esp_err_t canopen_drive_configure_tpdo3_fast_event(void);
esp_err_t canopen_drive_configure_tpdo4_map_current_only(void);
esp_err_t canopen_drive_configure_tpdo4_fast_event(void);

// Position is logical motor rev, speed is motor RPM, acceleration is RPM/s.
// Conversion retains 10000 user counts/rev; no gearbox/output-shaft conversion.
// PDO send success is transmission completion, not acknowledgement of physical motion.
esp_err_t canopen_drive_send_controlword(uint16_t cw);
esp_err_t canopen_drive_send_target_position(float pos_revs);
esp_err_t canopen_drive_send_target_position_and_speed(float pos_revs, float speed_rpm);
esp_err_t canopen_drive_send_profile_accel_decel(float accel_rpm_s, float decel_rpm_s);
// RX-only update of physical drive position; getter applies the local zero offset.
esp_err_t canopen_drive_set_actual_position_feedback(float pos_revs);
float canopen_drive_get_actual_position_feedback(void);
// Explicit local offset capture; normal manual moves never call this function.
esp_err_t canopen_drive_capture_current_position_as_zero(void);
esp_err_t canopen_drive_set_directional_profile(float accel_up_rpm_s, float decel_up_rpm_s,
                                                float accel_down_rpm_s, float decel_down_rpm_s);
esp_err_t canopen_drive_send_target_position_speed_directional_profile(float pos_revs,
                                                                       float speed_rpm);
esp_err_t canopen_drive_send_target_position_speed_accel_decel(float pos_revs, float speed_rpm,
                                                               float accel_rpm_s,
                                                               float decel_rpm_s);
// Low-level word sequences only: the operation layer owns live state confirmation.
esp_err_t canopen_drive_enable_operation_sequence_position(void);
esp_err_t canopen_drive_enable_position(void);

#ifdef __cplusplus
}
#endif

#endif
