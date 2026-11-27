#include "parameters.h"
#include "runtime.h"
#include "servo/ECVT_motor.h"
#include "esp_timer.h"

esp_err_t motor_read_object(const MotorObject &object, MotorObjectResult &result)
{
    result = {}; result.object = object; result.object.value = 0;
    uint8_t byte = 0; uint16_t word = 0; uint32_t value = 0;
    switch (object.width) {
    case ObjectWidth::U8: result.error = ecvt_motor_sdo_read_u8(object.index, object.subindex, &byte); value=byte; break;
    case ObjectWidth::U16: result.error = ecvt_motor_sdo_read_u16(object.index, object.subindex, &word); value=word; break;
    case ObjectWidth::U32: result.error = ecvt_motor_sdo_read_u32(object.index, object.subindex, &value); break;
    default: result.error = ESP_ERR_INVALID_ARG; break;
    }
    if (result.error == ESP_OK) result.object.value = value;
    result.sampled_us = esp_timer_get_time();
    return result.error;
}

esp_err_t motor_write_object(const MotorObject &object, MotorObjectResult &result)
{
    result = {}; result.object = object;
    // State, motion and persistent-store/reset commands have dedicated paths;
    // parameter access must not bypass operation ownership and stop handling.
    if (object.index==0x6040 || object.index==0x6060 || object.index==0x6071 ||
        object.index==0x607A || object.index==0x60FF || object.index==0x1010 || object.index==0x1011) {
        result.error=ESP_ERR_INVALID_ARG;
        result.sampled_us=esp_timer_get_time();
        return result.error;
    }
    // Explicit single-object writes; no automatic EEPROM save or factory reset.
    // Retain full-width values and reject overflow rather than truncating.
    switch (object.width) {
    case ObjectWidth::U8:
        result.error = object.value <= UINT8_MAX ? ecvt_motor_sdo_write_u8(object.index, object.subindex, object.value) : ESP_ERR_INVALID_ARG; break;
    case ObjectWidth::U16:
        result.error = object.value <= UINT16_MAX ? ecvt_motor_sdo_write_u16(object.index, object.subindex, object.value) : ESP_ERR_INVALID_ARG; break;
    case ObjectWidth::U32: result.error = ecvt_motor_sdo_write_u32(object.index, object.subindex, object.value); break;
    default: result.error = ESP_ERR_INVALID_ARG; break;
    }
    result.sampled_us = esp_timer_get_time();
    return result.error;
}

esp_err_t motor_read_snapshot(const MotorObject *objects, unsigned count, MotorResult &result)
{
    result.object_count = 0;
    if (!objects || count == 0 || count > MOTOR_SNAPSHOT_CAPACITY) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ESP_OK;
    result.object_count = 0;
    for (unsigned i=0; i<count; ++i) {
        if (motor_operation_cancelled()) return ESP_ERR_INVALID_STATE;
        const esp_err_t read = motor_read_object(objects[i], result.objects[i]);
        ++result.object_count;
        if (error == ESP_OK && read != ESP_OK) error = read;
    }
    return error;
}
