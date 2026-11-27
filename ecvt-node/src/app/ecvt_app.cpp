#include "ecvt_app.h"
#include "motor/motor_core.h"
#include "console/serial_console.h"
#include "esp_timer.h"
#include <cstdio>
#include <cinttypes>

void ecvt_app_print_status()
{
    const auto snapshot=motor_core_status();
    const auto &feedback=snapshot.feedback;
    const char *states[] = {"idle", "busy", "drive-enabled", "DISABLE-UNCONFIRMED"};
    printf("mode=manual state=%s busy=%d error=%s brake=UNVERIFIED\n",
        states[static_cast<unsigned>(snapshot.state)], snapshot.busy, esp_err_to_name(snapshot.error));
    printf("statusword=0x%04x position_counts=%" PRIi32 " velocity_counts=%" PRIi32
           " feedback_age_ms=%lld\n", feedback.statusword, feedback.position_counts, feedback.velocity_counts,
           feedback.status_us ? (long long)((esp_timer_get_time() - feedback.status_us)/1000) : -1LL);
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
    case CommandKind::Limits: request.kind=MotorCommand::Limits; break;
    case CommandKind::Profile: request.kind=MotorCommand::Profile; break;
    case CommandKind::Move: request.kind=MotorCommand::Move; break;
    case CommandKind::ServoPing: request.kind=MotorCommand::ServoPing; break;
    case CommandKind::ServoInspect: request.kind=MotorCommand::ServoInspect; break;
    case CommandKind::BrakeProbe: request.kind=MotorCommand::BrakeProbe; break;
    case CommandKind::BrakeRestore: request.kind=MotorCommand::BrakeRestore; break;
    case CommandKind::ServoErrors: request.kind=MotorCommand::ServoErrors; break;
    case CommandKind::ServoResetFault: request.kind=MotorCommand::ServoResetFault; break;
    case CommandKind::ServoRead: request.kind=MotorCommand::ServoRead; break;
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
    MotorRequest request;
    if (ecvt_app_motor_request(command,&request)) (void)motor_core_submit(request);

}

esp_err_t ecvt_app_start()
{
    return motor_core_start();
}
