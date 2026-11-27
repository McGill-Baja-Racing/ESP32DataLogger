// Serial command types, line framing and the console entry points.
// Commands adapt to drive/control; this interface never owns CAN transactions.
#pragma once
#include <cstddef>
#include "esp_err.h"

enum class CommandKind {
    Help,
    Status,
    Stop,
    ModeManual,
    Limits,
    Profile,
    Move,
    ServoPing,
    ServoInspect,
    BrakeProbe,
    BrakeRestore,
    ServoErrors,
    ServoResetFault,
    ServoRead,
    ServoControlword,
    ServoVelocityZero,
    ServoTorqueZeroTest,
    ServoPositionMode
};

struct ConsoleCommand {
    CommandKind kind = CommandKind::Help;
    float values[3] = {};
};

// Rejects extra arguments, non-finite values and truncated commands.
bool parse_console_command(const char *line, ConsoleCommand *command);

// Stream framing independent of stdio read boundaries. Never parse a truncated
// line or a line containing NUL/terminal escape sequences as a valid command.
struct ConsoleLineInput {
    enum class Result { Pending, Empty, Ready, Overflow, InvalidByte };
    char text[160] = {};
    std::size_t length = 0;
    bool overflow = false, invalid = false;

    Result push(unsigned char c)
    {
        if (c == '\r' || c == '\n') {
            text[length] = '\0';
            return overflow  ? Result::Overflow
                   : invalid ? Result::InvalidByte
                   : length  ? Result::Ready
                             : Result::Empty;
        }
        if (c == '\b' || c == 127) {
            if (!overflow && length)
                --length;
        } else if (!overflow) {
            if (length + 1 >= sizeof(text))
                overflow = true;
            else {
                if ((c < 32 && c != '\t') || c > 126)
                    invalid = true;
                text[length++] = static_cast<char>(c);
            }
        }
        return Result::Pending;
    }

    void reset()
    {
        length = 0;
        overflow = invalid = false;
        text[0] = '\0';
    }
};

struct DriveRequest;
// Map parsed commands to the public drive API; help/status stay local.
bool console_drive_request(const ConsoleCommand &command, DriveRequest *request);
// Help/status execute locally; other commands submit to the single drive owner.
void console_submit(const ConsoleCommand &command);
void console_print_status();
void serial_console_print_help();
// Start UART reception once; opening the console does not enable the drive.
esp_err_t serial_console_start();
