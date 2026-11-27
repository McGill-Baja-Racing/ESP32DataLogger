#include "motor_core.h"
#include "parameters.h"
#include "drive_operations.h"
#include "limits.h"
#include "operation_gate.h"
#include "servo/ECVT_motor.h"
#include "servo/velocity_zero.h"
#include "experiments/motor_tests.h"
#include "experiments/brake_probe.h"
#include "experiments/three_revolutions.h"
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
using RunState = MotorRunState;
struct PendingCommand { MotorRequest command; unsigned epoch; uint32_t id; };
QueueHandle_t commands;
OperationGate gate;
auto &stop_epoch = gate.stop_epoch;
auto &active_epoch = gate.active_epoch;
auto &busy = gate.busy;
std::atomic<bool> automatic{false};
std::atomic<RunState> state{RunState::Idle};
std::atomic<esp_err_t> last_error{ESP_OK};
MotorCoreServices services;
constexpr uint8_t NODE_ID = 1;
std::atomic<uint32_t> next_id{1}, stop_id{0}, active_id{0}, queued_id{0};
std::atomic<MotorCommand> queued_kind{MotorCommand::Stop};
MotorResult active_result, last_result;
esp_err_t last_disable_error=ESP_OK;
portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;

void publish(MotorResult result)
{
    result.completed_us=esp_timer_get_time();
    portENTER_CRITICAL(&status_mux);
    last_result=result;
    portEXIT_CRITICAL(&status_mux);
    if (services.completed) services.completed(services.context,result);
}
void publish_pending_stop(esp_err_t error)
{
    portENTER_CRITICAL(&status_mux);
    const uint32_t id=stop_id.exchange(0);
    portEXIT_CRITICAL(&status_mux);
    if (!id) return;
    MotorResult result; result.id=id; result.command=MotorCommand::Stop; result.error=error;
    publish(result);
}
bool drive_touched = false;
MotionLimits limits;
bool limits_explicit = false;
float profile_accel = 2300.0f, profile_decel = 2300.0f;

esp_err_t finish_operation()
{
    if (services.stop_inputs) services.stop_inputs(services.context);
    // Disable first, even after an MCU-only reboot or partial setup. Do not
    // wait for mode/velocity/feedback checks or replay/SD shutdown to send it.
    const esp_err_t result = brake_probe_restore();
    last_disable_error=result;
    const bool confirmed = result == ESP_OK;
    if (confirmed) drive_touched = false;
    else last_error.store(result);
    // Replay cooperatively returns; never delete a task while it owns FILE/vector state.
    if (services.wait_inputs) services.wait_inputs(services.context);
    state.store(confirmed ? RunState::Idle : RunState::DisableUnconfirmed);
    xQueueReset(commands);
    const uint32_t cancelled_id=queued_id.exchange(0);
    if (cancelled_id) {
        MotorResult cancelled; cancelled.id=cancelled_id; cancelled.command=queued_kind.load();
        cancelled.error=ESP_ERR_INVALID_STATE; cancelled.cancelled=true; publish(cancelled);
    }
    active_epoch.store(stop_epoch.load());
    puts(confirmed ? "Drive disabled; explicit start required. Standstill/brake lock UNVERIFIED."
                   : "DISABLE UNCONFIRMED; retry stop. No automatic restart.");
    busy.store(false);
    return result;
}

