#include "console/command_parser.h"
#include "console/line_input.h"
#include "motor/limits.h"
#include "motor/operation_gate.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>

int main()
{
    ConsoleLineInput input;
    using LineResult = ConsoleLineInput::Result;
    for (char c : "servo inspect") {
        if (c) assert(input.push(c) == LineResult::Pending);
    }
    assert(input.push('\r') == LineResult::Ready && std::strcmp(input.text, "servo inspect") == 0);
    input.reset();
    assert(input.push('\n') == LineResult::Empty); // CRLF produces one command.
    input.reset();
    for (char c : "servo inspecx\bt\r") if (c) (void)input.push(c);
    assert(std::strcmp(input.text, "servo inspect") == 0);
    input.reset();
    for (char c : "servo inspect") if (c) (void)input.push(c);
    (void)input.push(0); // Never execute only the prefix before an embedded NUL.
    assert(input.push('\n') == LineResult::InvalidByte);
    input.reset();
    for (char c : "servo\x1b[A inspect") if (c) (void)input.push(c);
    assert(input.push('\n') == LineResult::InvalidByte);
    input.reset();
    for (unsigned i = 0; i < 200; ++i) (void)input.push('x');
    (void)input.push('\b'); // Overflow remains latched until delimiter.
    assert(input.push('\n') == LineResult::Overflow);
    input.reset();
    for (char c : "help") if (c) (void)input.push(c);
    assert(input.push('\n') == LineResult::Ready && std::strcmp(input.text, "help") == 0);
    input.reset();
    (void)input.push(0xff); // Observed UART byte must not be silently dropped before motion commands.
    for (char c : "servo inspect") if (c) (void)input.push(c);
    assert(input.push('\n') == LineResult::InvalidByte && input.length==14);
    input.reset();
    for (char c : "servo inspect") if (c) (void)input.push(c);
    assert(input.push('\n') == LineResult::Ready);

    ConsoleCommand command;
    const char *valid[] = {"help", "status", "servo ping", "servo inspect", "servo errors", "servo reset-fault", "servo brake-test", "servo brake-restore", "stop", "test cancel", "mode manual",
        "controller stop",
        "servo read 0x6040 0 u16", "servo read 5120 1 u32", "servo read 0x1001 0x00 u8", "servo cw 0x0006", "servo cw 7", "servo cw 0x000F", "servo velocity-zero", "servo torque-zero-test", "servo position-mode",

        "limits -6.9 0 3000", "profile 2300 2300", "move -1.25 1000", " \tmove 0 50\r\n"};
    for (const char *line : valid) assert(parse_console_command(line, &command));
    const char *invalid[] = {"mode auto", "controller start", "test list", "test run position-step", "test run three-revolutions", "servo hold-test", "test run position-profile", "test run step-response", "log start", "log stop", "", " ", "move", "move 1", "move 1 2 3", "move nan 1",
        "move 1 inf", "move 1e100 2", "move 1x 2", "limits 0 1", "profile 1 2x",
        "mode manual go", "test run torque", "test run position-profile extra", "test run three-revolutions 3", "stop now", "servo ping now", "servo inspect extra", "servo brake-test 10", "servo brake-restore extra",
        "servo read -1 0 u8", "servo read 65536 0 u32", "servo read 0x6040 256 u16", "servo read 0x6040 0 u64", "servo read 0x6040 0 u16 extra", "servo read 1.5 0 u16", "servo cw 0x003f", "servo cw -1", "servo cw 65542", "servo cw 6 extra"};
    for (const char *line : invalid) assert(!parse_console_command(line, &command));
    char overflow[170]; std::memset(overflow, 'x', sizeof(overflow)); overflow[169]=0;
    assert(!parse_console_command(overflow, &command));
    command.kind = CommandKind::Status;
    assert(!parse_console_command("move nan 1", &command) && command.kind == CommandKind::Status);
    assert(parse_console_command("move -1.25 1000", &command));
    assert(command.kind == CommandKind::Move && command.values[0] == -1.25f && command.values[1] == 1000);
    assert(parse_console_command("servo read 0x6040 0 u16", &command));
    assert(command.kind == CommandKind::ServoRead && command.values[0] == 0x6040 && command.values[1] == 0 && command.values[2] == 2);

    MotionLimits limits;
    assert(direct_bench_enable_allowed(0x0233, 1, 0, 75));
    assert(direct_bench_enable_allowed(0x0233, 3, 0, 0));
    assert(!direct_bench_enable_allowed(0x0233, 3, 0, 1));
    assert(!direct_bench_enable_allowed(0x0233, 4, 0, 0));
    assert(!direct_bench_enable_allowed(0x0233, 1, 1, 0));
    assert(!direct_bench_enable_allowed(0x023b, 1, 0, 0));
    assert(!direct_bench_enable_allowed(0x0250, 1, 0, 0));
    assert(!within_limits(limits, 0, 1000));
    assert(!valid_limits(1, 0, 1000) && !valid_limits(0, 1, 0));
    assert(!valid_limits(-300000, 1, 1000) && !valid_limits(0, 1, INFINITY));
    limits = {true, -6.9f, 0, 3000};
    assert(within_limits(limits, -6.9f, 3000) && within_limits(limits, 0, 1));
    assert(!within_limits(limits, -7, 100) && !within_limits(limits, 1, 100));
    assert(!within_limits(limits, 0, -1) && !within_limits(limits, NAN, 1));
    assert(!within_limits(limits, 0, 3001));

    OperationGate gate;
    assert(!gate.cancelled() && gate.admit());
    const auto queued_epoch = gate.stop_epoch.load();
    assert(!gate.admit()); // no second owner
    gate.stop();
    assert(gate.cancelled() && queued_epoch != gate.stop_epoch.load());
    assert(!gate.admit()); // cannot restart until stop has finished
    gate.active_epoch.store(gate.stop_epoch.load()); gate.busy.store(false);
    assert(!gate.cancelled() && gate.admit()); // explicit new request
    gate.stop(); assert(gate.cancelled());

    OperationGate race;
    std::atomic<unsigned> admitted{0};
    std::vector<std::thread> callers;
    for (unsigned i=0; i<16; ++i) callers.emplace_back([&]{ if (race.admit()) admitted.fetch_add(1); });
    for (auto &caller : callers) caller.join();
    assert(admitted.load() == 1);

}
