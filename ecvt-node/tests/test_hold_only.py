"""The actual bench runner must not send a rotation target in hold-only mode."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class HoldOnlyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
    def test_no_move_duration_cancel_fault_and_stale_feedback(self):
        headers = {
            "esp_err.h": """#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_TIMEOUT=3;
const char *esp_err_to_name(int);
""",
            "motor/runtime.h": """#pragma once
bool motor_operation_cancelled();
bool motor_position_allowed(float,float);
bool motor_operation_wait_ms(unsigned);
""",
        }
        headers.update({
            "motor/motor_core.h": "#pragma once\nvoid motor_core_record_command(float,float);\n",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
            "freertos/FreeRTOS.h": "#pragma once\n#define pdMS_TO_TICKS(ms) (ms)\n",
            "freertos/task.h": "#pragma once\nvoid vTaskDelay(unsigned);\n",
            "servo/ECVT_motor.h": """#pragma once
#include <cstdint>
#include "esp_err.h"
esp_err_t ecvt_motor_send_target_position_speed_accel_decel(float,float,float,float);
esp_err_t ecvt_motor_enable_position();
float ecvt_motor_get_actual_position_feedback();
esp_err_t ecvt_motor_sdo_read_u16(uint16_t,uint8_t,uint16_t*);
esp_err_t ecvt_motor_sdo_read_u32(uint16_t,uint8_t,uint32_t*);
""",
        })
        harness = r'''
#include "experiments/motor_tests.h"
#include "motor/runtime.h"
#include "servo/servo_feedback.h"
#include <cassert>
#include <cstdint>
static int64_t now=1000000, fault_at, cancel_at;
static unsigned target_commands, triggers;
static bool stale, read_failure, demand_mismatch;
const char *esp_err_to_name(int) { return "test"; }
int64_t esp_timer_get_time() { return now; }
bool motor_operation_cancelled() { return cancel_at && now>=cancel_at; }
bool motor_position_allowed(float,float) { return true; }
bool motor_operation_wait_ms(unsigned ms) { now+=ms*1000; return !motor_operation_cancelled(); }
void vTaskDelay(unsigned ms) { now+=ms*1000; }
int ecvt_motor_send_target_position_speed_accel_decel(float,float,float,float) { ++target_commands; return 0; }
int ecvt_motor_enable_position() { ++triggers; return 0; }
float ecvt_motor_get_actual_position_feedback() { return 0; }
void motor_core_record_command(float,float) {}
ServoFeedback servo_feedback_snapshot() {
    ServoFeedback sample;
    sample.statusword = fault_at && now>=fault_at ? 0x1218 : 0x0237;
    sample.status_us=stale ? now-300000 : now;
    sample.velocity_counts=1; // Resting jitter must not cause an unsolicited move.
    return sample;
}
int ecvt_motor_sdo_read_u16(uint16_t index,uint8_t sub,uint16_t *out) {
    assert(sub==0 && (index==0x6078 || index==0x603f));
    *out=index==0x603f ? 0xff2f : 0; return 0;
}
int ecvt_motor_sdo_read_u32(uint16_t index,uint8_t sub,uint32_t *out) {
    assert((index==0x6079 && sub==0) || (index==0x60fe && sub==1) || (index==0x607a && sub==0) || (index==0x6062 && sub==0));
    if(index==0x607a || index==0x6062) { *out=index==0x6062 && demand_mismatch ? 29255 : 0; return 0; }
    *out=index==0x6079 ? 47900 : 0x20000;
    return read_failure ? -1 : 0;
}
void reset() { now=1000000; target_commands=triggers=0; fault_at=cancel_at=0; stale=read_failure=demand_mismatch=false; }
int main() {
    reset(); assert(motor_test_run(MotorCommand::BenchHold,3,60,2300,2300)==0);
    assert(now==11000000 && target_commands==0 && triggers==0);
    reset(); cancel_at=1600000;
    assert(motor_test_run(MotorCommand::BenchHold,3,60,2300,2300)==1 && now==1600000 && target_commands==0 && triggers==0);
    reset(); fault_at=1400000;
    assert(motor_test_run(MotorCommand::BenchHold,3,60,2300,2300)==-1 && now==1400000 && target_commands==0 && triggers==0);
    reset(); stale=true;
    assert(motor_test_run(MotorCommand::BenchHold,3,60,2300,2300)==1 && now==1000000 && target_commands==0 && triggers==0);
    reset(); read_failure=true;
    assert(motor_test_run(MotorCommand::BenchHold,3,60,2300,2300)==-1 && now==1000000 && target_commands==0 && triggers==0);
    reset(); demand_mismatch=true;
    assert(motor_test_run(MotorCommand::BenchHold,3,60,2300,2300)==1 && now==1000000 && target_commands==0 && triggers==0);
}
'''
        with tempfile.TemporaryDirectory(prefix="ecvt-hold-only-") as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / "hold.cpp", tmp / "hold"
            source.write_text(harness)
            subprocess.run([
                "c++", "-std=c++17", "-I", str(tmp), "-I", str(PROJECT / "src"),
                str(PROJECT / "src/experiments/motor_tests.cpp"), str(source), "-o", str(binary),
            ], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("error_code=0xff2f", result.stdout)
