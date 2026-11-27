// CANopen SDO/NMT/PDO transactions and drive-unit serialization.
// Calls are serialized by drive/control; bounded replies are matched to requests.
#include "drive.h"
#include "drive/operation_context.h"
#include <limits.h>
#include <stdatomic.h>
#include "esp_timer.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define NMT_ID 0x000
#define SDO_WAIT_MS 400

static twai_node_handle_t s_node_hdl = NULL;
static QueueHandle_t s_servo_sdo_queue = NULL;
static uint8_t s_node_id = 0;
static const char *s_log_tag = "ECVT";
static int64_t s_last_tx_started_us;
static float s_accel_up_rpm_s = 4000.0f;
static float s_decel_up_rpm_s = 4000.0f;
static float s_accel_down_rpm_s = 3000.0f;
static float s_decel_down_rpm_s = 3000.0f;
static _Atomic float s_latest_actual_position_revs = 0.0f;
static _Atomic float s_latest_actual_position_physical_revs = 0.0f;
static _Atomic float s_position_zero_offset_revs = 0.0f;

#define SDO_TX_ID (0x600 + s_node_id)
#define SDO_RX_ID (0x580 + s_node_id)
#define RPDO1_ID (0x200 + s_node_id)
#define RPDO2_ID (0x300 + s_node_id)
#define RPDO3_ID (0x400 + s_node_id)