esp_err_t run_motion(const MotorRequest &command)
{
    float position = command.values[0], rpm = command.values[1];
    float accel = profile_accel, decel = profile_decel;
    const bool bench_test = command.kind == MotorCommand::ThreeRevolutions || command.kind == MotorCommand::BenchHold;
    if (bench_test) {
        // Fresh-boot manual bench test: software zero has not been recaptured.
        if (drive_touched || state.load() != RunState::Idle || automatic.load()) {
            puts("Three-revolution test requires idle manual mode with drive disabled; inspect first.");
            busy.store(false); return ESP_ERR_INVALID_STATE;
        }
        uint32_t numerator = 0, denominator = 0, raw_start = 0, output_mask = 0;
        uint16_t drive_status = 0;
        esp_err_t result = ecvt_motor_sdo_read_u16(0x6041, 0, &drive_status);
        if (result == ESP_OK && (drive_status & 0x0008)) {
            printf("Three-revolution test refused before setup: drive fault, statusword=0x%04x; use servo errors.\n", drive_status);
            busy.store(false); return ESP_ERR_INVALID_STATE;
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
            busy.store(false); return ESP_ERR_INVALID_STATE;
        }
        rpm = 60;
        if (!limits_explicit) {
            // The user approved this exact relative distance and speed.
            portENTER_CRITICAL(&status_mux);
            limits = {true, start, position, rpm};
            portEXIT_CRITICAL(&status_mux);
        }
        if (!within_limits(limits, start, rpm) || !within_limits(limits, position, rpm)) {
            puts("Three-revolution test refused: configured limits do not cover both endpoints at 60 rpm.");
            busy.store(false); return ESP_ERR_INVALID_STATE;
        }
        if (command.kind == MotorCommand::BenchHold)
            printf("Unloaded HOLD ONLY test: measured start=%" PRId32 "; no three-turn target will be sent.\n", static_cast<int32_t>(raw_start));
        else printf("Unloaded bench test: start=%" PRId32 " target=%" PRId64 " command units; +3 motor rev at 60 rpm. Normal drive enable/brake sequencing; no forced brake output.\n",
                    static_cast<int32_t>(raw_start), static_cast<int64_t>(static_cast<int32_t>(raw_start)) + 30000);
    }
    if (command.kind == MotorCommand::PositionStep) { position = 100; rpm = 1000; accel = 500; decel = 100; }
    if (command.kind == MotorCommand::PositionProfile) { position = 75; rpm = 2000; accel = decel = 1666; }
    if (command.kind == MotorCommand::StepResponse) { position = 10; rpm = 2000; accel = decel = 1000; }
    const bool auto_run = command.kind == MotorCommand::AutoStart;
    if (auto_run && !services.run_automatic) {
        puts("No automatic controller service installed; no setup or enable sent.");
        return ESP_ERR_INVALID_STATE;
    }
    if (automatic.load() != auto_run) { puts("Select the matching mode while stopped first."); busy.store(false); return ESP_ERR_INVALID_STATE; }
    if (auto_run ? (!within_limits(limits, -6.9f, 3000) || !within_limits(limits, 0, 3000)) :
        (!within_limits(limits, position, rpm) ||
         (command.kind == MotorCommand::StepResponse && !within_limits(limits, -10, rpm)))) {
        puts("Movement rejected: configure explicit limits covering the requested operation."); busy.store(false); return ESP_ERR_INVALID_STATE;
    }
    state.store(RunState::Busy);
    esp_err_t result = motor_prepare_position({profile_accel,profile_decel,limits.max_rpm,NODE_ID},bench_test,drive_touched);
    if (result == ESP_OK && !motor_operation_cancelled()) {
        if (auto_run) {
            result = services.run_automatic ? services.run_automatic(services.context) : ESP_ERR_INVALID_STATE;
        } else result = motor_test_run(command.kind, position, rpm, accel, decel);
    }
    if (result != ESP_OK && !motor_operation_cancelled()) {
        last_error.store(result);
        printf("Operation ended with %s\n", esp_err_to_name(result));
    }
    // End normal completion as well as cancellation before finishing the owner.
    active_result.cancelled = motor_operation_cancelled();
    stop_epoch.fetch_add(1);
    const esp_err_t disabled = finish_operation();
    return result != ESP_OK ? result : disabled;
}

