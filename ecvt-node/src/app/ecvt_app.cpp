#include "ecvt_app.h"
#include "operating_policy.h"
#include "operation_gate.h"
#include "app/modules_internal.h"
#include "console/serial_console.h"
#include "experiments/motor_tests.h"
#include "experiments/brake_probe.h"
#include "experiments/three_revolutions.h"
#include "servo/servo_feedback.h"
#include "servo/velocity_zero.h"
#include <atomic>

namespace {
enum class RunState { Idle, Busy, Holding, StopUnconfirmed };
struct PendingCommand { ConsoleCommand command; unsigned epoch; };
QueueHandle_t commands;
OperationGate gate;
auto &stop_epoch = gate.stop_epoch;
auto &active_epoch = gate.active_epoch;
auto &busy = gate.busy;
std::atomic<bool> automatic{false};
std::atomic<RunState> state{RunState::Idle};
std::atomic<esp_err_t> last_error{ESP_OK};
bool sd_available;
bool drive_touched = false;
MotionLimits limits;
bool limits_explicit = false;
float profile_accel = 2300.0f, profile_decel = 2300.0f;
std::atomic<bool> replay_alive{false};
constexpr int64_t torque_zero_window_us = 20000000;

esp_err_t wait_for_drive_state(uint16_t expected, uint16_t controlword)
{
    const int64_t deadline = esp_timer_get_time() + 1000000;
    uint16_t actual = 0;
    while (esp_timer_get_time() < deadline) {
        if (ecvt_app_stop_requested()) return ESP_ERR_INVALID_STATE;
        const esp_err_t error = ecvt_motor_sdo_read_u16(0x6041, 0, &actual);
        if (error != ESP_OK) return error;
        if (actual & 0x0008) {
            printf("Enable stopped: drive fault after controlword=0x%04x statusword=0x%04x.\n", controlword, actual);
            return ESP_FAIL;
        }
        if ((actual & 0x006F) == expected) {
            printf("Drive transition confirmed: controlword=0x%04x statusword=0x%04x.\n", controlword, actual);
            return ESP_OK;
        }
        if (!ecvt_app_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    }
    printf("Enable stopped: controlword=0x%04x statusword=0x%04x; expected state=0x%04x (mask 0x006F). No target trigger sent.\n",
           controlword, actual, expected);
    return ESP_ERR_TIMEOUT;
}

// This is a bounded brake-voltage observation. Zero is the only torque target
// it can send, and cleanup writes zero again before disabling the drive.
esp_err_t torque_zero_brake_test()
{
    uint16_t status = 0, target = 1;
    uint8_t errors = 0, mode = 0;
    esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u8(0x1001, 0, &errors);
    if (result != ESP_OK || (status & 0x004F) != 0x0040 || errors != 0) {
        printf("Torque-zero test refused: status=0x%04x error_register=0x%02x; requires disabled, alarm-free drive.\n", status, errors);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    drive_touched = true;
    result = ecvt_motor_sdo_write_u16(0x6071, 0, 0);
    if (result == ESP_OK) result = ecvt_motor_sdo_write_u8(0x6060, 0, 4);
    const int64_t mode_deadline = esp_timer_get_time() + 500000;
    while (result == ESP_OK && esp_timer_get_time() < mode_deadline) {
        result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
        if (result != ESP_OK || mode == 4) break;
        if (!ecvt_app_wait_ms(20)) { result = ESP_ERR_INVALID_STATE; break; }
    }
    if (result == ESP_OK && mode != 4) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u16(0x6071, 0, &target);
    if (result == ESP_OK && target != 0) result = ESP_FAIL;
    for (uint16_t word : {uint16_t(0x0006), uint16_t(0x0007), uint16_t(0x000F)}) {
        if (result != ESP_OK || ecvt_app_stop_requested()) {
            if (result == ESP_OK) result = ESP_ERR_INVALID_STATE;
            break;
        }
        result = ecvt_motor_sdo_write_u16(0x6040, 0, word);
        if (result == ESP_OK) result = wait_for_drive_state(word == 0x0006 ? 0x0021 : word == 0x0007 ? 0x0023 : 0x0027, word);
    }
    int64_t window_started = 0;
    if (result == ESP_OK) {
        puts("ZERO-TORQUE BRAKE WINDOW: measure BRK+/BRK- now; target torque=0, duration 20 seconds.");
        window_started = esp_timer_get_time();
        const int64_t deadline = window_started + torque_zero_window_us;
        while (esp_timer_get_time() < deadline && !ecvt_app_stop_requested()) {
            result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
            if (result != ESP_OK || (status & 0x006F) != 0x0027 || (status & 0x0008)) {
                if (result == ESP_OK) {
                    printf("Torque-zero window ended early: statusword=0x%04x.\n", status);
                    result = ESP_FAIL;
                }
                break;
            }
            result = ecvt_motor_sdo_read_u16(0x6071, 0, &target);
            if (result != ESP_OK || target != 0) { if (result == ESP_OK) result = ESP_FAIL; break; }
            if (!ecvt_app_wait_ms(50)) { result = ESP_ERR_INVALID_STATE; break; }
        }
    }
    const esp_err_t zero_result = ecvt_motor_sdo_write_u16(0x6071, 0, 0);
    const esp_err_t disable_result = ecvt_motor_sdo_write_u16(0x6040, 0, 0);
    if (result == ESP_OK && zero_result != ESP_OK) result = zero_result;
    if (result == ESP_OK && disable_result != ESP_OK) result = disable_result;
    const int64_t elapsed_ms = window_started ? (esp_timer_get_time() - window_started) / 1000 : 0;
    printf("Torque-zero test cleanup: window_elapsed_ms=%lld target torque zero=%s disable=%s.\n",
           static_cast<long long>(elapsed_ms), esp_err_to_name(zero_result), esp_err_to_name(disable_result));
    return result;
}

esp_err_t servo_prepare_position_mode()
{
    uint16_t status = 0;
    uint8_t errors = 0, mode = 0;
    esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &status);
    if (result == ESP_OK) result = ecvt_motor_sdo_read_u8(0x1001, 0, &errors);
    if (result != ESP_OK || (status & 0x004F) != 0x0040 || errors != 0) {
        printf("Position-mode preparation refused: status=0x%04x error_register=0x%02x; requires disabled drive without alarm.\n", status, errors);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    result = ecvt_motor_sdo_write_u8(0x6060, 0, 1);
    const int64_t deadline = esp_timer_get_time() + 500000;
    while (result == ESP_OK && esp_timer_get_time() < deadline) {
        result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
        if (result != ESP_OK || mode == 1) break;
        if (!ecvt_app_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK && mode != 1) result = ESP_ERR_TIMEOUT;
    if (result == ESP_OK) puts("Position mode verified: mode=1. No target or controlword was sent; any stored position target remains unchanged.");
    return result;
}

esp_err_t wait_for_hold_target_ack(bool high)
{
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < deadline) {
        if (ecvt_app_stop_requested()) return ESP_ERR_INVALID_STATE;
        uint16_t word = 0;
        const esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &word);
        if (result != ESP_OK) return result;
        if ((word & 0x006F) != 0x0027) return ESP_ERR_INVALID_STATE;
        if (static_cast<bool>(word & 0x1000) == high) return ESP_OK;
        if (!ecvt_app_wait_ms(20)) return ESP_ERR_INVALID_STATE;
    }
    puts("Current-position hold target was not acknowledged; Halt remains set, move not triggered.");
    return ESP_ERR_TIMEOUT;
}

esp_err_t prepare_position(bool brake_release_wait)
{
    drive_touched = true;
#define PREPARE(call) do { if (ecvt_app_stop_requested()) return ESP_ERR_INVALID_STATE; \
    esp_err_t result = (call); if (result != ESP_OK) return result; } while (0)
    PREPARE(ecvt_motor_nmt_send(0x80, NODE_ID));
    if (!ecvt_app_wait_ms(100)) return ESP_ERR_INVALID_STATE;
    PREPARE(ecvt_motor_configure_rpdo1_map_controlword_only());
    PREPARE(ecvt_motor_configure_rpdo2());
    PREPARE(ecvt_motor_configure_rpdo3_map_profile_accel_decel());
    PREPARE(ecvt_motor_configure_tpdo1_map_statusword_only());
    // Confirmation needs periodic status, not only status-change events.
    PREPARE(ecvt_motor_sdo_write_u8(0x1800, 2, 0xFF));
    PREPARE(ecvt_motor_sdo_write_u16(0x1800, 3, 200));
    PREPARE(ecvt_motor_sdo_write_u16(0x1800, 5, 30));
    PREPARE(ecvt_motor_configure_tpdo2_map_position_only());
    PREPARE(ecvt_motor_configure_tpdo2_fast_event());
    PREPARE(ecvt_motor_configure_tpdo3_map_velocity_only());
    PREPARE(ecvt_motor_configure_tpdo3_fast_event());
    PREPARE(ecvt_motor_configure_tpdo4_map_current_only());
    PREPARE(ecvt_motor_configure_tpdo4_fast_event());
    PREPARE(ecvt_motor_sdo_write_u8(0x6060, 0, 1));
    PREPARE(ecvt_motor_sdo_write_u16(0x605D, 0, 1)); // Halt uses configured profile deceleration.
    PREPARE(ecvt_motor_set_directional_profile(accel_up, decel_up, accel_down, decel_down));
    PREPARE(ecvt_motor_send_profile_accel_decel(profile_accel, profile_decel));
    const int64_t feedback_start = esp_timer_get_time();
    PREPARE(ecvt_motor_nmt_send(0x01, NODE_ID));
    uint8_t actual_mode = 0;
    PREPARE(ecvt_motor_sdo_read_u8(0x6061, 0, &actual_mode));
    if (actual_mode != 1) return ESP_ERR_INVALID_STATE;
    while (servo_feedback_snapshot().position_us <= feedback_start) {
        if (esp_timer_get_time() - feedback_start > 1000000) return ESP_ERR_TIMEOUT;
        if (!ecvt_app_wait_ms(10)) return ESP_ERR_INVALID_STATE;
    }
    // Seed a stationary target before enabling, so clearing Halt cannot resume
    // an old target from a prior test. No zero capture occurs here.
    PREPARE(ecvt_motor_send_target_position_and_speed(ecvt_motor_get_actual_position_feedback(), fminf(1800, limits.max_rpm)));
    for (uint16_t word : {uint16_t(0x0006), uint16_t(0x0007), uint16_t(0x010F)}) {
        PREPARE(ecvt_motor_send_controlword(word));
        PREPARE(wait_for_drive_state(word == 0x0006 ? 0x0021 : word == 0x0007 ? 0x0023 : 0x0027, word));
    }
    if (brake_release_wait) {
        // Latch the measured hold target with Halt set before clearing Halt.
        // Clearing Halt must not resume a target from a previous operation.
        uint32_t held_position = 0;
        PREPARE(ecvt_motor_sdo_read_u32(0x6064, 0, &held_position));
        PREPARE(ecvt_motor_sdo_write_u32(0x607A, 0, held_position));
        PREPARE(ecvt_motor_sdo_write_u16(0x6040, 0, 0x012F));
        PREPARE(wait_for_hold_target_ack(false));
        PREPARE(ecvt_motor_sdo_write_u16(0x6040, 0, 0x013F));
        PREPARE(wait_for_hold_target_ack(true));
        uint32_t demanded_position = 0;
        PREPARE(ecvt_motor_sdo_read_u32(0x6062, 0, &demanded_position));
        if (!hold_demand_matches(held_position, demanded_position)) {
            printf("Hold refused BEFORE clearing Halt: requested=%" PRId32 " demanded=%" PRId32 "; no active-wait or rotation target sent.\n",
                   static_cast<int32_t>(held_position), static_cast<int32_t>(demanded_position));
            return ESP_ERR_INVALID_STATE;
        }
        PREPARE(ecvt_motor_sdo_write_u16(0x6040, 0, 0x000F));
        PREPARE(wait_for_drive_state(0x0027, 0x000F));
        puts("ACTIVE HOLD: waiting 1000 ms for drive-managed brake release before the bench measurement/move.");
        const int64_t release_ready_us = esp_timer_get_time() + 1000000;
        while (esp_timer_get_time() < release_ready_us) {
            uint16_t observed = 0;
            PREPARE(ecvt_motor_sdo_read_u16(0x6041, 0, &observed));
            PREPARE(ecvt_motor_sdo_read_u32(0x6062, 0, &demanded_position));
            if (!hold_demand_matches(held_position, demanded_position)) {
                printf("Active hold aborted: requested=%" PRId32 " demanded=%" PRId32 "; three-turn target not sent.\n",
                       static_cast<int32_t>(held_position), static_cast<int32_t>(demanded_position));
                return ESP_ERR_INVALID_STATE;
            }
            if ((observed & 0x006F) != 0x0027) {
                printf("Active-hold wait failed: statusword=0x%04x; three-turn target not sent.\n", observed);
                return ESP_ERR_INVALID_STATE;
            }
            if (!ecvt_app_wait_ms(20)) return ESP_ERR_INVALID_STATE;
        }
        puts("Active-hold wait complete. Mechanical brake release remains unverified.");
    }
#undef PREPARE
    return ESP_OK;
}

void finish_operation()
{
    s_sample_rpm_enabled = false;
    // Disable first, even after an MCU-only reboot or partial setup. Do not
    // wait for mode/velocity/feedback checks or replay/SD shutdown to send it.
    const esp_err_t result = brake_probe_restore();
    const bool confirmed = result == ESP_OK;
    if (confirmed) drive_touched = false;
    else last_error.store(result);
    // Replay cooperatively returns; never delete a task while it owns FILE/vector state.
    while (replay_alive.load()) vTaskDelay(pdMS_TO_TICKS(10));
    state.store(confirmed ? RunState::Idle : RunState::StopUnconfirmed);
    xQueueReset(commands);
    active_epoch.store(stop_epoch.load());
    puts(confirmed ? "Drive disabled; explicit start required. Standstill/brake lock UNVERIFIED."
                   : "DISABLE UNCONFIRMED; retry stop. No automatic restart.");
    busy.store(false);
}

void replay_task(void *)
{
    read_sample_data(nullptr);
    replay_alive.store(false);
    vTaskDelete(nullptr);
}

void run_motion(const ConsoleCommand &command)
{
    float position = command.values[0], rpm = command.values[1];
    float accel = profile_accel, decel = profile_decel;
    const bool bench_test = command.kind == CommandKind::ThreeRevolutions || command.kind == CommandKind::BenchHold;
    if (bench_test) {
        // Fresh-boot manual bench test: software zero has not been recaptured.
        if (drive_touched || state.load() != RunState::Idle || automatic.load()) {
            puts("Three-revolution test requires idle manual mode with drive disabled; inspect first.");
            busy.store(false); return;
        }
        uint32_t numerator = 0, denominator = 0, raw_start = 0, output_mask = 0;
        uint16_t drive_status = 0;
        esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &drive_status);
        if (result == ESP_OK && (drive_status & 0x0008)) {
            printf("Three-revolution test refused before setup: drive fault, statusword=0x%04x; use servo errors.\n", drive_status);
            busy.store(false); return;
        }
        if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x60FE, 2, &output_mask);
        if (result == ESP_OK && output_mask != 0) result = ESP_ERR_INVALID_STATE;
        if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x6091, 1, &numerator);
        if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x6091, 2, &denominator);
        if (result == ESP_OK) result = ecvt_motor_sdo_read_u32(0x6064, 0, &raw_start);
        float start = 0;
        if (result == ESP_OK && !three_revolution_target(numerator, denominator, static_cast<int32_t>(raw_start), start, position))
            result = ESP_ERR_INVALID_ARG;
        if (result != ESP_OK) {
            printf("Three-revolution test refused before enable: error=%s mask=0x%08" PRIx32
                   " 6091:01=%" PRIu32 " 6091:02=%" PRIu32 ". Requires verified 10000 command units/motor rev and representable endpoints; run servo inspect.\n",
                   esp_err_to_name(result), output_mask, numerator, denominator);
            busy.store(false); return;
        }
        rpm = 60;
        if (!limits_explicit) {
            // The user approved this exact relative distance and speed.
            limits = {true, start, position, rpm};
        }
        if (!within_limits(limits, start, rpm) || !within_limits(limits, position, rpm)) {
            puts("Three-revolution test refused: configured limits do not cover both endpoints at 60 rpm.");
            busy.store(false); return;
        }
        if (command.kind == CommandKind::BenchHold)
            printf("Unloaded HOLD ONLY test: measured start=%" PRId32 "; no three-turn target will be sent.\n", static_cast<int32_t>(raw_start));
        else printf("Unloaded bench test: start=%" PRId32 " target=%" PRId64 " command units; +3 motor rev at 60 rpm. Normal drive enable/brake sequencing; no forced brake output.\n",
                    static_cast<int32_t>(raw_start), static_cast<int64_t>(static_cast<int32_t>(raw_start)) + 30000);
    }
    if (command.kind == CommandKind::PositionStep) { position = 100; rpm = 1000; accel = 500; decel = 100; }
    if (command.kind == CommandKind::PositionProfile) { position = 75; rpm = 2000; accel = decel = 1666; }
    if (command.kind == CommandKind::StepResponse) { position = 10; rpm = 2000; accel = decel = 1000; }
    const bool auto_run = command.kind == CommandKind::AutoStart;
    if (automatic.load() != auto_run) { puts("Select the matching mode while stopped first."); busy.store(false); return; }
    if (auto_run ? (!within_limits(limits, -6.9f, 3000) || !within_limits(limits, 0, 3000)) :
        (!within_limits(limits, position, rpm) ||
         (command.kind == CommandKind::StepResponse && !within_limits(limits, -10, rpm)))) {
        puts("Movement rejected: configure explicit limits covering the requested operation."); busy.store(false); return;
    }
    state.store(RunState::Busy);
    esp_err_t result = prepare_position(bench_test);
    if (result == ESP_OK && !ecvt_app_stop_requested()) {
        if (auto_run) {
            // Existing bench input path, not the future DAQ input contract.
            s_latest_throttle = 0; s_latest_engine_rpm = 0; s_latest_wheel_rpm = 0; s_latest_brake_state = false;
            xQueueReset(twai_rx_sample_queue);
            s_sample_rpm_enabled = true;
            if (sd_available) {
                replay_alive.store(true);
                if (xTaskCreate(replay_task, "csv_replay", 8192, nullptr, 5, nullptr) != pdPASS) {
                    replay_alive.store(false); result = ESP_ERR_NO_MEM;
                }
            }
            if (result == ESP_OK) calc_task(nullptr);
        } else result = motor_test_run(command.kind, position, rpm, accel, decel);
    }
    if (result != ESP_OK && !ecvt_app_stop_requested()) {
        last_error.store(result);
        printf("Operation ended with %s\n", esp_err_to_name(result));
    }
    // End normal completion as well as cancellation before finishing the owner.
    stop_epoch.fetch_add(1);
    finish_operation();
}

