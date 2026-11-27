// Single task owning drive transactions, operation admission and completion.
// Stop invalidates queued work and reaches the unconditional disable path.
#include "control.h"
#include "control_policy.h"
#include "operation_context.h"
#include "operations.h"
#include "canopen/drive.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cinttypes>
#include <cmath>
#include <initializer_list>

namespace {
// Read one exact-width object; a failed read remains an error, never a valid zero.
esp_err_t read_object(const DriveObject &object, DriveObjectResult &result)
{
    result = {};
    result.object = object;
    result.object.value = 0;
    uint8_t byte = 0;
    uint16_t word = 0;
    uint32_t value = 0;
    switch (object.width) {
    case ObjectWidth::U8:
        result.error = canopen_drive_sdo_read_u8(object.index, object.subindex, &byte);
        value = byte;
        break;
    case ObjectWidth::U16:
        result.error = canopen_drive_sdo_read_u16(object.index, object.subindex, &word);
        value = word;
        break;
    case ObjectWidth::U32:
        result.error = canopen_drive_sdo_read_u32(object.index, object.subindex, &value);
        break;
    default:
        result.error = ESP_ERR_INVALID_ARG;
        break;
    }
    if (result.error == ESP_OK)
        result.object.value = value;
    result.sampled_us = esp_timer_get_time();
    return result.error;
}

// Generic parameter write; state/motion and EEPROM commands stay dedicated.
esp_err_t write_object(const DriveObject &object, DriveObjectResult &result)
{
    result = {};
    result.object = object;
    // State, motion and persistent-store/reset commands have dedicated paths;
    // parameter access must not bypass operation ownership and stop handling.
    if (object.index == 0x6040 || object.index == 0x6060 || object.index == 0x6071 ||
        object.index == 0x607A || object.index == 0x60FF || object.index == 0x1010 ||
        object.index == 0x1011) {
        result.error = ESP_ERR_INVALID_ARG;
        result.sampled_us = esp_timer_get_time();
        return result.error;
    }
    // Explicit single-object writes; no automatic EEPROM save or factory reset.
    // Retain full-width values and reject overflow rather than truncating.
    switch (object.width) {
    case ObjectWidth::U8:
        result.error = object.value <= UINT8_MAX
                           ? canopen_drive_sdo_write_u8(object.index, object.subindex, object.value)
                           : ESP_ERR_INVALID_ARG;
        break;
    case ObjectWidth::U16:
        result.error =
            object.value <= UINT16_MAX
                ? canopen_drive_sdo_write_u16(object.index, object.subindex, object.value)
                : ESP_ERR_INVALID_ARG;
        break;
    case ObjectWidth::U32:
        result.error = canopen_drive_sdo_write_u32(object.index, object.subindex, object.value);
        break;
    default:
        result.error = ESP_ERR_INVALID_ARG;
        break;
    }
    result.sampled_us = esp_timer_get_time();
    return result.error;
}

// Sequential, cancellable reads with per-object errors and sampling timestamps.
esp_err_t read_snapshot(const DriveObject *objects, unsigned count, DriveResult &result)
{
    result.object_count = 0;
    if (!objects || count == 0 || count > DRIVE_SNAPSHOT_CAPACITY)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = ESP_OK;
    result.object_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (drive_operation_cancelled())
            return ESP_ERR_INVALID_STATE;
        const esp_err_t read = read_object(objects[i], result.objects[i]);
        ++result.object_count;
        if (error == ESP_OK && read != ESP_OK)
            error = read;
    }
    return error;
}

using RunState = DriveRunState;
struct PendingCommand {
    DriveRequest command;
    unsigned epoch;
    uint32_t id;
};
QueueHandle_t commands;
OperationGate gate;
auto &stop_epoch = gate.stop_epoch;
auto &active_epoch = gate.active_epoch;
auto &busy = gate.busy;
std::atomic<RunState> state{RunState::Idle};
std::atomic<esp_err_t> last_error{ESP_OK};
DriveObserver observer;
constexpr uint8_t NODE_ID = 1;
std::atomic<uint32_t> next_id{1}, stop_id{0}, active_id{0}, queued_id{0};
std::atomic<DriveCommand> queued_kind{DriveCommand::Stop};
DriveResult active_result, last_result;
esp_err_t last_disable_error = ESP_OK;
portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;