static uint32_t u32_le(const uint8_t *b)
{
    return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

static esp_err_t ensure_init(void)
{
    if (!s_node_hdl || !s_servo_sdo_queue || s_node_id == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static bool wait_for_id(uint32_t want_id, canopen_drive_can_rx_item_t *out, uint32_t timeout_ms)
{
    TickType_t to = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();

    while (true) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= to)
            return false;
        canopen_drive_can_rx_item_t item;
        TickType_t left = to - elapsed;
        if (xQueueReceive(s_servo_sdo_queue, &item, left) == pdTRUE) {
            if (item.id == want_id) {
                if (out) {
                    *out = item;
                }
                return true;
            }
        }
    }
    return false;
}

// Persistent TX storage survives a completion timeout; never reuse it while pending.
static esp_err_t can_send(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    ESP_RETURN_ON_ERROR(ensure_init(), s_log_tag, "Motor driver not initialized");

    if (dlc > 8) {
        dlc = 8;
    }

    // Persistent buffers remain valid even if hardware completion times out.
    // Calls are serialized by the application's single motor-owner task.
    static uint8_t tx_bytes[8];
    static twai_frame_t tx;
    static bool pending = false;
    if (pending) {
        esp_err_t error = twai_node_transmit_wait_all_done(s_node_hdl, 100);
        if (error != ESP_OK)
            return error;
        pending = false;
    }
    memcpy(tx_bytes, data, dlc);
    memset(&tx, 0, sizeof(tx));
    tx.header.id = id;
    tx.header.dlc = dlc;
    tx.buffer = tx_bytes;
    tx.buffer_len = dlc;
    s_last_tx_started_us = esp_timer_get_time();
    esp_err_t error = twai_node_transmit(s_node_hdl, &tx, 100);
    if (error != ESP_OK)
        return error;
    pending = true;
    error = twai_node_transmit_wait_all_done(s_node_hdl, 100);
    if (error == ESP_OK)
        pending = false;
    return error;
}

// Send one expedited download and reject stale, aborted or mismatched replies.
static esp_err_t sdo_write_8b(uint8_t cs, uint16_t idx, uint8_t sub, uint32_t payload_le)
{
    uint8_t d[8] = {0};
    d[0] = cs;
    d[1] = (uint8_t)(idx & 0xFF);
    d[2] = (uint8_t)(idx >> 8);
    d[3] = sub;
    d[4] = (uint8_t)(payload_le & 0xFF);
    d[5] = (uint8_t)((payload_le >> 8) & 0xFF);
    d[6] = (uint8_t)((payload_le >> 16) & 0xFF);
    d[7] = (uint8_t)((payload_le >> 24) & 0xFF);

    ESP_RETURN_ON_ERROR(can_send(SDO_TX_ID, d, 8), s_log_tag, "SDO tx failed");
    const int64_t requested_us = s_last_tx_started_us;

    canopen_drive_can_rx_item_t r = {0};
    if (!wait_for_id(SDO_RX_ID, &r, SDO_WAIT_MS)) {
        ESP_LOGE(s_log_tag, "SDO timeout idx=0x%04X sub=0x%02X", idx, sub);
        return ESP_ERR_TIMEOUT;
    }

    if (r.received_us < requested_us)
        return ESP_FAIL;

    if (r.dlc == 8 && r.data[0] == 0x80) {
        uint16_t ridx = (uint16_t)(r.data[1] | (r.data[2] << 8));
        uint8_t rsub = r.data[3];
        uint32_t acode = u32_le(&r.data[4]);
        ESP_LOGE(s_log_tag, "SDO ABORT idx=0x%04X sub=0x%02X code=0x%08" PRIX32, ridx, rsub, acode);
        return ESP_FAIL;
    }

    if (r.dlc == 8 && r.data[0] == 0x60 && r.data[1] == (uint8_t)(idx & 0xFF) &&
        r.data[2] == (uint8_t)(idx >> 8) && r.data[3] == sub) {
        return ESP_OK;
    }

    ESP_LOGW(s_log_tag, "Unexpected SDO response");
    return ESP_FAIL;
}

esp_err_t canopen_drive_init(twai_node_handle_t node_hdl, QueueHandle_t servo_sdo_queue,
                             uint8_t node_id, const char *log_tag)
{
    if (!node_hdl || !servo_sdo_queue || node_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_node_hdl = node_hdl;
    s_servo_sdo_queue = servo_sdo_queue;
    s_node_id = node_id;
    s_log_tag = (log_tag && log_tag[0] != '\0') ? log_tag : "ECVT";
    s_latest_actual_position_revs = 0.0f;
    s_latest_actual_position_physical_revs = 0.0f;
    s_position_zero_offset_revs = 0.0f;
    return ESP_OK;
}

// Expedited upload only: exact data width, object address and fresh receive time.
static esp_err_t sdo_read_value(uint16_t idx, uint8_t sub, unsigned width, uint32_t *value)
{
    if (!value)
        return ESP_ERR_INVALID_ARG;
    uint8_t request[8] = {0x40, (uint8_t)idx, (uint8_t)(idx >> 8), sub, 0, 0, 0, 0};
    ESP_RETURN_ON_ERROR(can_send(SDO_TX_ID, request, 8), s_log_tag, "SDO read tx failed");
    const int64_t requested_us = s_last_tx_started_us;
    canopen_drive_can_rx_item_t response = {0};
    if (!wait_for_id(SDO_RX_ID, &response, SDO_WAIT_MS))
        return ESP_ERR_TIMEOUT;
    const uint8_t expected = width == 1 ? 0x4F : (width == 2 ? 0x4B : 0x43);
    if (response.dlc == 8 && response.data[0] == 0x80)
        ESP_LOGE(s_log_tag, "SDO read abort idx=0x%04x sub=0x%02x code=0x%08" PRIx32, idx, sub,
                 u32_le(&response.data[4]));
    else if (response.dlc == 8 && response.data[0] != expected)
        ESP_LOGE(s_log_tag,
                 "SDO read size mismatch idx=0x%04x sub=0x%02x response=0x%02x expected=0x%02x",
                 idx, sub, response.data[0], expected);
    if (response.received_us < requested_us || response.dlc != 8 || response.data[0] != expected ||
        response.data[1] != (uint8_t)idx || response.data[2] != (uint8_t)(idx >> 8) ||
        response.data[3] != sub)
        return ESP_FAIL;
    uint32_t result = 0;
    for (unsigned i = 0; i < width; ++i)
        result |= (uint32_t)response.data[4 + i] << (8 * i);
    *value = result;
    return ESP_OK;
}

esp_err_t canopen_drive_sdo_read_u8(uint16_t idx, uint8_t sub, uint8_t *value)
{
    if (!value)
        return ESP_ERR_INVALID_ARG;
    uint32_t result;
    esp_err_t error = sdo_read_value(idx, sub, 1, &result);
    if (error == ESP_OK)
        *value = (uint8_t)result;
    return error;
}

esp_err_t canopen_drive_sdo_read_u16(uint16_t idx, uint8_t sub, uint16_t *value)
{
    if (!value)
        return ESP_ERR_INVALID_ARG;
    uint32_t result;
    esp_err_t error = sdo_read_value(idx, sub, 2, &result);
    if (error == ESP_OK)
        *value = (uint16_t)result;
    return error;
}

esp_err_t canopen_drive_sdo_read_u32(uint16_t idx, uint8_t sub, uint32_t *value)
{
    return sdo_read_value(idx, sub, 4, value);
}

esp_err_t canopen_drive_nmt_send(uint8_t cmd, uint8_t node)
{
    uint8_t d[2] = {cmd, node};
    return can_send(NMT_ID, d, 2);
}

esp_err_t canopen_drive_sdo_write_u8(uint16_t idx, uint8_t sub, uint8_t v)
{
    return sdo_write_8b(0x2F, idx, sub, (uint32_t)v);
}

esp_err_t canopen_drive_sdo_write_u16(uint16_t idx, uint8_t sub, uint16_t v)
{
    return sdo_write_8b(0x2B, idx, sub, (uint32_t)v);
}

esp_err_t canopen_drive_sdo_write_u32(uint16_t idx, uint8_t sub, uint32_t v)
{
    return sdo_write_8b(0x23, idx, sub, v);
}

esp_err_t canopen_drive_configure_rpdo1_map_controlword_only(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1600, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1600, 0x01, 0x60400010), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1600, 0x00, 1), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "RPDO1 -> 0x6040 Controlword (16-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_rpdo2(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1601, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1601, 0x01, 0x607A0020), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1601, 0x02, 0x60810020), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1601, 0x00, 2), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "RPDO2 -> 0x607A Target position (32-bit), 0x6081 Target speed (32-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_rpdo3_map_profile_accel_decel(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1602, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1602, 0x01, 0x60830020), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1602, 0x02, 0x60840020), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1602, 0x00, 2), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag,
             "RPDO3 -> 0x6083 Profile acceleration (32-bit), 0x6084 Profile deceleration (32-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo1_map_statusword_only(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A00, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1A00, 0x01, 0x60410010), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A00, 0x00, 1), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "TPDO1 -> 0x6041 Statusword (16-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo2_map_current_only(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A01, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1A01, 0x01, 0x60780010), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A01, 0x00, 1), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "TPDO2 -> 0x6078 Current actual value (16-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo2_map_position_only(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A01, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1A01, 0x01, 0x60640020), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A01, 0x00, 1), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "TPDO2 -> 0x6064 Position actual value (32-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo2_inhibit_100hz(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1801, 0x02, 0xFF), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1801, 0x03, 100), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "TPDO2: transmission=255 (event), inhibit=10ms (~100Hz max)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo2_fast_event(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1801, 0x02, 0xFF), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1801, 0x03, 200), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1801, 0x05, 30), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(
        s_log_tag,
        "TPDO2: transmission=255, inhibit_raw=200 (100us units), event=30ms (drive-dependent)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo3_map_velocity_only(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A02, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1A02, 0x01, 0x606C0020), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A02, 0x00, 1), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "TPDO3 -> 0x606C Velocity actual value (32-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo3_fast_event(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1802, 0x02, 0xFF), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1802, 0x03, 200), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1802, 0x05, 30), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(
        s_log_tag,
        "TPDO3: transmission=255, inhibit_raw=200 (100us units), event=30ms (drive-dependent)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo4_map_current_only(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A03, 0x00, 0), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u32(0x1A03, 0x01, 0x60780010), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1A03, 0x00, 1), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(s_log_tag, "TPDO4 -> 0x6078 Current actual value (16-bit)");
    return ESP_OK;
}