void worker(void *)
{
    while (true) {
        if (ecvt_app_stop_requested()) { finish_operation(); continue; }
        PendingCommand pending;
        if (xQueueReceive(commands, &pending, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        if (pending.epoch != stop_epoch.load()) { finish_operation(); continue; }
        active_epoch.store(pending.epoch);
        const auto &command = pending.command;
        if (brake_probe_restore_pending() && command.kind != CommandKind::BrakeRestore &&
            command.kind != CommandKind::ServoPing && command.kind != CommandKind::ServoInspect && command.kind != CommandKind::ServoErrors && command.kind != CommandKind::ServoRead) {
            puts("Brake-test disable unconfirmed: use servo brake-restore; motion/settings refused.");
            busy.store(false); continue;
        }
        if (state.load() == RunState::StopUnconfirmed && command.kind != CommandKind::ServoPing &&
            command.kind != CommandKind::ServoInspect && command.kind != CommandKind::ServoErrors && command.kind != CommandKind::ServoRead && command.kind != CommandKind::BrakeRestore && command.kind != CommandKind::ServoControlword) {
            puts("Stop unconfirmed: use stop to retry; mode changes/motion refused."); busy.store(false); continue;
        }
        switch (command.kind) {
        case CommandKind::ServoVelocityZero: {
            const esp_err_t result = automatic.load() ? ESP_ERR_INVALID_STATE : servo_prepare_velocity_zero();
            printf("Velocity-zero preparation: %s. No automatic state sequence or speed run.\n", esp_err_to_name(result));
            break;
        }
        case CommandKind::ServoTorqueZeroTest: {
            const esp_err_t result = automatic.load() ? ESP_ERR_INVALID_STATE : torque_zero_brake_test();
            last_error.store(result);
            printf("Torque-zero brake test: %s.\n", esp_err_to_name(result));
            break;
        }
        case CommandKind::ServoPositionMode: {
            const esp_err_t result = automatic.load() ? ESP_ERR_INVALID_STATE : servo_prepare_position_mode();
            printf("Position-mode preparation: %s. No target or enable command sent.\n", esp_err_to_name(result));
            break;
        }
        case CommandKind::ServoControlword: {
            const auto word = static_cast<uint16_t>(command.values[0]);
            // Normally handled at submission; retain the unconditional path
            // here too so a queued disable can never acquire enable guards.
            if (word == 0) { finish_operation(); break; }
            uint16_t before = 0, after = 0;
            esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &before);
            if (automatic.load()) result = ESP_ERR_INVALID_STATE;
            // Nonzero state steps remain supervised. The 0000 path above
            // bypasses these start-only restrictions.
            if (result == ESP_OK && (before & 0x0004)) result = ESP_ERR_INVALID_STATE;
            if (result == ESP_OK && word == 0x000F) {
                uint8_t mode = 0, errors = 0;
                uint32_t target_velocity = 1;
                result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
                if (result == ESP_OK) result = ecvt_motor_sdo_read_u8(0x1001, 0, &errors);
                if (result == ESP_OK && mode == 3 && errors == 0)
                    result = ecvt_motor_sdo_write_u32(0x60FF, 0, 0);
                if (result == ESP_OK && mode == 3) result = ecvt_motor_sdo_read_u32(0x60FF, 0, &target_velocity);
                if (result == ESP_OK && !direct_bench_enable_allowed(before, mode, errors, target_velocity))
                    result = ESP_ERR_INVALID_STATE;
                if (result != ESP_OK) puts("Direct enable refused: requires switched-on state, no drive error, and position mode 1 or zero-velocity mode 3.");
                else if (mode == 1) puts("Explicit position-mode state step; existing position/trajectory registers remain unchanged.");
            }
            if (result != ESP_OK) {
                printf("Single controlword 0x%04x NOT SENT: %s; status_before=0x%04x.\n", word, esp_err_to_name(result), before);
                break;
            }
            result = ecvt_motor_sdo_write_u16(0x6040, 0, word);
            printf("ONE controlword 6040:00=0x%04x: %s. No automatic next controlword or target.\n", word, esp_err_to_name(result));
            if (result == ESP_OK) {
                if (word == 0x000F) { drive_touched = true; state.store(RunState::Holding); }
                vTaskDelay(pdMS_TO_TICKS(100));
                const esp_err_t read = ecvt_motor_sdo_read_u16(0x6041, 0, &after);
                if (read == ESP_OK) printf("Status after single controlword: 0x%04x operation_enabled=%u fault=%u.\n", after, (after >> 2)&1, (after >> 3)&1);
                else printf("Status read failed: %s.\n", esp_err_to_name(read));
            }
            break;
        }
        case CommandKind::ServoRead: {
            const auto index = static_cast<uint16_t>(command.values[0]);
            const auto sub = static_cast<uint8_t>(command.values[1]);
            const auto width = static_cast<unsigned>(command.values[2]);
            uint32_t value = 0;
            esp_err_t result;
            if (width == 1) {
                uint8_t byte = 0; result = ecvt_motor_sdo_read_u8(index, sub, &byte); value = byte;
            } else if (width == 2) {
                uint16_t word = 0; result = ecvt_motor_sdo_read_u16(index, sub, &word); value = word;
            } else result = ecvt_motor_sdo_read_u32(index, sub, &value);
            if (result == ESP_OK) printf("Read-only 0x%04x:%02x u%u = 0x%08" PRIx32 " (%" PRIu32 ")\n", index, sub, width*8, value, value);
            else printf("Read-only 0x%04x:%02x u%u failed: %s\n", index, sub, width*8, esp_err_to_name(result));
            break;
        }
        case CommandKind::ServoResetFault: {
            if (drive_touched || state.load() != RunState::Idle || automatic.load()) {
                puts("Fault reset requires idle manual bench mode before motion this boot; no reset sent."); break;
            }
            uint16_t word = 0;
            esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &word);
            if (result == ESP_OK && !(word & 0x0008)) {
                puts("Drive reports no fault; no reset sent."); break;
            }
            if (result == ESP_OK) {
                result = ecvt_motor_sdo_write_u16(0x6040, 0, 0x0080);
                // A timed-out write may still take effect: lower the reset bit
                // after every attempt, including error/cancellation paths.
                vTaskDelay(pdMS_TO_TICKS(100));
                const esp_err_t clear = ecvt_motor_sdo_write_u16(0x6040, 0, 0);
                if (clear != ESP_OK) result = clear;
            }
            if (result == ESP_OK) result = ecvt_motor_sdo_read_u16(0x6041, 0, &word);
            if (result == ESP_OK && (word & 0x004F) != 0x0040) result = ESP_ERR_INVALID_STATE;
            last_error.store(result);
            printf("Fault reset: %s statusword=0x%04x; no enable or target command sent.\n", esp_err_to_name(result), word);
            break;
        }
        case CommandKind::ServoErrors: {
            puts("Read-only drive errors; no fault reset, controlword or history clear.");
            uint16_t code = 0;
            esp_err_t result = ecvt_motor_sdo_read_u16(0x603F, 0, &code);
            if (result == ESP_OK) printf("0x603f:00 error_code=0x%04x (%u)\n", code, code);
            else printf("0x603f:00 read failed: %s\n", esp_err_to_name(result));
            uint8_t error_register = 0;
            uint16_t count = 0;
            result = ecvt_motor_sdo_read_u8(0x1001, 0, &error_register);
            if (result == ESP_OK) printf("0x1001:00 error_register=0x%02x\n", error_register);
            else printf("0x1001:00 read failed: %s\n", esp_err_to_name(result));
            result = ecvt_motor_sdo_read_u16(0x1003, 0, &count);
            if (result != ESP_OK) {
                // This bench drive advertises u8 in its EDS but replies u16.
                // Keep strict width checking for other object reads.
                uint8_t count8 = 0;
                result = ecvt_motor_sdo_read_u8(0x1003, 0, &count8);
                if (result == ESP_OK) count = count8;
            }
            if (result == ESP_OK) printf("0x1003:00 error_history_count=%u\n", count);
            else printf("0x1003:00 read failed: %s\n", esp_err_to_name(result));
            for (uint8_t sub = 1; result == ESP_OK && sub <= count && sub <= 5 && !ecvt_app_stop_requested(); ++sub) {
                uint32_t entry = 0;
                result = ecvt_motor_sdo_read_u32(0x1003, sub, &entry);
                if (result == ESP_OK) printf("0x1003:%02x error_history=0x%08" PRIx32 "\n", sub, entry);
                else printf("0x1003:%02x read failed: %s\n", sub, esp_err_to_name(result));
            }
            uint32_t voltage = 0;
            result = ecvt_motor_sdo_read_u32(0x6079, 0, &voltage);
            if (result == ESP_OK) printf("0x6079:00 dc_link_voltage_raw=%" PRIu32 "\n", voltage);
            else printf("0x6079:00 read failed: %s\n", esp_err_to_name(result));
            break;
        }
        case CommandKind::BrakeProbe:
        case CommandKind::BrakeRestore: {
            if (command.kind == CommandKind::BrakeProbe &&
                (drive_touched || state.load() != RunState::Idle || automatic.load())) {
                puts("Brake test requires idle manual mode before motion; inspect first.");
                break;
            }
            const bool had_pending_disable = brake_probe_restore_pending();
            const esp_err_t result = command.kind == CommandKind::BrakeProbe ? brake_probe_run() : brake_probe_restore();
            last_error.store(result);
            if (command.kind == CommandKind::BrakeProbe || had_pending_disable)
                state.store(brake_probe_restore_pending() ? RunState::StopUnconfirmed : RunState::Idle);
            printf("Brake state test: %s; disable=%s; mechanical_brake=UNVERIFIED\n",
                   esp_err_to_name(result), brake_probe_restore_pending() ? "UNCONFIRMED: use servo brake-restore" : "no pending disable");
            break;
        }
        case CommandKind::ServoInspect: {
            puts("Read-only servo inspection; no controlword, enable or output writes.");
            uint16_t statusword = 0;
            esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &statusword);
            if (result == ESP_OK)
                printf("0x6041:00 statusword=0x%04x operation_enabled=%u fault=%u (drive report; brake unverified)\n",
                       statusword, (statusword >> 2) & 1, (statusword >> 3) & 1);
            else printf("0x6041:00 read failed: %s\n", esp_err_to_name(result));
            const uint16_t indices[] = {0x6064, 0x606C, 0x608F, 0x608F, 0x6091, 0x6091, 0x6093, 0x6093, 0x60FE, 0x60FE};
            const uint8_t subs[] = {0, 0, 1, 2, 1, 2, 1, 2, 1, 2};
            const char *labels[] = {"position_user_units", "velocity_raw", "encoder_increments", "encoder_motor_revs",
                "gear_motor_revs", "gear_shaft_revs", "position_factor_numerator", "position_factor_denominator",
                "outputs_physical", "outputs_mask"};
            for (unsigned i = 0; i < sizeof(indices)/sizeof(indices[0]) && !ecvt_app_stop_requested(); ++i) {
                uint32_t value = 0;
                result = ecvt_motor_sdo_read_u32(indices[i], subs[i], &value);
                if (result == ESP_OK) {
                    if (indices[i] == 0x6064 || indices[i] == 0x606C)
                        printf("0x%04x:%02x %s=%" PRId32 " (0x%08" PRIx32 ")\n", indices[i], subs[i], labels[i], static_cast<int32_t>(value), value);
                    else printf("0x%04x:%02x %s=%" PRIu32 " (0x%08" PRIx32 ")\n", indices[i], subs[i], labels[i], value, value);
                }
                else printf("0x%04x:%02x read failed: %s\n", indices[i], subs[i], esp_err_to_name(result));
            }
            puts("Inspection finished. Output bits do not confirm mechanical brake state.");
            break;
        }
        case CommandKind::ServoPing: {
            // Intentionally no preparation, NMT, controlword, PDO remapping,
            // drive_touched mutation or automatic stop on probe failure.
            uint8_t mode = 0;
            const esp_err_t result = ecvt_motor_sdo_read_u8(0x6061, 0, &mode);
            if (result == ESP_OK) {
                printf("Servo replied: node=%u object=0x6061:00 mode=%d. Read-only; no enable/motion command sent.\n",
                       NODE_ID, static_cast<int>(static_cast<int8_t>(mode)));
            } else {
                printf("Servo ping failed: %s. No valid fresh reply; check drive power, node ID, bitrate and CAN wiring.\n",
                       esp_err_to_name(result));
            }
            break;
        }
        case CommandKind::ModeManual: automatic.store(false); puts("Manual mode selected; no motion started."); break;
        case CommandKind::ModeAuto: automatic.store(true); puts("Automatic mode selected; use controller start."); break;
        case CommandKind::Limits:
            if (valid_limits(command.values[0], command.values[1], command.values[2])) {
                limits = {true, command.values[0], command.values[1], command.values[2]};
                limits_explicit = true;
                printf("Limits: %.4f .. %.4f logical rev; max %.2f rpm\n", limits.min_revs, limits.max_revs, limits.max_rpm);
            } else puts("Invalid limits.");
            break;
        case CommandKind::Profile:
            if (command.values[0] > 0 && command.values[1] > 0 && command.values[0] <= 12884900 && command.values[1] <= 12884900) {
                profile_accel = command.values[0]; profile_decel = command.values[1];
                puts("Manual motion profile updated (rpm/s).");
            } else puts("Profile values must be positive and serializable.");
            break;
        case CommandKind::LogStart:
            printf("Log start requested: %s\n", esp_err_to_name(sd_available ? logger_session_start() : ESP_ERR_INVALID_STATE)); break;
        case CommandKind::LogStop: logger_session_stop(); puts("Log stop requested; draining queued records."); break;
        default: run_motion(command); continue;
        }
        busy.store(false);
    }
}
} // namespace