// Publish a copy under the status lock, then notify without holding that lock.
void publish(DriveResult result)
{
    result.completed_us = esp_timer_get_time();
    portENTER_CRITICAL(&status_mux);
    last_result = result;
    portEXIT_CRITICAL(&status_mux);
    if (observer.completed)
        observer.completed(observer.context, result);
}
// Repeated pending stops share one receipt; report the disable result once.
void publish_pending_stop(esp_err_t error)
{
    portENTER_CRITICAL(&status_mux);
    const uint32_t id = stop_id.exchange(0);
    portEXIT_CRITICAL(&status_mux);
    if (!id)
        return;
    DriveResult result;
    result.id = id;
    result.command = DriveCommand::Stop;
    result.error = error;
    publish(result);
}
bool drive_touched = false;
MotionLimits limits;
float profile_accel = 2300.0f, profile_decel = 2300.0f;

// Disable first, discard queued work and acknowledge cancellation before admission reopens.
esp_err_t finish_operation()
{
    // Disable first, even after an MCU-only reboot or partial setup. Do not
    // wait for mode/velocity/feedback checks to send it.
    const esp_err_t result = drive_disable();
    last_disable_error = result;
    const bool confirmed = result == ESP_OK;
    if (confirmed)
        drive_touched = false;
    else
        last_error.store(result);
    state.store(confirmed ? RunState::Idle : RunState::DisableUnconfirmed);
    xQueueReset(commands);
    const uint32_t cancelled_id = queued_id.exchange(0);
    if (cancelled_id) {
        DriveResult cancelled;
        cancelled.id = cancelled_id;
        cancelled.command = queued_kind.load();
        cancelled.error = ESP_ERR_INVALID_STATE;
        cancelled.cancelled = true;
        publish(cancelled);
    }
    active_epoch.store(stop_epoch.load());
    puts(confirmed ? "Drive disabled; explicit start required. Standstill/brake lock UNVERIFIED."
                   : "DISABLE UNCONFIRMED; retry stop. No automatic restart.");
    busy.store(false);
    return result;
}

// Prepare and run one absolute move; every attempted setup ends through disable.
esp_err_t run_motion(const DriveRequest &command)
{
    float position = command.values[0], rpm = command.values[1];
    float accel = profile_accel, decel = profile_decel;
    if (command.kind != DriveCommand::Move)
        return ESP_ERR_INVALID_ARG;
    if (!within_limits(limits, position, rpm)) {
        puts("Movement rejected: configure explicit limits covering the requested operation.");
        return ESP_ERR_INVALID_STATE;
    }
    state.store(RunState::Busy);
    esp_err_t result = drive_prepare_position(
        {profile_accel, profile_decel, limits.max_rpm, NODE_ID}, drive_touched);
    if (result == ESP_OK && !drive_operation_cancelled()) {
        result = drive_move_to(position, rpm, accel, decel);
    }
    if (result != ESP_OK && !drive_operation_cancelled()) {
        last_error.store(result);
        printf("Operation ended with %s\n", esp_err_to_name(result));
    }
    // End normal completion as well as cancellation before finishing the owner.
    active_result.cancelled = drive_operation_cancelled();
    stop_epoch.fetch_add(1);
    const esp_err_t disabled = finish_operation();
    return result != ESP_OK ? result : disabled;
}