esp_err_t canopen_drive_configure_tpdo4_fast_event(void)
{
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u8(0x1803, 0x02, 0xFF), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1803, 0x03, 200), s_log_tag,
                        "Servo operation failed");
    ESP_RETURN_ON_ERROR(canopen_drive_sdo_write_u16(0x1803, 0x05, 30), s_log_tag,
                        "Servo operation failed");
    ESP_LOGI(
        s_log_tag,
        "TPDO4: transmission=255, inhibit_raw=200 (100us units), event=30ms (drive-dependent)");
    return ESP_OK;
}

esp_err_t canopen_drive_send_controlword(uint16_t cw)
{
    uint8_t d[2] = {(uint8_t)(cw & 0xFF), (uint8_t)(cw >> 8)};
    return can_send(RPDO1_ID, d, 2);
}

esp_err_t canopen_drive_send_target_position(float pos_revs)
{
    if (drive_operation_cancelled())
        return ESP_ERR_INVALID_STATE;
    const float physical_pos_revs = pos_revs + s_position_zero_offset_revs;
    if (!isfinite(physical_pos_revs) || (double)physical_pos_revs * 10000.0 > INT32_MAX - 1.0 ||
        (double)physical_pos_revs * 10000.0 < INT32_MIN + 1.0)
        return ESP_ERR_INVALID_ARG;
    int32_t pos = (int32_t)lroundf(physical_pos_revs * 10000.0f);

    ESP_LOGI(s_log_tag, "POS logical=%.4f physical=%.4f counts=%" PRIi32, pos_revs,
             physical_pos_revs, pos);

    uint8_t d[4] = {
        (uint8_t)(pos & 0xFF),
        (uint8_t)((pos >> 8) & 0xFF),
        (uint8_t)((pos >> 16) & 0xFF),
        (uint8_t)((pos >> 24) & 0xFF),
    };
    return can_send(RPDO2_ID, d, 4);
}

