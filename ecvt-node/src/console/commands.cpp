// Parse manual commands and translate them to drive requests or local status.
// Parsing rejects malformed input before anything is submitted to the owner.
#include "drive/control.h"
#include "esp_timer.h"
#include <cstdio>
#include <cinttypes>
#include "console.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

bool parse_console_command(const char *line, ConsoleCommand *command)
{
    if (!line || !command || std::strlen(line) >= 160)
        return false;
    char copy[160];
    std::strcpy(copy, line);
    char *tokens[6] = {};
    char *save = nullptr;
    unsigned count = 0;
    for (char *token = strtok_r(copy, " \t\r\n", &save); token;
         token = strtok_r(nullptr, " \t\r\n", &save)) {
        if (count == 6)
            return false;
        tokens[count++] = token;
    }
    if (count == 0)
        return false;
    ConsoleCommand parsed;
    auto is = [&](unsigned i, const char *text) {
        return i < count && std::strcmp(tokens[i], text) == 0;
    };
    unsigned numbers = 0;
    if (count == 1 && is(0, "help"))
        parsed.kind = CommandKind::Help;
    else if (count == 1 && is(0, "status"))
        parsed.kind = CommandKind::Status;
    else if (count == 2 && is(0, "servo") && is(1, "ping"))
        parsed.kind = CommandKind::ServoPing;
    else if (count == 2 && is(0, "servo") && is(1, "inspect"))
        parsed.kind = CommandKind::ServoInspect;
    else if (count == 2 && is(0, "servo") && is(1, "errors"))
        parsed.kind = CommandKind::ServoErrors;
    else if (count == 2 && is(0, "servo") && is(1, "reset-fault"))
        parsed.kind = CommandKind::ServoResetFault;
    else if (count == 2 && is(0, "servo") && is(1, "velocity-zero"))
        parsed.kind = CommandKind::ServoVelocityZero;
    else if (count == 2 && is(0, "servo") && is(1, "position-mode"))
        parsed.kind = CommandKind::ServoPositionMode;
    else if (count == 2 && is(0, "servo") && is(1, "torque-zero-test"))
        parsed.kind = CommandKind::ServoTorqueZeroTest;
    else if (count == 3 && is(0, "servo") && is(1, "cw")) {
        char *end = nullptr;
        errno = 0;
        if (*tokens[2] == '-' || *tokens[2] == '+')
            return false;
        const unsigned long word = std::strtoul(tokens[2], &end, 0);
        if (end == tokens[2] || *end || errno == ERANGE ||
            (word != 0 && word != 6 && word != 7 && word != 15))
            return false;
        parsed.kind = CommandKind::ServoControlword;
        parsed.values[0] = static_cast<float>(word);
    } else if (count == 5 && is(0, "servo") && is(1, "read")) {
        parsed.kind = CommandKind::ServoRead;
        for (unsigned i = 0; i < 2; ++i) {
            const char *token = tokens[i + 2];
            if (*token == '-' || *token == '+')
                return false;
            char *end = nullptr;
            errno = 0;
            const int base = token[0] == '0' && (token[1] == 'x' || token[1] == 'X') ? 16 : 10;
            const unsigned long value = std::strtoul(token, &end, base);
            if (end == token || *end || errno == ERANGE || value > (i == 0 ? 65535UL : 255UL))
                return false;
            parsed.values[i] = static_cast<float>(value);
        }
        parsed.values[2] = is(4, "u8") ? 1 : is(4, "u16") ? 2 : is(4, "u32") ? 4 : 0;
        if (!parsed.values[2])
            return false;
    } else if (count == 2 && is(0, "servo") && is(1, "brake-test"))
        parsed.kind = CommandKind::BrakeProbe;
    else if (count == 2 && is(0, "servo") && is(1, "brake-restore"))
        parsed.kind = CommandKind::BrakeRestore;
    else if ((count == 1 && is(0, "stop")) || (count == 2 && is(0, "test") && is(1, "cancel")))
        parsed.kind = CommandKind::Stop;
    else if (count == 2 && is(0, "mode") && is(1, "manual"))
        parsed.kind = CommandKind::ModeManual;
    else if (count == 2 && is(0, "controller") && is(1, "stop"))
        parsed.kind = CommandKind::Stop;
    else if (count == 4 && is(0, "limits")) {
        parsed.kind = CommandKind::Limits;
        numbers = 3;
    } else if (count == 3 && is(0, "profile")) {
        parsed.kind = CommandKind::Profile;
        numbers = 2;
    } else if (count == 3 && is(0, "move")) {
        parsed.kind = CommandKind::Move;
        numbers = 2;
    } else
        return false;
    for (unsigned i = 0; i < numbers; ++i) {
        char *end = nullptr;
        errno = 0;
        parsed.values[i] = std::strtof(tokens[i + 1], &end);
        if (end == tokens[i + 1] || *end || errno == ERANGE || !std::isfinite(parsed.values[i]))
            return false;
    }
    *command = parsed;
    return true;
}