extern "C" bool ecvt_app_stop_requested() { return gate.cancelled(); }

extern "C" void ecvt_app_motor_error(esp_err_t error)
{
    last_error.store(error);
    stop_epoch.fetch_add(1);
}

extern "C" bool ecvt_app_allow_move(float position, float rpm) { return within_limits(limits, position, rpm); }

bool ecvt_app_wait_ms(unsigned milliseconds)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(milliseconds) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (ecvt_app_stop_requested()) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !ecvt_app_stop_requested();
}

void ecvt_app_print_status()
{
    const auto feedback = servo_feedback_snapshot();
    const char *states[] = {"idle", "busy", "drive-enabled", "DISABLE-UNCONFIRMED"};
    printf("mode=%s state=%s busy=%d error=%s log=%s brake=UNVERIFIED\n",
        automatic.load() ? "auto" : "manual", states[static_cast<unsigned>(state.load())], busy.load(),
        esp_err_to_name(last_error.load()), stop_logging ? "off/draining" : "on");
    printf("statusword=0x%04x position_counts=%" PRIi32 " velocity_counts=%" PRIi32
           " feedback_age_ms=%lld\n", feedback.statusword, feedback.position_counts, feedback.velocity_counts,
           feedback.status_us ? (long long)((esp_timer_get_time() - feedback.status_us)/1000) : -1LL);
    printf("log_dropped_records=%" PRIu32 "\n", logger_dropped_records());
    printf("brake_test_disable=%s\n", brake_probe_restore_pending() ? "UNCONFIRMED" : "no pending disable");
}