esp_err_t canopen_drive_send_target_position_and_speed(float pos_revs, float speed_rpm)
{
    if (drive_operation_cancelled())
        return ESP_ERR_INVALID_STATE;
    if (!drive_position_allowed(pos_revs, speed_rpm))
        return ESP_ERR_INVALID_ARG;
    const float physical_pos_revs = pos_revs + s_position_zero_offset_revs;
    if (!isfinite(physical_pos_revs) || (double)physical_pos_revs * 10000.0 > INT32_MAX - 1.0 ||
        (double)physical_pos_revs * 10000.0 < INT32_MIN + 1.0)
        return ESP_ERR_INVALID_ARG;
    int32_t pos = (int32_t)lroundf(physical_pos_revs * 10000.0f);
    int32_t speed = (int32_t)lroundf(speed_rpm * 10000.0f / 60.0f);

    ESP_LOGI(s_log_tag, "POS logical=%.4f physical=%.4f counts=%" PRIi32 ", SPEED: %" PRIi32,
             pos_revs, physical_pos_revs, pos, speed);

    uint8_t d[8] = {
        (uint8_t)(pos & 0xFF),           (uint8_t)((pos >> 8) & 0xFF),
        (uint8_t)((pos >> 16) & 0xFF),   (uint8_t)((pos >> 24) & 0xFF),
        (uint8_t)(speed & 0xFF),         (uint8_t)((speed >> 8) & 0xFF),
        (uint8_t)((speed >> 16) & 0xFF), (uint8_t)((speed >> 24) & 0xFF),
    };

    return can_send(RPDO2_ID, d, 8);
}

