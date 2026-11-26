#include "command_parser.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

bool parse_console_command(const char *line, ConsoleCommand *command)
{
    if (!line || !command || std::strlen(line) >= 160) return false;
    char copy[160];
    std::strcpy(copy, line);
    char *tokens[6] = {};
    char *save = nullptr;
    unsigned count = 0;
    for (char *token = strtok_r(copy, " \t\r\n", &save); token;
         token = strtok_r(nullptr, " \t\r\n", &save)) {
        if (count == 6) return false;
        tokens[count++] = token;
    }
    if (count == 0) return false;
    ConsoleCommand parsed;
    auto is = [&](unsigned i, const char *text) {
        return i < count && std::strcmp(tokens[i], text) == 0;
    };
    unsigned numbers = 0;
    if (count == 1 && is(0, "help")) parsed.kind = CommandKind::Help;
    else if (count == 1 && is(0, "status")) parsed.kind = CommandKind::Status;
    else if (count == 2 && is(0, "servo") && is(1, "ping")) parsed.kind = CommandKind::ServoPing;
    else if (count == 2 && is(0, "servo") && is(1, "inspect")) parsed.kind = CommandKind::ServoInspect;
    else if (count == 2 && is(0, "servo") && is(1, "errors")) parsed.kind = CommandKind::ServoErrors;
    else if (count == 2 && is(0, "servo") && is(1, "reset-fault")) parsed.kind = CommandKind::ServoResetFault;
    else if (count == 2 && is(0, "servo") && is(1, "hold-test")) parsed.kind = CommandKind::BenchHold;
    else if (count == 2 && is(0, "servo") && is(1, "velocity-zero")) parsed.kind = CommandKind::ServoVelocityZero;
    else if (count == 2 && is(0, "servo") && is(1, "position-mode")) parsed.kind = CommandKind::ServoPositionMode;
    else if (count == 2 && is(0, "servo") && is(1, "torque-zero-test")) parsed.kind = CommandKind::ServoTorqueZeroTest;
    else if (count == 3 && is(0, "servo") && is(1, "cw")) {
        char *end = nullptr;
        errno = 0;
        if (*tokens[2] == '-' || *tokens[2] == '+') return false;
        const unsigned long word = std::strtoul(tokens[2], &end, 0);
        if (end == tokens[2] || *end || errno == ERANGE ||
            (word != 0 && word != 6 && word != 7 && word != 15)) return false;
        parsed.kind = CommandKind::ServoControlword;
        parsed.values[0] = static_cast<float>(word);
    }
    else if (count == 5 && is(0, "servo") && is(1, "read")) {
        parsed.kind = CommandKind::ServoRead;
        for (unsigned i = 0; i < 2; ++i) {
            const char *token = tokens[i + 2];
            if (*token == '-' || *token == '+') return false;
            char *end = nullptr;
            errno = 0;
            const int base = token[0] == '0' && (token[1] == 'x' || token[1] == 'X') ? 16 : 10;
            const unsigned long value = std::strtoul(token, &end, base);
            if (end == token || *end || errno == ERANGE || value > (i == 0 ? 65535UL : 255UL)) return false;
            parsed.values[i] = static_cast<float>(value);
        }
        parsed.values[2] = is(4, "u8") ? 1 : is(4, "u16") ? 2 : is(4, "u32") ? 4 : 0;
        if (!parsed.values[2]) return false;
    }
    else if (count == 2 && is(0, "servo") && is(1, "brake-test")) parsed.kind = CommandKind::BrakeProbe;
    else if (count == 2 && is(0, "servo") && is(1, "brake-restore")) parsed.kind = CommandKind::BrakeRestore;
    else if ((count == 1 && is(0, "stop")) ||
             (count == 2 && is(0, "test") && is(1, "cancel"))) parsed.kind = CommandKind::Stop;
    else if (count == 2 && is(0, "mode") && is(1, "manual")) parsed.kind = CommandKind::ModeManual;
    else if (count == 2 && is(0, "mode") && is(1, "auto")) parsed.kind = CommandKind::ModeAuto;
    else if (count == 2 && is(0, "controller") && is(1, "start")) parsed.kind = CommandKind::AutoStart;
    else if (count == 2 && is(0, "controller") && is(1, "stop")) parsed.kind = CommandKind::Stop;
    else if (count == 4 && is(0, "limits")) { parsed.kind = CommandKind::Limits; numbers = 3; }
    else if (count == 3 && is(0, "profile")) { parsed.kind = CommandKind::Profile; numbers = 2; }
    else if (count == 3 && is(0, "move")) { parsed.kind = CommandKind::Move; numbers = 2; }
    else if (count == 2 && is(0, "test") && is(1, "list")) parsed.kind = CommandKind::TestList;
    else if (count == 3 && is(0, "test") && is(1, "run") && is(2, "position-step")) parsed.kind = CommandKind::PositionStep;
    else if (count == 3 && is(0, "test") && is(1, "run") && is(2, "position-profile")) parsed.kind = CommandKind::PositionProfile;
    else if (count == 3 && is(0, "test") && is(1, "run") && is(2, "step-response")) parsed.kind = CommandKind::StepResponse;
    else if (count == 3 && is(0, "test") && is(1, "run") && is(2, "three-revolutions")) parsed.kind = CommandKind::ThreeRevolutions;
    else if (count == 2 && is(0, "log") && is(1, "start")) parsed.kind = CommandKind::LogStart;
    else if (count == 2 && is(0, "log") && is(1, "stop")) parsed.kind = CommandKind::LogStop;
    else return false;
    for (unsigned i = 0; i < numbers; ++i) {
        char *end = nullptr;
        errno = 0;
        parsed.values[i] = std::strtof(tokens[i + 1], &end);
        if (end == tokens[i + 1] || *end || errno == ERANGE || !std::isfinite(parsed.values[i])) return false;
    }
    *command = parsed;
    return true;
}
