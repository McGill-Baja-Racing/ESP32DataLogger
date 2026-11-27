#include "ecvt_app.h"
#include "motor/motor_core.h"
#include "legacy/legacy_runtime.h"
#include "console/serial_console.h"
#include "esp_timer.h"
#include <cstdio>
#include <cinttypes>

extern "C" bool ecvt_app_stop_requested() { return motor_operation_cancelled(); }
extern "C" void ecvt_app_motor_error(esp_err_t error) { motor_operation_error(error); }
extern "C" bool ecvt_app_allow_move(float position, float rpm) { return motor_position_allowed(position,rpm); }
bool ecvt_app_wait_ms(unsigned ms) { return motor_operation_wait_ms(ms); }

void ecvt_app_print_status()
{
    const auto snapshot=motor_core_status();
    const auto &feedback=snapshot.feedback;
    const char *states[] = {"idle", "busy", "drive-enabled", "DISABLE-UNCONFIRMED"};
    printf("mode=%s state=%s busy=%d error=%s log=%s brake=UNVERIFIED\n",
        snapshot.automatic ? "auto" : "manual", states[static_cast<unsigned>(snapshot.state)], snapshot.busy,
        esp_err_to_name(snapshot.error), snapshot.recording_stopped ? "off/draining" : "on");
    printf("statusword=0x%04x position_counts=%" PRIi32 " velocity_counts=%" PRIi32
           " feedback_age_ms=%lld\n", feedback.statusword, feedback.position_counts, feedback.velocity_counts,
           feedback.status_us ? (long long)((esp_timer_get_time() - feedback.status_us)/1000) : -1LL);
    printf("log_dropped_records=%" PRIu32 "\n", snapshot.dropped_records);
    printf("brake_test_disable=%s\n", snapshot.disable_pending ? "UNCONFIRMED" : "no pending disable");
}

bool ecvt_app_motor_request(const ConsoleCommand &command, MotorRequest *output)
{
    if (!output) return false;
    MotorRequest request;
    for (unsigned i=0;i<3;++i) request.values[i]=command.values[i];
    switch(command.kind) {
    case CommandKind::Stop: request.kind=MotorCommand::Stop; break;
    case CommandKind::ModeManual: request.kind=MotorCommand::ModeManual; break;
    case CommandKind::ModeAuto: request.kind=MotorCommand::ModeAuto; break;
    case CommandKind::AutoStart: request.kind=MotorCommand::AutoStart; break;
    case CommandKind::Limits: request.kind=MotorCommand::Limits; break;
    case CommandKind::Profile: request.kind=MotorCommand::Profile; break;
    case CommandKind::Move: request.kind=MotorCommand::Move; break;
    case CommandKind::PositionStep: request.kind=MotorCommand::PositionStep; break;
    case CommandKind::PositionProfile: request.kind=MotorCommand::PositionProfile; break;
    case CommandKind::StepResponse: request.kind=MotorCommand::StepResponse; break;
    case CommandKind::ServoPing: request.kind=MotorCommand::ServoPing; break;
    case CommandKind::ServoInspect: request.kind=MotorCommand::ServoInspect; break;
    case CommandKind::BrakeProbe: request.kind=MotorCommand::BrakeProbe; break;
    case CommandKind::BrakeRestore: request.kind=MotorCommand::BrakeRestore; break;
    case CommandKind::ThreeRevolutions: request.kind=MotorCommand::ThreeRevolutions; break;
    case CommandKind::ServoErrors: request.kind=MotorCommand::ServoErrors; break;
    case CommandKind::ServoResetFault: request.kind=MotorCommand::ServoResetFault; break;
    case CommandKind::ServoRead: request.kind=MotorCommand::ServoRead; break;
    case CommandKind::BenchHold: request.kind=MotorCommand::BenchHold; break;
    case CommandKind::ServoControlword: request.kind=MotorCommand::ServoControlword; break;
    case CommandKind::ServoVelocityZero: request.kind=MotorCommand::ServoVelocityZero; break;
    case CommandKind::ServoTorqueZeroTest: request.kind=MotorCommand::ServoTorqueZeroTest; break;
    case CommandKind::ServoPositionMode: request.kind=MotorCommand::ServoPositionMode; break;
    default: return false;
    }
    if (command.kind==CommandKind::ServoRead) {
        request.object_count=1;
        request.objects[0]={static_cast<uint16_t>(command.values[0]), static_cast<uint8_t>(command.values[1]),
                            static_cast<ObjectWidth>(static_cast<unsigned>(command.values[2])),0};
    }
    *output=request; return true;
}

void ecvt_app_submit(const ConsoleCommand &command)
{
    if (command.kind==CommandKind::Help) { serial_console_print_help(); return; }
    if (command.kind==CommandKind::Status) { ecvt_app_print_status(); return; }
    if (command.kind==CommandKind::TestList) {
        puts("position-step: 100 rev, 1000 rpm, 12 s\nposition-profile: 75 rev, 2000 rpm, target/20 s\n"
             "step-response: +10/-10 rev, 2000 rpm, 3 s each\nthree-revolutions: +3 motor rev from current position, 60 rpm; fresh-boot unloaded bench\nVelocity/torque presets are not yet exposed."); return;
    }
    if (command.kind==CommandKind::LogStart) {
        printf("Log start requested: %s\n",esp_err_to_name(motor_core_start_recording())); return;
    }
    if (command.kind==CommandKind::LogStop) {
        motor_core_stop_recording(); puts("Log stop requested; draining queued records."); return;
    }
    MotorRequest request;
    if (ecvt_app_motor_request(command,&request)) (void)motor_core_submit(request);

}

esp_err_t ecvt_app_start(bool storage_available)
{
    return motor_core_start(legacy_motor_services(storage_available));
}