esp_err_t canopen_drive_send_profile_accel_decel(float accel_rpm_s, float decel_rpm_s)
{
    int32_t accel = (int32_t)lroundf(accel_rpm_s * 10000.0f / 60.0f);
    int32_t decel = (int32_t)lroundf(decel_rpm_s * 10000.0f / 60.0f);

    ESP_LOGI(s_log_tag, "ACCEL: %" PRIi32 ", DECEL: %" PRIi32, accel, decel);

    uint8_t d[8] = {
        (uint8_t)(accel & 0xFF),         (uint8_t)((accel >> 8) & 0xFF),
        (uint8_t)((accel >> 16) & 0xFF), (uint8_t)((accel >> 24) & 0xFF),
        (uint8_t)(decel & 0xFF),         (uint8_t)((decel >> 8) & 0xFF),
        (uint8_t)((decel >> 16) & 0xFF), (uint8_t)((decel >> 24) & 0xFF),
    };

    return can_send(RPDO3_ID, d, 8);
}

esp_err_t canopen_drive_set_actual_position_feedback(float pos_revs)
{
    s_latest_actual_position_physical_revs = pos_revs;
    s_latest_actual_position_revs = pos_revs - s_position_zero_offset_revs;
    return ESP_OK;
}

float canopen_drive_get_actual_position_feedback(void)
{
    return s_latest_actual_position_revs;
}

esp_err_t canopen_drive_capture_current_position_as_zero(void)
{
    s_position_zero_offset_revs = s_latest_actual_position_physical_revs;
    s_latest_actual_position_revs = 0.0f;
    ESP_LOGW(s_log_tag, "Captured new logical zero at physical position %.4f revs",
             s_position_zero_offset_revs);
    return ESP_OK;
}

esp_err_t canopen_drive_set_directional_profile(float accel_up_rpm_s, float decel_up_rpm_s,
                                                float accel_down_rpm_s, float decel_down_rpm_s)
{
    s_accel_up_rpm_s = accel_up_rpm_s;
    s_decel_up_rpm_s = decel_up_rpm_s;
    s_accel_down_rpm_s = accel_down_rpm_s;
    s_decel_down_rpm_s = decel_down_rpm_s;
    return ESP_OK;
}

esp_err_t canopen_drive_send_target_position_speed_directional_profile(float pos_revs,
                                                                       float speed_rpm)
{
    const bool shifting_up = pos_revs > s_latest_actual_position_revs;
    const float accel = shifting_up ? s_accel_up_rpm_s : s_accel_down_rpm_s;
    const float decel = shifting_up ? s_decel_up_rpm_s : s_decel_down_rpm_s;

    ESP_RETURN_ON_ERROR(
        canopen_drive_send_target_position_speed_accel_decel(pos_revs, speed_rpm, accel, decel),
        s_log_tag, "Directional profile move failed");
    return ESP_OK;
}

esp_err_t canopen_drive_send_target_position_speed_accel_decel(float pos_revs, float speed_rpm,
                                                               float accel_rpm_s, float decel_rpm_s)
{
    ESP_RETURN_ON_ERROR(canopen_drive_send_profile_accel_decel(accel_rpm_s, decel_rpm_s), s_log_tag,
                        "RPDO3 accel/decel tx failed");
    return canopen_drive_send_target_position_and_speed(pos_revs, speed_rpm);
}

esp_err_t canopen_drive_enable_operation_sequence_position(void)
{
    const uint16_t seq[] = {0x0006, 0x0007, 0x000F};
    for (int i = 0; i < 3; i++) {
        if (drive_operation_cancelled())
            return ESP_ERR_INVALID_STATE;
        ESP_LOGI(s_log_tag, "RPDO1 Controlword = 0x%04X", seq[i]);
        ESP_RETURN_ON_ERROR(canopen_drive_send_controlword(seq[i]), s_log_tag,
                            "Servo operation failed");
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ESP_OK;
}

esp_err_t canopen_drive_enable_position(void)
{
    const uint16_t seq[] = {0x002F, 0x003F};
    for (int i = 0; i < 2; i++) {
        if (drive_operation_cancelled())
            return ESP_ERR_INVALID_STATE;
        ESP_LOGI(s_log_tag, "RPDO1 Controlword = 0x%04X", seq[i]);
        ESP_RETURN_ON_ERROR(canopen_drive_send_controlword(seq[i]), s_log_tag,
                            "Servo operation failed");
    }
    return ESP_OK;
}
