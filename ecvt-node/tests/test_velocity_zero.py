"""Verify zero-velocity preparation order, refusals, and absence of enable writes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class VelocityZeroTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
    def test_prepare_without_controlword_or_nonzero_speed(self):
        headers = {
            "esp_err.h": """#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_TIMEOUT=3;
""",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
            "app/ecvt_app.h": "#pragma once\nbool ecvt_app_stop_requested();\nbool ecvt_app_wait_ms(unsigned);\n",
            "servo/ECVT_motor.h": """#pragma once
#include <cstdint>
#include "esp_err.h"
esp_err_t ecvt_motor_sdo_read_u8(uint16_t,uint8_t,uint8_t*);
esp_err_t ecvt_motor_sdo_read_u16(uint16_t,uint8_t,uint16_t*);
esp_err_t ecvt_motor_sdo_read_u32(uint16_t,uint8_t,uint32_t*);
esp_err_t ecvt_motor_sdo_write_u8(uint16_t,uint8_t,uint8_t);
esp_err_t ecvt_motor_sdo_write_u32(uint16_t,uint8_t,uint32_t);
""",
        }
        harness = r'''
#include "servo/velocity_zero.h"
#include <cassert>
#include <cstdint>
static uint16_t status=0x0250;
static uint8_t errors, mode=1;
static uint32_t target=12345, demand;
static unsigned writes, fail_write, cancel_write;
static bool cancelled, mode_stuck, bad_target;
static int64_t now;
int64_t esp_timer_get_time() { return now; }
bool ecvt_app_stop_requested() { return cancelled; }
bool ecvt_app_wait_ms(unsigned ms) { now+=ms*1000; return !cancelled; }
int ecvt_motor_sdo_read_u8(uint16_t index,uint8_t sub,uint8_t *out) {
    assert(sub==0 && (index==0x1001 || index==0x6061));
    *out=index==0x1001 ? errors : mode; return 0;
}
int ecvt_motor_sdo_read_u16(uint16_t index,uint8_t sub,uint16_t *out) {
    assert(index==0x6041 && sub==0); *out=status; return 0;
}
int ecvt_motor_sdo_read_u32(uint16_t index,uint8_t sub,uint32_t *out) {
    assert(sub==0 && index==0x60ff);
    *out=bad_target ? 5 : target; return 0;
}
int ecvt_motor_sdo_write_u32(uint16_t index,uint8_t sub,uint32_t value) {
    assert(index==0x60ff && sub==0 && value==0 && writes==0);
    target=value; ++writes;
    if(writes==cancel_write) cancelled=true;
    return writes==fail_write ? -1 : 0;
}
int ecvt_motor_sdo_write_u8(uint16_t index,uint8_t sub,uint8_t value) {
    assert(index==0x6060 && sub==0 && value==3 && writes==1 && target==0);
    if(!mode_stuck) mode=value;
    ++writes;
    return writes==fail_write ? -1 : 0;
}
void reset() {
    status=0x0250; errors=0; mode=1; target=12345; demand=0;
    writes=fail_write=cancel_write=0; cancelled=mode_stuck=bad_target=false; now=0;
}
int main() {
    reset(); assert(servo_prepare_velocity_zero()==0 && writes==2 && mode==3 && target==0);
    reset(); status=0x0237; assert(servo_prepare_velocity_zero()==1 && writes==0);
    reset(); status=0x1218; assert(servo_prepare_velocity_zero()==1 && writes==0);
    reset(); errors=1; assert(servo_prepare_velocity_zero()==1 && writes==0);
    reset(); cancelled=true; assert(servo_prepare_velocity_zero()==1 && writes==0);
    reset(); cancel_write=1; assert(servo_prepare_velocity_zero()==1 && writes==1 && mode==1);
    reset(); fail_write=1; assert(servo_prepare_velocity_zero()==-1 && writes==1);
    reset(); fail_write=2; assert(servo_prepare_velocity_zero()==-1 && writes==2);
    reset(); mode_stuck=true; assert(servo_prepare_velocity_zero()==3 && writes==2 && now>=500000);
    reset(); demand=1; assert(servo_prepare_velocity_zero()==0 && writes==2);
    reset(); bad_target=true; assert(servo_prepare_velocity_zero()==1 && writes==2);
}
'''
        with tempfile.TemporaryDirectory(prefix="ecvt-velocity-zero-") as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / "zero.cpp", tmp / "zero"
            source.write_text(harness)
            subprocess.run([
                "c++", "-std=c++17", "-I", str(tmp), "-I", str(PROJECT / "src"),
                str(PROJECT / "src/servo/velocity_zero.cpp"), str(source), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, capture_output=True)
