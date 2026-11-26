"""Exercise production stop routing, owner cleanup and CANopen disable code."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class StopTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'Host C++ compiler unavailable')
    def test_disable_bypasses_admission_and_sends_before_any_read(self):
        # Compile the actual owner finish and submission functions, not copies
        # of their policy. The remaining owner/FreeRTOS environment is simulated.
        app = (PROJECT / 'src/app/ecvt_app.cpp').read_text()
        finish = app[app.index('void finish_operation()'):app.index('void replay_task(')]
        submit = app[app.index('void ecvt_app_submit('):app.index('esp_err_t ecvt_app_start(')]
        controlword = app[app.index('        case CommandKind::ServoControlword:'):app.index('        case CommandKind::ServoRead:')]
        headers = {
            'esp_err.h': '#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_TIMEOUT=3;\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'freertos/FreeRTOS.h': '#pragma once\n#define pdMS_TO_TICKS(x) (x)\n',
            'freertos/task.h': '#pragma once\nvoid vTaskDelay(unsigned);\n',
            'app/ecvt_app.h': '#pragma once\nbool ecvt_app_stop_requested();\nbool ecvt_app_wait_ms(unsigned);\n',
            'servo/ECVT_motor.h': '''#pragma once
#include <cstdint>
#include "esp_err.h"
esp_err_t ecvt_motor_sdo_read_u8(uint16_t,uint8_t,uint8_t*);
esp_err_t ecvt_motor_sdo_read_u16(uint16_t,uint8_t,uint16_t*);
esp_err_t ecvt_motor_sdo_read_u32(uint16_t,uint8_t,uint32_t*);
esp_err_t ecvt_motor_sdo_write_u16(uint16_t,uint8_t,uint16_t);
esp_err_t ecvt_motor_sdo_write_u32(uint16_t,uint8_t,uint32_t);
''',
        }
        harness = r'''
#include "experiments/brake_probe.h"
#include "app/operation_gate.h"
#include "app/operating_policy.h"
#include "console/command_parser.h"
#include "freertos/FreeRTOS.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
static OperationGate gate;
static auto &stop_epoch=gate.stop_epoch;
static auto &active_epoch=gate.active_epoch;
static auto &busy=gate.busy;
enum class RunState {Idle, Busy, Holding, StopUnconfirmed};
static std::atomic<RunState> state{RunState::Idle};
static std::atomic<bool> automatic{false}, replay_alive{false};
static std::atomic<int> last_error{0};
static bool drive_touched, s_sample_rpm_enabled, sd_available;
struct PendingCommand { ConsoleCommand command; unsigned epoch; };
static int commands=1;
static uint16_t status;
static bool write_failure, read_failure, stuck, replay_was_waited;
static int64_t now;
static unsigned writes, reads, queued, resets;
static std::vector<std::string> events;
static bool allow_enable;
static uint8_t mode=3;
static uint32_t target_velocity;
const char *esp_err_to_name(int) {return "test";}
void serial_console_print_help() {}
void ecvt_app_print_status() {}
int logger_session_start() {return 0;}
void logger_session_stop() {}
void xQueueReset(int queue) {assert(queue==commands); queued=0; ++resets;}
int xQueueSend(int queue, const PendingCommand*, int) {assert(queue==commands); ++queued; return 1;}
constexpr int pdTRUE=1;
int64_t esp_timer_get_time() {return now;}
void vTaskDelay(unsigned ms) {
    now+=ms*1000;
    if(replay_alive.load()) {
        assert(writes>0); // Disable is sent before waiting for replay/SD cleanup.
        replay_was_waited=true; replay_alive.store(false);
    }
}
bool ecvt_app_stop_requested() {return gate.cancelled();}
bool ecvt_app_wait_ms(unsigned ms) {now+=ms*1000; return !gate.cancelled();}
int servo_prepare_velocity_zero() {assert(false); return -1;}
int ecvt_motor_sdo_read_u8(uint16_t index,uint8_t sub,uint8_t *out) {
    assert(allow_enable && sub==0 && (index==0x6061 || index==0x1001));
    *out=index==0x6061 ? mode : 0; return 0;
}
int ecvt_motor_sdo_read_u32(uint16_t index,uint8_t sub,uint32_t *out) {
    assert(allow_enable && index==0x60ff && sub==0); *out=target_velocity; return 0;
}
int ecvt_motor_sdo_write_u32(uint16_t index,uint8_t sub,uint32_t value) {
    assert(allow_enable && index==0x60ff && sub==0 && value==0);
    events.push_back("zero"); target_velocity=0; return 0;
}
int ecvt_motor_sdo_write_u16(uint16_t index,uint8_t sub,uint16_t value) {
    assert(index==0x6040 && sub==0);
    if(allow_enable && value==15) {
        assert(target_velocity==0 && events.back()=="zero");
        events.push_back("enable"); ++writes; status=0x0237; return 0;
    }
    assert(value==0); // No Halt or enable on any stop.
    events.push_back("disable"); ++writes;
    if(!stuck && !(status & 8)) status=0x0250;
    return write_failure ? -1 : 0; // Can fail after the drive accepted it.
}
int ecvt_motor_sdo_read_u16(uint16_t index,uint8_t sub,uint16_t *out) {
    assert(index==0x6041 && sub==0 && (writes>0 || allow_enable));
    events.push_back("status"); ++reads; *out=status;
    return read_failure ? -1 : 0;
}
''' + finish + submit + '\nvoid execute_controlword(const ConsoleCommand &command) { switch(command.kind) {\n' + controlword + '\ndefault: assert(false); } }\n' + r'''
void reset() {
    assert(!brake_probe_restore_pending());
    gate.stop_epoch.store(0); gate.active_epoch.store(0); busy.store(false);
    state.store(RunState::Idle); automatic.store(false); replay_alive.store(false);
    drive_touched=false; s_sample_rpm_enabled=false; sd_available=false;
    last_error.store(0); status=0x0637; write_failure=read_failure=stuck=replay_was_waited=false;
    now=0; writes=reads=queued=resets=0; events.clear();
    allow_enable=false; mode=3; target_velocity=123;
}
void request(const char *line) {
    ConsoleCommand command; assert(parse_console_command(line,&command));
    ecvt_app_submit(command);
    assert(gate.cancelled() && busy.load() && queued==0 && writes==0);
    // This is the worker's cancellation branch, before queued-command guards.
    finish_operation();
    assert(!gate.cancelled() && !busy.load() && resets==1);
}
int main() {
    for(const char *line : {"stop", "controller stop", "test cancel", "servo cw 0x0000", "servo cw 0"}) {
        reset(); request(line); // MCU-only reboot: drive enabled, local touched=false.
        assert(writes==1 && reads==1 && events.front()=="disable");
        assert(state.load()==RunState::Idle && !brake_probe_restore_pending());
        reset(); automatic.store(true); busy.store(true); drive_touched=true;
        state.store(RunState::Holding); replay_alive.store(true);
        request(line);
        assert(writes==1 && replay_was_waited && state.load()==RunState::Idle && !drive_touched);
    }
    reset(); status=0x0250; request("stop"); assert(writes==1); // Even if already disabled.
    reset(); write_failure=true; request("stop"); // Lost acknowledgement, live state proves disable.
    assert(state.load()==RunState::Idle && !brake_probe_restore_pending());
    reset(); read_failure=true; request("stop");
    assert(writes==1 && state.load()==RunState::StopUnconfirmed && brake_probe_restore_pending());
    read_failure=false; writes=0; resets=0; events.clear();
    automatic.store(true); busy.store(true); request("servo cw 0x0000");
    assert(writes==1 && state.load()==RunState::Idle && !brake_probe_restore_pending());
    reset(); stuck=true; request("stop");
    assert(writes==1 && reads>1 && now>=1000000 && state.load()==RunState::StopUnconfirmed);
    stuck=false; assert(brake_probe_restore()==0); // Explicit retry clears the pending latch.
    reset(); status=0x1218; request("stop"); // Fault does not prevent the disable write.
    assert(writes==1 && last_error.load()!=0 && state.load()==RunState::StopUnconfirmed);
    status=0x0250; assert(brake_probe_restore()==0);
    reset(); finish_operation(); // Normal runner completion uses the same disable path.
    assert(writes==1 && state.load()==RunState::Idle);
    reset(); allow_enable=true; status=0x0233;
    ConsoleCommand command; assert(parse_console_command("servo cw 0x000F",&command));
    execute_controlword(command);
    assert(writes==1 && status==0x0237 && target_velocity==0 && drive_touched);
    command.values[0]=0; execute_controlword(command); // Defensive queued-zero path.
    assert(writes==2 && status==0x0250 && !drive_touched && state.load()==RunState::Idle);
}
'''
        with tempfile.TemporaryDirectory(prefix='ecvt-stop-') as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / 'stop.cpp', tmp / 'stop'
            source.write_text(harness)
            subprocess.run([
                'c++', '-std=c++17', '-I', str(tmp), '-I', str(PROJECT / 'src'),
                str(PROJECT / 'src/servo/drive_state.cpp'),
                str(PROJECT / 'src/experiments/brake_probe.cpp'),
                str(PROJECT / 'src/console/command_parser.cpp'), str(source), '-o', str(binary),
            ], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
            self.assertIn('Drive disable confirmed', result.stdout)
            self.assertIn('DISABLE UNCONFIRMED', result.stdout)
