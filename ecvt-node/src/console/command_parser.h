#pragma once

enum class CommandKind {
    Help, Status, Stop, ModeManual, ModeAuto, AutoStart, Limits, Profile,
    Move, TestList, PositionStep, PositionProfile, StepResponse, LogStart, LogStop, ServoPing, ServoInspect,
    BrakeProbe, BrakeRestore, ThreeRevolutions, ServoErrors, ServoResetFault, ServoRead, BenchHold, ServoControlword, ServoVelocityZero, ServoTorqueZeroTest, ServoPositionMode
};

struct ConsoleCommand {
    CommandKind kind = CommandKind::Help;
    float values[3] = {};
};

// Rejects extra arguments, non-finite values and truncated commands.
bool parse_console_command(const char *line, ConsoleCommand *command);
