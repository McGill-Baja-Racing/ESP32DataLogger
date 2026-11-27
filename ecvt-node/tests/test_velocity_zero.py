"""Verify zero-velocity preparation order, refusals, and absence of enable writes."""
from pathlib import Path
import shutil
import re
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
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_TIMEOUT=3;\nconst char *esp_err_to_name(int);
""",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
            "esp_twai.h": "#pragma once\nusing twai_node_handle_t=void*;\n",
            "freertos/queue.h": "#pragma once\nusing QueueHandle_t=void*;\n",
        }
        headers["freertos/FreeRTOS.h"] = "#pragma once\n#define pdMS_TO_TICKS(x) (x)\n"
        headers["freertos/task.h"] = "#pragma once\nvoid vTaskDelay(unsigned);\n"
        harness = r'''
#include "drive/operations.h"
#include <cassert>
#include <cstdint>
static uint16_t status=0x0250;
static uint8_t errors, mode=1;
static uint32_t target=12345, demand;
static unsigned writes, fail_write, cancel_write;
static bool cancelled, mode_stuck, bad_target;
static int64_t now;
int64_t esp_timer_get_time() { return now; }
bool drive_operation_cancelled() { return cancelled; }
bool drive_operation_wait_ms(unsigned ms) { now+=ms*1000; return !cancelled; }
int canopen_drive_sdo_read_u8(uint16_t index,uint8_t sub,uint8_t *out) {
    assert(sub==0 && (index==0x1001 || index==0x6061));
    *out=index==0x1001 ? errors : mode; return 0;
}
int canopen_drive_sdo_read_u16(uint16_t index,uint8_t sub,uint16_t *out) {
    assert(index==0x6041 && sub==0); *out=status; return 0;
}
int canopen_drive_sdo_read_u32(uint16_t index,uint8_t sub,uint32_t *out) {
    assert(sub==0 && index==0x60ff);
    *out=bad_target ? 5 : target; return 0;
}
int canopen_drive_sdo_write_u32(uint16_t index,uint8_t sub,uint32_t value) {
    assert(index==0x60ff && sub==0 && value==0 && writes==0);
    target=value; ++writes;
    if(writes==cancel_write) cancelled=true;
    return writes==fail_write ? -1 : 0;
}
int canopen_drive_sdo_write_u8(uint16_t index,uint8_t sub,uint8_t value) {
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
    reset(); assert(drive_prepare_velocity_zero()==0 && writes==2 && mode==3 && target==0);
    reset(); status=0x0237; assert(drive_prepare_velocity_zero()==1 && writes==0);
    reset(); status=0x1218; assert(drive_prepare_velocity_zero()==1 && writes==0);
    reset(); errors=1; assert(drive_prepare_velocity_zero()==1 && writes==0);
    reset(); cancelled=true; assert(drive_prepare_velocity_zero()==1 && writes==0);
    reset(); cancel_write=1; assert(drive_prepare_velocity_zero()==1 && writes==1 && mode==1);
    reset(); fail_write=1; assert(drive_prepare_velocity_zero()==-1 && writes==1);
    reset(); fail_write=2; assert(drive_prepare_velocity_zero()==-1 && writes==2);
    reset(); mode_stuck=true; assert(drive_prepare_velocity_zero()==3 && writes==2 && now>=500000);
    reset(); demand=1; assert(drive_prepare_velocity_zero()==0 && writes==2);
    reset(); bad_target=true; assert(drive_prepare_velocity_zero()==1 && writes==2);
}
'''
        harness += r'''
#include "canopen/feedback.h"
const char *esp_err_to_name(int) {return "test";}
bool drive_position_allowed(float,float) {assert(false);return false;}
float canopen_drive_get_actual_position_feedback() {assert(false);return 0;}
DriveFeedback drive_feedback_snapshot() {assert(false);return {};}

void vTaskDelay(unsigned ms) { now+=ms*1000; }
'''
        declarations = (PROJECT/'src/canopen/drive.h').read_text()
        custom = set(re.findall(r'int (canopen_drive_\w+)\(', harness))
        for function, args in re.findall(r'esp_err_t (canopen_drive_\w+)\((.*?)\);', declarations, re.S):
            if function not in custom:
                harness += f'\nint {function}({args}) {{assert(false);return ESP_FAIL;}}\n'
        with tempfile.TemporaryDirectory(prefix="ecvt-velocity-zero-") as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / "zero.cpp", tmp / "zero"
            source.write_text('#include "drive/operation_context.h"\n#include "canopen/drive.h"\n' + harness)
            subprocess.run([
                "c++", "-std=c++17", "-I", str(tmp), "-I", str(PROJECT / "src"),
                str(PROJECT / "src/drive/operations.cpp"), str(source), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, capture_output=True)