void worker(void *)
{
    while (true) {
        if (motor_operation_cancelled()) { publish_pending_stop(finish_operation()); continue; }
        PendingCommand pending;
        if (xQueueReceive(commands, &pending, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        if (pending.epoch != stop_epoch.load()) { publish_pending_stop(finish_operation()); continue; }
        queued_id.store(0);
        active_epoch.store(pending.epoch);
        const auto &command = pending.command;
        active_id.store(pending.id);
        active_result={}; active_result.id=pending.id; active_result.command=command.kind;
        esp_err_t outcome=ESP_OK;
        auto note = [&](esp_err_t error) { if (error != ESP_OK) outcome=error; return error; };
        if (brake_probe_restore_pending() && command.kind != MotorCommand::BrakeRestore &&
            command.kind != MotorCommand::ServoPing && command.kind != MotorCommand::ServoInspect && command.kind != MotorCommand::ServoErrors && command.kind != MotorCommand::ServoRead &&
            command.kind != MotorCommand::ParameterRead && command.kind != MotorCommand::ParameterSnapshot) {
            puts("Brake-test disable unconfirmed: use servo brake-restore; motion/settings refused.");
            active_id.store(0); busy.store(false); active_result.error=ESP_ERR_INVALID_STATE; publish(active_result); continue;
        }
        if (state.load() == RunState::DisableUnconfirmed && command.kind != MotorCommand::ServoPing &&
            command.kind != MotorCommand::ServoInspect && command.kind != MotorCommand::ServoErrors && command.kind != MotorCommand::ServoRead && command.kind != MotorCommand::ParameterRead &&
            command.kind != MotorCommand::ParameterSnapshot && command.kind != MotorCommand::BrakeRestore && command.kind != MotorCommand::ServoControlword) {
            puts("Stop unconfirmed: use stop to retry; mode changes/motion refused."); active_id.store(0); busy.store(false); active_result.error=ESP_ERR_INVALID_STATE; publish(active_result); continue;
        }
        switch (command.kind) {
        case MotorCommand::ServoVelocityZero: {
            const esp_err_t result = note(automatic.load() ? ESP_ERR_INVALID_STATE : servo_prepare_velocity_zero());
            printf("Velocity-zero preparation: %s. No automatic state sequence or speed run.\n", esp_err_to_name(result));
            break;
        }
        case MotorCommand::ServoTorqueZeroTest: {
            const esp_err_t result = note(automatic.load() ? ESP_ERR_INVALID_STATE : motor_torque_zero_brake_test(drive_touched));
            last_error.store(result);
            printf("Torque-zero brake test: %s.\n", esp_err_to_name(result));
            break;
        }
        case MotorCommand::ServoPositionMode: {
            const esp_err_t result = note(automatic.load() ? ESP_ERR_INVALID_STATE : motor_prepare_position_mode());
            printf("Position-mode preparation: %s. No target or enable command sent.\n", esp_err_to_name(result));
            break;
        }
        case MotorCommand::ServoControlword: {
            const auto word = static_cast<uint16_t>(command.values[0]);
            // Normally handled at submission; retain the unconditional path
            // here too so a queued disable can never acquire enable guards.
            if (word == 0) { outcome=finish_operation(); break; }
            uint16_t before = 0, after = 0;
            esp_err_t result = note(ecvt_motor_sdo_read_u16(0x6041, 0, &before));
            if (automatic.load()) result = note(ESP_ERR_INVALID_STATE);
            // Nonzero state steps remain supervised. The 0000 path above
            // bypasses these start-only restrictions.
            if (result == ESP_OK && (before & 0x0004)) result = note(ESP_ERR_INVALID_STATE);
            if (result == ESP_OK && word == 0x000F) {
                uint8_t mode = 0, errors = 0;
                uint32_t target_velocity = 1;
                result = note(ecvt_motor_sdo_read_u8(0x6061, 0, &mode));
                if (result == ESP_OK) result = note(ecvt_motor_sdo_read_u8(0x1001, 0, &errors));
                if (result == ESP_OK && mode == 3 && errors == 0)
                    result = note(ecvt_motor_sdo_write_u32(0x60FF, 0, 0));
                if (result == ESP_OK && mode == 3) result = note(ecvt_motor_sdo_read_u32(0x60FF, 0, &target_velocity));
                if (result == ESP_OK && !direct_bench_enable_allowed(before, mode, errors, target_velocity))
                    result = note(ESP_ERR_INVALID_STATE);
                if (result != ESP_OK) puts("Direct enable refused: requires switched-on state, no drive error, and position mode 1 or zero-velocity mode 3.");
                else if (mode == 1) puts("Explicit position-mode state step; existing position/trajectory registers remain unchanged.");
            }
            if (result != ESP_OK) {
                printf("Single controlword 0x%04x NOT SENT: %s; status_before=0x%04x.\n", word, esp_err_to_name(result), before);
                break;
            }
            result = note(ecvt_motor_sdo_write_u16(0x6040, 0, word));
            printf("ONE controlword 6040:00=0x%04x: %s. No automatic next controlword or target.\n", word, esp_err_to_name(result));
            if (result == ESP_OK) {
                if (word == 0x000F) { drive_touched = true; state.store(RunState::Enabled); }
                vTaskDelay(pdMS_TO_TICKS(100));
                const esp_err_t read = note(ecvt_motor_sdo_read_u16(0x6041, 0, &after));
                if (read == ESP_OK) printf("Status after single controlword: 0x%04x operation_enabled=%u fault=%u.\n", after, (after >> 2)&1, (after >> 3)&1);
                else printf("Status read failed: %s.\n", esp_err_to_name(read));
            }
            break;
        }
        case MotorCommand::ServoRead:
        case MotorCommand::ParameterRead: {
            MotorObject object=command.objects[0];
            outcome=motor_read_object(object,active_result.objects[0]);
            active_result.object_count=1;
            const uint32_t value=active_result.objects[0].object.value;
            if (outcome==ESP_OK) printf("Read-only 0x%04x:%02x u%u = 0x%08" PRIx32 " (%" PRIu32 ")\n", object.index, object.subindex, unsigned(object.width)*8, value, value);
            else printf("Read-only 0x%04x:%02x u%u failed: %s\n", object.index, object.subindex, unsigned(object.width)*8, esp_err_to_name(outcome));
            break;
        }
        case MotorCommand::ParameterWrite:
            outcome=motor_write_object(command.objects[0],active_result.objects[0]);
            active_result.object_count=1;
            break;
        case MotorCommand::ParameterSnapshot:
            outcome=motor_read_snapshot(command.objects,command.object_count,active_result);
            break;
        case MotorCommand::ServoResetFault: {
            if (drive_touched || state.load() != RunState::Idle || automatic.load()) {
                outcome=ESP_ERR_INVALID_STATE; puts("Fault reset requires idle manual bench mode before motion this boot; no reset sent."); break;
            }
            uint16_t word = 0;
            esp_err_t result = note(ecvt_motor_sdo_read_u16(0x6041, 0, &word));
            if (result == ESP_OK && !(word & 0x0008)) {
                puts("Drive reports no fault; no reset sent."); break;
            }
            if (result == ESP_OK) {
                result = note(ecvt_motor_sdo_write_u16(0x6040, 0, 0x0080));
                // A timed-out write may still take effect: lower the reset bit
                // after every attempt, including error/cancellation paths.
                vTaskDelay(pdMS_TO_TICKS(100));
                const esp_err_t clear = note(ecvt_motor_sdo_write_u16(0x6040, 0, 0));
                if (clear != ESP_OK) result = note(clear);
            }
            if (result == ESP_OK) result = note(ecvt_motor_sdo_read_u16(0x6041, 0, &word));
            if (result == ESP_OK && (word & 0x004F) != 0x0040) result = note(ESP_ERR_INVALID_STATE);
            last_error.store(result);
            printf("Fault reset: %s statusword=0x%04x; no enable or target command sent.\n", esp_err_to_name(result), word);
            break;
        }
        case MotorCommand::ServoErrors: {
            puts("Read-only drive errors; no fault reset, controlword or history clear.");
            uint16_t code = 0;
            esp_err_t result = note(ecvt_motor_sdo_read_u16(0x603F, 0, &code));
            if (result == ESP_OK) printf("0x603f:00 error_code=0x%04x (%u)\n", code, code);
            else printf("0x603f:00 read failed: %s\n", esp_err_to_name(result));
            uint8_t error_register = 0;
            uint16_t count = 0;
            result = note(ecvt_motor_sdo_read_u8(0x1001, 0, &error_register));
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
            note(result);
            if (result == ESP_OK) printf("0x1003:00 error_history_count=%u\n", count);
            else printf("0x1003:00 read failed: %s\n", esp_err_to_name(result));
            for (uint8_t sub = 1; result == ESP_OK && sub <= count && sub <= 5 && !motor_operation_cancelled(); ++sub) {
                uint32_t entry = 0;
                result = note(ecvt_motor_sdo_read_u32(0x1003, sub, &entry));
                if (result == ESP_OK) printf("0x1003:%02x error_history=0x%08" PRIx32 "\n", sub, entry);
                else printf("0x1003:%02x read failed: %s\n", sub, esp_err_to_name(result));
            }
            uint32_t voltage = 0;
            result = note(ecvt_motor_sdo_read_u32(0x6079, 0, &voltage));
            if (result == ESP_OK) printf("0x6079:00 dc_link_voltage_raw=%" PRIu32 "\n", voltage);
            else printf("0x6079:00 read failed: %s\n", esp_err_to_name(result));
            break;
        }
        case MotorCommand::BrakeProbe:
        case MotorCommand::BrakeRestore: {
            if (command.kind == MotorCommand::BrakeProbe &&
                (drive_touched || state.load() != RunState::Idle || automatic.load())) {
                outcome=ESP_ERR_INVALID_STATE; puts("Brake test requires idle manual mode before motion; inspect first.");
                break;
            }
            const bool had_pending_disable = brake_probe_restore_pending();
            const esp_err_t result = note(command.kind == MotorCommand::BrakeProbe ? brake_probe_run() : brake_probe_restore());
            last_error.store(result);
            if (command.kind == MotorCommand::BrakeProbe || had_pending_disable)
                state.store(brake_probe_restore_pending() ? RunState::DisableUnconfirmed : RunState::Idle);
            printf("Brake state test: %s; disable=%s; mechanical_brake=UNVERIFIED\n",
                   esp_err_to_name(result), brake_probe_restore_pending() ? "UNCONFIRMED: use servo brake-restore" : "no pending disable");
            break;
        }
        case MotorCommand::ServoInspect: {
            puts("Read-only servo inspection; no controlword, enable or output writes.");
            uint16_t statusword = 0;
            esp_err_t result = note(ecvt_motor_sdo_read_u16(0x6041, 0, &statusword));
            if (result == ESP_OK)
                printf("0x6041:00 statusword=0x%04x operation_enabled=%u fault=%u (drive report; brake unverified)\n",
                       statusword, (statusword >> 2) & 1, (statusword >> 3) & 1);
            else printf("0x6041:00 read failed: %s\n", esp_err_to_name(result));
            const uint16_t indices[] = {0x6064, 0x606C, 0x608F, 0x608F, 0x6091, 0x6091, 0x6093, 0x6093, 0x60FE, 0x60FE};
            const uint8_t subs[] = {0, 0, 1, 2, 1, 2, 1, 2, 1, 2};
            const char *labels[] = {"position_user_units", "velocity_raw", "encoder_increments", "encoder_motor_revs",
                "gear_motor_revs", "gear_shaft_revs", "position_factor_numerator", "position_factor_denominator",
                "outputs_physical", "outputs_mask"};
            for (unsigned i = 0; i < sizeof(indices)/sizeof(indices[0]) && !motor_operation_cancelled(); ++i) {
                uint32_t value = 0;
                result = note(ecvt_motor_sdo_read_u32(indices[i], subs[i], &value));
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
        case MotorCommand::ServoPing: {
            // Intentionally no preparation, NMT, controlword, PDO remapping,
            // drive_touched mutation or automatic stop on probe failure.
            uint8_t mode = 0;
            const esp_err_t result = note(ecvt_motor_sdo_read_u8(0x6061, 0, &mode));
            if (result == ESP_OK) {
                printf("Servo replied: node=%u object=0x6061:00 mode=%d. Read-only; no enable/motion command sent.\n",
                       NODE_ID, static_cast<int>(static_cast<int8_t>(mode)));
            } else {
                printf("Servo ping failed: %s. No valid fresh reply; check drive power, node ID, bitrate and CAN wiring.\n",
                       esp_err_to_name(result));
            }
            break;
        }
        case MotorCommand::ModeManual: automatic.store(false); puts("Manual mode selected; no motion started."); break;
        case MotorCommand::ModeAuto: automatic.store(true); puts("Automatic mode selected; use controller start."); break;
        case MotorCommand::Limits:
            if (valid_limits(command.values[0], command.values[1], command.values[2])) {
                portENTER_CRITICAL(&status_mux);
                limits = {true, command.values[0], command.values[1], command.values[2]};
                portEXIT_CRITICAL(&status_mux);
                limits_explicit = true;
                printf("Limits: %.4f .. %.4f logical rev; max %.2f rpm\n", limits.min_revs, limits.max_revs, limits.max_rpm);
            } else { outcome=ESP_ERR_INVALID_ARG; puts("Invalid limits."); }
            break;
        case MotorCommand::Profile:
            if (command.values[0] > 0 && command.values[1] > 0 && command.values[0] <= 12884900 && command.values[1] <= 12884900) {
                portENTER_CRITICAL(&status_mux);
                profile_accel = command.values[0]; profile_decel = command.values[1];
                portEXIT_CRITICAL(&status_mux);
                puts("Manual motion profile updated (rpm/s).");
            } else { outcome=ESP_ERR_INVALID_ARG; puts("Profile values must be positive and serializable."); }
            break;
        default: outcome=run_motion(command); break;
        }
        active_result.error=outcome;
        active_result.cancelled=active_result.cancelled || motor_operation_cancelled();
        if (outcome != ESP_OK && !active_result.cancelled) last_error.store(outcome);
        active_id.store(0);
        busy.store(false);
        publish(active_result);
        if (motor_operation_cancelled()) publish_pending_stop(finish_operation());
        else publish_pending_stop(last_disable_error);
    }
}
} // namespace

extern "C" bool motor_operation_cancelled() { return gate.cancelled(); }

extern "C" void motor_operation_error(esp_err_t error)
{
    last_error.store(error);
    stop_epoch.fetch_add(1);
}

extern "C" bool motor_position_allowed(float position, float rpm) { return within_limits(limits, position, rpm); }

bool motor_operation_wait_ms(unsigned milliseconds)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(milliseconds) * 1000;
    while (esp_timer_get_time() < deadline) {
        if (motor_operation_cancelled()) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !motor_operation_cancelled();
}

esp_err_t motor_core_start_recording()
{
    return services.start_recording ? services.start_recording(services.context) : ESP_ERR_INVALID_STATE;
}
void motor_core_stop_recording()
{
    if (services.stop_recording) services.stop_recording(services.context);
}
void motor_core_record_command(float position, float rpm)
{
    if (services.record_command) services.record_command(services.context,position,rpm);
}

MotorStatus motor_core_status()
{
    MotorStatus snapshot;
    snapshot.state=state.load(); snapshot.busy=busy.load(); snapshot.automatic=automatic.load();
    snapshot.active_request_id=active_id.load();
    if (!snapshot.active_request_id) snapshot.active_request_id=queued_id.load();
    snapshot.disable_pending=brake_probe_restore_pending(); snapshot.error=last_error.load();
    snapshot.feedback=servo_feedback_snapshot();
    portENTER_CRITICAL(&status_mux);
    snapshot.limits_configured=limits.configured; snapshot.minimum_revs=limits.min_revs;
    snapshot.maximum_revs=limits.max_revs; snapshot.maximum_rpm=limits.max_rpm;
    snapshot.acceleration_rpm_s=profile_accel; snapshot.deceleration_rpm_s=profile_decel;
    snapshot.last_result=last_result;
    portEXIT_CRITICAL(&status_mux);
    snapshot.recording_stopped=services.recording_stopped ? services.recording_stopped(services.context) : true;
    snapshot.dropped_records=services.dropped_records ? services.dropped_records(services.context) : 0;
    return snapshot;
}

MotorReceipt motor_core_submit(const MotorRequest &command)
{
    if (!commands) return {ESP_ERR_INVALID_STATE,0};
    if (command.kind == MotorCommand::Stop ||
        (command.kind == MotorCommand::ServoControlword && command.values[0] == 0)) {
        portENTER_CRITICAL(&status_mux);
        uint32_t id=stop_id.load();
        if (!id) { id=next_id.fetch_add(1); stop_id.store(id); }
        gate.stop();
        portEXIT_CRITICAL(&status_mux);
        puts("Disable requested (6040=0x0000).");
        return {ESP_OK,id};
    }
    if (static_cast<unsigned>(command.kind)>static_cast<unsigned>(MotorCommand::ParameterSnapshot))
        return {ESP_ERR_INVALID_ARG,0};
    for (float value: command.values) if (!std::isfinite(value)) return {ESP_ERR_INVALID_ARG,0};
    if ((command.kind==MotorCommand::ParameterRead || command.kind==MotorCommand::ParameterWrite ||
         command.kind==MotorCommand::ServoRead) && command.object_count!=1) return {ESP_ERR_INVALID_ARG,0};
    if (command.kind==MotorCommand::ParameterSnapshot &&
        (!command.object_count || command.object_count>MOTOR_SNAPSHOT_CAPACITY)) return {ESP_ERR_INVALID_ARG,0};
    const unsigned epoch=stop_epoch.load();
    if (!gate.admit()) {
        puts("Operation in progress; use stop before changing mode or starting another operation.");
        return {ESP_ERR_INVALID_STATE,0};
    }
    if (epoch!=stop_epoch.load()) { busy.store(false); return {ESP_ERR_INVALID_STATE,0}; }
    const uint32_t id=next_id.fetch_add(1);
    queued_kind.store(command.kind); queued_id.store(id);
    PendingCommand pending{command,epoch,id};
    if (xQueueSend(commands,&pending,0)!=pdTRUE) {
        queued_id.store(0); busy.store(false); puts("Command not queued."); return {ESP_ERR_NO_MEM,0};
    }
    return {ESP_OK,id};
}

esp_err_t motor_core_start(const MotorCoreServices &configuration)
{
    if (commands) return ESP_ERR_INVALID_STATE;
    services=configuration;
    commands=xQueueCreate(1,sizeof(PendingCommand));
    if (!commands) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker,"motor_owner",12288,nullptr,6,nullptr)!=pdPASS) {
        vQueueDelete(commands); commands=nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
