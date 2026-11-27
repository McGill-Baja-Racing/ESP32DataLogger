#pragma once

enum class CommandKind {
    Help, Status, Stop, ModeManual, Limits, Profile,
    Move, ServoPing, ServoInspect,
    BrakeProbe, BrakeRestore, ServoErrors, ServoResetFault, ServoRead, ServoControlword, ServoVelocityZero, ServoTorqueZeroTest, ServoPositionMode
};

struct ConsoleCommand {
    CommandKind kind = CommandKind::Help;
    float values[3] = {};
};

// Rejects extra arguments, non-finite values and truncated commands.
bool parse_console_command(const char *line, ConsoleCommand *command);