void ecvt_app_submit(const ConsoleCommand &command)
{
    if (command.kind == CommandKind::Help) { serial_console_print_help(); return; }
    if (command.kind == CommandKind::Status) { ecvt_app_print_status(); return; }
    if (command.kind == CommandKind::TestList) {
        puts("position-step: 100 rev, 1000 rpm, 12 s\nposition-profile: 75 rev, 2000 rpm, target/20 s\n"
             "step-response: +10/-10 rev, 2000 rpm, 3 s each\nthree-revolutions: +3 motor rev from current position, 60 rpm; fresh-boot unloaded bench\nVelocity/torque presets are not yet exposed."); return;
    }
    if (command.kind == CommandKind::Stop ||
        (command.kind == CommandKind::ServoControlword && command.values[0] == 0)) {
        // Disable is a cancellation request, not a queued state step. It must
        // bypass busy/mode/pending-disable checks and run through the owner.
        gate.stop(); puts("Disable requested (6040=0x0000)."); return;
    }
    if (command.kind == CommandKind::LogStart) {
        printf("Log start requested: %s\n", esp_err_to_name(sd_available ? logger_session_start() : ESP_ERR_INVALID_STATE)); return;
    }
    if (command.kind == CommandKind::LogStop) {
        logger_session_stop(); puts("Log stop requested; draining queued records."); return;
    }
    if (!gate.admit()) {
        puts("Operation in progress; use stop before changing mode or starting another operation."); return;
    }
    PendingCommand pending{command, stop_epoch.load()};
    if (xQueueSend(commands, &pending, 0) != pdTRUE) { busy.store(false); puts("Command not queued."); }
}

esp_err_t ecvt_app_start(bool storage_available)
{
    sd_available = storage_available;
    commands = xQueueCreate(1, sizeof(PendingCommand));
    if (!commands) return ESP_ERR_NO_MEM;
    return xTaskCreate(worker, "motor_owner", 12288, nullptr, 6, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