// Sole drive transaction owner. Check cancellation before consuming queued work and
// after each operation; diagnostics also use this task to prevent overlapping SDOs.
void worker(void *)
{
    while (true) {
        if (drive_operation_cancelled()) {
            publish_pending_stop(finish_operation());
            continue;
        }
        PendingCommand pending;
        if (xQueueReceive(commands, &pending, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        if (pending.epoch != stop_epoch.load()) {
            publish_pending_stop(finish_operation());
            continue;
        }
        queued_id.store(0);
        active_epoch.store(pending.epoch);
        const auto &command = pending.command;
        active_id.store(pending.id);
        active_result = {};
        active_result.id = pending.id;
        active_result.command = command.kind;
        esp_err_t outcome = ESP_OK;
        auto note = [&](esp_err_t error) {
            if (error != ESP_OK)
                outcome = error;
            return error;
        };
        if (drive_disable_pending() && command.kind != DriveCommand::BrakeRestore &&
            command.kind != DriveCommand::ServoPing && command.kind != DriveCommand::ServoInspect &&
            command.kind != DriveCommand::ServoErrors && command.kind != DriveCommand::ServoRead &&
            command.kind != DriveCommand::ParameterRead &&
            command.kind != DriveCommand::ParameterSnapshot) {
            puts("Brake-test disable unconfirmed: use servo brake-restore; motion/settings "
                 "refused.");
            active_id.store(0);
            busy.store(false);
            active_result.error = ESP_ERR_INVALID_STATE;
            publish(active_result);
            continue;
        }
        if (state.load() == RunState::DisableUnconfirmed &&
            command.kind != DriveCommand::ServoPing && command.kind != DriveCommand::ServoInspect &&
            command.kind != DriveCommand::ServoErrors && command.kind != DriveCommand::ServoRead &&
            command.kind != DriveCommand::ParameterRead &&
            command.kind != DriveCommand::ParameterSnapshot &&
            command.kind != DriveCommand::BrakeRestore &&
            command.kind != DriveCommand::ServoControlword) {
            puts("Stop unconfirmed: use stop to retry; mode changes/motion refused.");
            active_id.store(0);
            busy.store(false);
            active_result.error = ESP_ERR_INVALID_STATE;
            publish(active_result);
            continue;
        }
        switch (command.kind) {
        case DriveCommand::ServoVelocityZero: {
            const esp_err_t result = note(drive_prepare_velocity_zero());
            printf("Velocity-zero preparation: %s. No automatic state sequence or speed run.\n",
                   esp_err_to_name(result));
            break;
        }
        case DriveCommand::ServoTorqueZeroTest: {
            const esp_err_t result = note(drive_test_zero_torque(drive_touched));
            last_error.store(result);
            printf("Torque-zero brake test: %s.\n", esp_err_to_name(result));
            break;
        }
        case DriveCommand::ServoPositionMode: {
            const esp_err_t result = note(drive_prepare_position_mode());
            printf("Position-mode preparation: %s. No target or enable command sent.\n",
                   esp_err_to_name(result));
            break;
        }
        case DriveCommand::ServoControlword: {
            const auto word = static_cast<uint16_t>(command.values[0]);
            // Normally handled at submission; retain the unconditional path
            // here too so a queued disable can never acquire enable guards.
            if (word == 0) {
                outcome = finish_operation();
                break;
            }
            uint16_t before = 0, after = 0;
            esp_err_t result = note(canopen_drive_sdo_read_u16(0x6041, 0, &before));
            // Nonzero state steps remain supervised. The 0000 path above
            // bypasses these start-only restrictions.
            if (result == ESP_OK && (before & 0x0004))
                result = note(ESP_ERR_INVALID_STATE);
            if (result == ESP_OK && word == 0x000F) {
                uint8_t mode = 0, errors = 0;
                uint32_t target_velocity = 1;
                result = note(canopen_drive_sdo_read_u8(0x6061, 0, &mode));
                if (result == ESP_OK)
                    result = note(canopen_drive_sdo_read_u8(0x1001, 0, &errors));
                if (result == ESP_OK && mode == 3 && errors == 0)
                    result = note(canopen_drive_sdo_write_u32(0x60FF, 0, 0));
                if (result == ESP_OK && mode == 3)
                    result = note(canopen_drive_sdo_read_u32(0x60FF, 0, &target_velocity));
                if (result == ESP_OK &&
                    !direct_bench_enable_allowed(before, mode, errors, target_velocity))
                    result = note(ESP_ERR_INVALID_STATE);
                if (result != ESP_OK)
                    puts("Direct enable refused: requires switched-on state, no drive error, and "
                         "position mode 1 or zero-velocity mode 3.");
                else if (mode == 1)
                    puts("Explicit position-mode state step; existing position/trajectory "
                         "registers remain unchanged.");
            }
            if (result != ESP_OK) {
                printf("Single controlword 0x%04x NOT SENT: %s; status_before=0x%04x.\n", word,
                       esp_err_to_name(result), before);
                break;
            }
            result = note(canopen_drive_sdo_write_u16(0x6040, 0, word));
            printf("ONE controlword 6040:00=0x%04x: %s. No automatic next controlword or target.\n",
                   word, esp_err_to_name(result));
            if (result == ESP_OK) {
                if (word == 0x000F) {
                    drive_touched = true;
                    state.store(RunState::Enabled);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                const esp_err_t read = note(canopen_drive_sdo_read_u16(0x6041, 0, &after));
                if (read == ESP_OK)
                    printf(
                        "Status after single controlword: 0x%04x operation_enabled=%u fault=%u.\n",
                        after, (after >> 2) & 1, (after >> 3) & 1);
                else
                    printf("Status read failed: %s.\n", esp_err_to_name(read));
            }
            break;
        }
        case DriveCommand::ServoRead:
        case DriveCommand::ParameterRead: {
            DriveObject object = command.objects[0];
            outcome = read_object(object, active_result.objects[0]);
            active_result.object_count = 1;
            const uint32_t value = active_result.objects[0].object.value;
            if (outcome == ESP_OK)
                printf("Read-only 0x%04x:%02x u%u = 0x%08" PRIx32 " (%" PRIu32 ")\n", object.index,
                       object.subindex, unsigned(object.width) * 8, value, value);
            else
                printf("Read-only 0x%04x:%02x u%u failed: %s\n", object.index, object.subindex,
                       unsigned(object.width) * 8, esp_err_to_name(outcome));
            break;
        }
        case DriveCommand::ParameterWrite:
            outcome = write_object(command.objects[0], active_result.objects[0]);
            active_result.object_count = 1;
            break;
        case DriveCommand::ParameterSnapshot:
            outcome = read_snapshot(command.objects, command.object_count, active_result);
            break;
        case DriveCommand::ServoResetFault: {
            if (drive_touched || state.load() != RunState::Idle) {
                outcome = ESP_ERR_INVALID_STATE;
                puts("Fault reset requires idle manual bench mode before motion this boot; no "
                     "reset sent.");
                break;
            }
            uint16_t word = 0;
            esp_err_t result = note(canopen_drive_sdo_read_u16(0x6041, 0, &word));
            if (result == ESP_OK && !(word & 0x0008)) {
                puts("Drive reports no fault; no reset sent.");
                break;
            }
            if (result == ESP_OK) {
                result = note(canopen_drive_sdo_write_u16(0x6040, 0, 0x0080));
                // A timed-out write may still take effect: lower the reset bit
                // after every attempt, including error/cancellation paths.
                vTaskDelay(pdMS_TO_TICKS(100));
                const esp_err_t clear = note(canopen_drive_sdo_write_u16(0x6040, 0, 0));
                if (clear != ESP_OK)
                    result = note(clear);
            }
            if (result == ESP_OK)
                result = note(canopen_drive_sdo_read_u16(0x6041, 0, &word));
            if (result == ESP_OK && (word & 0x004F) != 0x0040)
                result = note(ESP_ERR_INVALID_STATE);
            last_error.store(result);
            printf("Fault reset: %s statusword=0x%04x; no enable or target command sent.\n",
                   esp_err_to_name(result), word);
            break;
        }
        case DriveCommand::ServoErrors: {
            puts("Read-only drive errors; no fault reset, controlword or history clear.");
            uint16_t code = 0;
            esp_err_t result = note(canopen_drive_sdo_read_u16(0x603F, 0, &code));
            if (result == ESP_OK)
                printf("0x603f:00 error_code=0x%04x (%u)\n", code, code);
            else
                printf("0x603f:00 read failed: %s\n", esp_err_to_name(result));
            uint8_t error_register = 0;
            uint16_t count = 0;
            result = note(canopen_drive_sdo_read_u8(0x1001, 0, &error_register));
            if (result == ESP_OK)
                printf("0x1001:00 error_register=0x%02x\n", error_register);
            else
                printf("0x1001:00 read failed: %s\n", esp_err_to_name(result));
            result = canopen_drive_sdo_read_u16(0x1003, 0, &count);
            if (result != ESP_OK) {
                // This bench drive advertises u8 in its EDS but replies u16.
                // Keep strict width checking for other object reads.
                uint8_t count8 = 0;
                result = canopen_drive_sdo_read_u8(0x1003, 0, &count8);
                if (result == ESP_OK)
                    count = count8;
            }
            note(result);
            if (result == ESP_OK)
                printf("0x1003:00 error_history_count=%u\n", count);
            else
                printf("0x1003:00 read failed: %s\n", esp_err_to_name(result));
            for (uint8_t sub = 1;
                 result == ESP_OK && sub <= count && sub <= 5 && !drive_operation_cancelled();
                 ++sub) {
                uint32_t entry = 0;
                result = note(canopen_drive_sdo_read_u32(0x1003, sub, &entry));
                if (result == ESP_OK)
                    printf("0x1003:%02x error_history=0x%08" PRIx32 "\n", sub, entry);
                else
                    printf("0x1003:%02x read failed: %s\n", sub, esp_err_to_name(result));
            }
            uint32_t voltage = 0;
            result = note(canopen_drive_sdo_read_u32(0x6079, 0, &voltage));
            if (result == ESP_OK)
                printf("0x6079:00 dc_link_voltage_raw=%" PRIu32 "\n", voltage);
            else
                printf("0x6079:00 read failed: %s\n", esp_err_to_name(result));
            break;
        }
        case DriveCommand::BrakeProbe:
        case DriveCommand::BrakeRestore: {
            if (command.kind == DriveCommand::BrakeProbe &&
                (drive_touched || state.load() != RunState::Idle)) {
                outcome = ESP_ERR_INVALID_STATE;
                puts("Brake test requires idle manual mode before motion; inspect first.");
                break;
            }
            const bool had_pending_disable = drive_disable_pending();
            const esp_err_t result = note(
                command.kind == DriveCommand::BrakeProbe ? drive_test_brake() : drive_disable());
            last_error.store(result);
            if (command.kind == DriveCommand::BrakeProbe || had_pending_disable)
                state.store(drive_disable_pending() ? RunState::DisableUnconfirmed
                                                    : RunState::Idle);
            printf("Brake state test: %s; disable=%s; mechanical_brake=UNVERIFIED\n",
                   esp_err_to_name(result),
                   drive_disable_pending() ? "UNCONFIRMED: use servo brake-restore"
                                           : "no pending disable");
            break;
        }
        case DriveCommand::ServoInspect: {
            puts("Read-only servo inspection; no controlword, enable or output writes.");
            uint16_t statusword = 0;
            esp_err_t result = note(canopen_drive_sdo_read_u16(0x6041, 0, &statusword));
            if (result == ESP_OK)
                printf("0x6041:00 statusword=0x%04x operation_enabled=%u fault=%u (drive report; "
                       "brake unverified)\n",
                       statusword, (statusword >> 2) & 1, (statusword >> 3) & 1);
            else
                printf("0x6041:00 read failed: %s\n", esp_err_to_name(result));
            const uint16_t indices[] = {0x6064, 0x606C, 0x608F, 0x608F, 0x6091,
                                        0x6091, 0x6093, 0x6093, 0x60FE, 0x60FE};
            const uint8_t subs[] = {0, 0, 1, 2, 1, 2, 1, 2, 1, 2};
            const char *labels[] = {"position_user_units",       "velocity_raw",
                                    "encoder_increments",        "encoder_motor_revs",
                                    "gear_motor_revs",           "gear_shaft_revs",
                                    "position_factor_numerator", "position_factor_denominator",
                                    "outputs_physical",          "outputs_mask"};
            for (unsigned i = 0;
                 i < sizeof(indices) / sizeof(indices[0]) && !drive_operation_cancelled(); ++i) {
                uint32_t value = 0;
                result = note(canopen_drive_sdo_read_u32(indices[i], subs[i], &value));
                if (result == ESP_OK) {
                    if (indices[i] == 0x6064 || indices[i] == 0x606C)
                        printf("0x%04x:%02x %s=%" PRId32 " (0x%08" PRIx32 ")\n", indices[i],
                               subs[i], labels[i], static_cast<int32_t>(value), value);
                    else
                        printf("0x%04x:%02x %s=%" PRIu32 " (0x%08" PRIx32 ")\n", indices[i],
                               subs[i], labels[i], value, value);
                } else
                    printf("0x%04x:%02x read failed: %s\n", indices[i], subs[i],
                           esp_err_to_name(result));
            }
            puts("Inspection finished. Output bits do not confirm mechanical brake state.");
            break;
        }
        case DriveCommand::ServoPing: {
            // Intentionally no preparation, NMT, controlword, PDO remapping,
            // drive_touched mutation or automatic stop on probe failure.
            uint8_t mode = 0;
            const esp_err_t result = note(canopen_drive_sdo_read_u8(0x6061, 0, &mode));
            if (result == ESP_OK) {
                printf("Servo replied: node=%u object=0x6061:00 mode=%d. Read-only; no "
                       "enable/motion command sent.\n",
                       NODE_ID, static_cast<int>(static_cast<int8_t>(mode)));
            } else {
                printf("Servo ping failed: %s. No valid fresh reply; check drive power, node ID, "
                       "bitrate and CAN wiring.\n",
                       esp_err_to_name(result));
            }
            break;
        }
        case DriveCommand::ModeManual:
            puts("Manual mode selected; no motion started.");
            break;
        case DriveCommand::Limits:
            if (valid_limits(command.values[0], command.values[1], command.values[2])) {
                portENTER_CRITICAL(&status_mux);
                limits = {true, command.values[0], command.values[1], command.values[2]};
                portEXIT_CRITICAL(&status_mux);
                printf("Limits: %.4f .. %.4f logical rev; max %.2f rpm\n", limits.min_revs,
                       limits.max_revs, limits.max_rpm);
            } else {
                outcome = ESP_ERR_INVALID_ARG;
                puts("Invalid limits.");
            }
            break;
        case DriveCommand::Profile:
            if (command.values[0] > 0 && command.values[1] > 0 && command.values[0] <= 12884900 &&
                command.values[1] <= 12884900) {
                portENTER_CRITICAL(&status_mux);
                profile_accel = command.values[0];
                profile_decel = command.values[1];
                portEXIT_CRITICAL(&status_mux);
                puts("Manual motion profile updated (rpm/s).");
            } else {
                outcome = ESP_ERR_INVALID_ARG;
                puts("Profile values must be positive and serializable.");
            }
            break;
        default:
            outcome = run_motion(command);
            break;
        }
        active_result.error = outcome;
        active_result.cancelled = active_result.cancelled || drive_operation_cancelled();
        if (outcome != ESP_OK && !active_result.cancelled)
            last_error.store(outcome);
        active_id.store(0);
        busy.store(false);
        publish(active_result);
        if (drive_operation_cancelled())
            publish_pending_stop(finish_operation());
        else
            publish_pending_stop(last_disable_error);
    }
}
} // namespace

extern "C" bool drive_operation_cancelled()
{
    return gate.cancelled();
}

extern "C" void drive_operation_error(esp_err_t error)
{
    last_error.store(error);
    stop_epoch.fetch_add(1);
}

extern "C" bool drive_position_allowed(float position, float rpm)
{
    return within_limits(limits, position, rpm);
}

bool drive_operation_wait_ms(unsigned milliseconds)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(milliseconds) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (drive_operation_cancelled())
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !drive_operation_cancelled();
}

DriveStatus drive_control_status()
{
    DriveStatus snapshot;
    snapshot.state = state.load();
    snapshot.busy = busy.load();
    snapshot.active_request_id = active_id.load();
    if (!snapshot.active_request_id)
        snapshot.active_request_id = queued_id.load();
    snapshot.disable_pending = drive_disable_pending();
    snapshot.error = last_error.load();
    snapshot.feedback = drive_feedback_snapshot();
    portENTER_CRITICAL(&status_mux);
    snapshot.limits_configured = limits.configured;
    snapshot.minimum_revs = limits.min_revs;
    snapshot.maximum_revs = limits.max_revs;
    snapshot.maximum_rpm = limits.max_rpm;
    snapshot.acceleration_rpm_s = profile_accel;
    snapshot.deceleration_rpm_s = profile_decel;
    snapshot.last_result = last_result;
    portEXIT_CRITICAL(&status_mux);
    return snapshot;
}

DriveReceipt drive_control_submit(const DriveRequest &command)
{
    if (!commands)
        return {ESP_ERR_INVALID_STATE, 0};
    if (command.kind == DriveCommand::Stop ||
        (command.kind == DriveCommand::ServoControlword && command.values[0] == 0)) {
        portENTER_CRITICAL(&status_mux);
        uint32_t id = stop_id.load();
        if (!id) {
            id = next_id.fetch_add(1);
            stop_id.store(id);
        }
        gate.stop();
        portEXIT_CRITICAL(&status_mux);
        puts("Disable requested (6040=0x0000).");
        return {ESP_OK, id};
    }
    if (static_cast<unsigned>(command.kind) >
        static_cast<unsigned>(DriveCommand::ParameterSnapshot))
        return {ESP_ERR_INVALID_ARG, 0};
    for (float value : command.values)
        if (!std::isfinite(value))
            return {ESP_ERR_INVALID_ARG, 0};
    if ((command.kind == DriveCommand::ParameterRead ||
         command.kind == DriveCommand::ParameterWrite || command.kind == DriveCommand::ServoRead) &&
        command.object_count != 1)
        return {ESP_ERR_INVALID_ARG, 0};
    if (command.kind == DriveCommand::ParameterSnapshot &&
        (!command.object_count || command.object_count > DRIVE_SNAPSHOT_CAPACITY))
        return {ESP_ERR_INVALID_ARG, 0};
    const unsigned epoch = stop_epoch.load();
    if (!gate.admit()) {
        puts("Operation in progress; use stop before changing mode or starting another operation.");
        return {ESP_ERR_INVALID_STATE, 0};
    }
    if (epoch != stop_epoch.load()) {
        busy.store(false);
        return {ESP_ERR_INVALID_STATE, 0};
    }
    const uint32_t id = next_id.fetch_add(1);
    queued_kind.store(command.kind);
    queued_id.store(id);
    PendingCommand pending{command, epoch, id};
    if (xQueueSend(commands, &pending, 0) != pdTRUE) {
        queued_id.store(0);
        busy.store(false);
        puts("Command not queued.");
        return {ESP_ERR_NO_MEM, 0};
    }
    return {ESP_OK, id};
}

esp_err_t drive_control_start(const DriveObserver &configuration)
{
    if (commands)
        return ESP_ERR_INVALID_STATE;
    observer = configuration;
    commands = xQueueCreate(1, sizeof(PendingCommand));
    if (!commands)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "drive_owner", 12288, nullptr, 6, nullptr) != pdPASS) {
        vQueueDelete(commands);
        commands = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
