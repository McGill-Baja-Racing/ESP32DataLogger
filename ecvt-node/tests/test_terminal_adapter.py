"""The real terminal adapter maps to the public motor API without drive access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT=Path(__file__).resolve().parents[1]

class TerminalAdapterTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'Host C++ compiler unavailable')
    def test_mapping_and_ui_only_commands(self):
        harness=r'''
#include "app/ecvt_app.h"
#include "motor/motor_core.h"
#include <cassert>
static unsigned submissions, help, recording;
static MotorRequest submitted;
const char *esp_err_to_name(int) {return "test";}
int64_t esp_timer_get_time() {return 0;}
bool motor_operation_cancelled() {return false;}
bool motor_operation_wait_ms(unsigned) {return true;}
bool motor_position_allowed(float,float) {return true;}
void motor_operation_error(int) {}
MotorCoreServices legacy_motor_services(bool) {return {};}
int motor_core_start(const MotorCoreServices&) {return 0;}
int motor_core_start_recording() {++recording; return 0;}
void motor_core_stop_recording() {++recording;}
MotorStatus motor_core_status() {return {};}
MotorReceipt motor_core_submit(const MotorRequest &request) {++submissions;submitted=request;return {0,submissions};}
void serial_console_print_help() {++help;}
int main() {
    ConsoleCommand command; MotorRequest request;
    const char *local[]={"help","status","test list","log start","log stop"};
    for(auto text:local) {
        assert(parse_console_command(text,&command));
        assert(!ecvt_app_motor_request(command,&request)); ecvt_app_submit(command);
    }
    assert(submissions==0 && help==1 && recording==2);
    assert(parse_console_command("move 0.25 10",&command));ecvt_app_submit(command);
    assert(submitted.kind==MotorCommand::Move && submitted.values[0]==0.25f && submitted.values[1]==10);
    for(auto text:{"stop","controller stop","test cancel"}) {
        assert(parse_console_command(text,&command));ecvt_app_submit(command);assert(submitted.kind==MotorCommand::Stop);
    }
    assert(parse_console_command("servo cw 0x0000",&command));ecvt_app_submit(command);
    assert(submitted.kind==MotorCommand::ServoControlword && submitted.values[0]==0);
    assert(parse_console_command("servo read 0x6064 0 u32",&command));ecvt_app_submit(command);
    assert(submitted.kind==MotorCommand::ServoRead && submitted.object_count==1);
    assert(submitted.objects[0].index==0x6064 && submitted.objects[0].width==ObjectWidth::U32);
    assert(!ecvt_app_motor_request(command,nullptr));
}
'''
        with tempfile.TemporaryDirectory(prefix='ecvt-terminal-adapter-') as directory:
            tmp=Path(directory)
            (tmp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0;\nconst char *esp_err_to_name(int);\n')
            (tmp/'esp_timer.h').write_text('#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n')
            cpp=tmp/'adapter.cpp';cpp.write_text('#include <initializer_list>\n'+harness)
            binary=tmp/'adapter'
            subprocess.run(['c++','-std=c++17','-I',str(tmp),'-I',str(PROJECT/'src'),
                            str(PROJECT/'src/app/ecvt_app.cpp'),str(PROJECT/'src/console/command_parser.cpp'),
                            str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],capture_output=True,check=True)