// Present cached owner/feedback data, including its age; no new SDO read is issued.
void console_print_status()
{
    const auto snapshot = drive_control_status();
    const auto &feedback = snapshot.feedback;
    const char *states[] = {"idle", "busy", "drive-enabled", "DISABLE-UNCONFIRMED"};
    printf("mode=manual state=%s busy=%d error=%s brake=UNVERIFIED\n",
           states[static_cast<unsigned>(snapshot.state)], snapshot.busy,
           esp_err_to_name(snapshot.error));
    printf("statusword=0x%04x position_counts=%" PRIi32 " velocity_counts=%" PRIi32
           " feedback_age_ms=%lld\n",
           feedback.statusword, feedback.position_counts, feedback.velocity_counts,
           feedback.status_us ? (long long)((esp_timer_get_time() - feedback.status_us) / 1000)
                              : -1LL);
    printf("brake_test_disable=%s\n",
           snapshot.disable_pending ? "UNCONFIRMED" : "no pending disable");
}

bool console_drive_request(const ConsoleCommand &command, DriveRequest *output)
{
    if (!output)
        return false;
    DriveRequest request;
    for (unsigned i = 0; i < 3; ++i)
        request.values[i] = command.values[i];
    switch (command.kind) {
    case CommandKind::Stop:
        request.kind = DriveCommand::Stop;
        break;
    case CommandKind::ModeManual:
        request.kind = DriveCommand::ModeManual;
        break;
    case CommandKind::Limits:
        request.kind = DriveCommand::Limits;
        break;
    case CommandKind::Profile:
        request.kind = DriveCommand::Profile;
        break;
    case CommandKind::Move:
        request.kind = DriveCommand::Move;
        break;
    case CommandKind::ServoPing:
        request.kind = DriveCommand::ServoPing;
        break;
    case CommandKind::ServoInspect:
        request.kind = DriveCommand::ServoInspect;
        break;
    case CommandKind::BrakeProbe:
        request.kind = DriveCommand::BrakeProbe;
        break;
    case CommandKind::BrakeRestore:
        request.kind = DriveCommand::BrakeRestore;
        break;
    case CommandKind::ServoErrors:
        request.kind = DriveCommand::ServoErrors;
        break;
    case CommandKind::ServoResetFault:
        request.kind = DriveCommand::ServoResetFault;
        break;
    case CommandKind::ServoRead:
        request.kind = DriveCommand::ServoRead;
        break;
    case CommandKind::ServoControlword:
        request.kind = DriveCommand::ServoControlword;
        break;
    case CommandKind::ServoVelocityZero:
        request.kind = DriveCommand::ServoVelocityZero;
        break;
    case CommandKind::ServoTorqueZeroTest:
        request.kind = DriveCommand::ServoTorqueZeroTest;
        break;
    case CommandKind::ServoPositionMode:
        request.kind = DriveCommand::ServoPositionMode;
        break;
    default:
        return false;
    }
    if (command.kind == CommandKind::ServoRead) {
        request.object_count = 1;
        request.objects[0] = {
            static_cast<uint16_t>(command.values[0]), static_cast<uint8_t>(command.values[1]),
            static_cast<ObjectWidth>(static_cast<unsigned>(command.values[2])), 0};
    }
    *output = request;
    return true;
}

void console_submit(const ConsoleCommand &command)
{
    if (command.kind == CommandKind::Help) {
        serial_console_print_help();
        return;
    }
    if (command.kind == CommandKind::Status) {
        console_print_status();
        return;
    }
    DriveRequest request;
    if (console_drive_request(command, &request))
        (void)drive_control_submit(request);
}
