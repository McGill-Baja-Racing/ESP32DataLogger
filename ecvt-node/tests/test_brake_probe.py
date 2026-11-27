"""Exercise the real brake state sequence and zero-velocity preparation against a simulated drive."""
from pathlib import Path
import shutil
import re
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class BrakeProbeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
    def test_state_sequence_window_guards_and_disable_cleanup(self):
        headers = {
            "esp_err.h": "#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_TIMEOUT=3;\nconst char *esp_err_to_name(int);\n",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
            "freertos/FreeRTOS.h": "#pragma once\n#define pdMS_TO_TICKS(x) (x)\n",
            "freertos/task.h": "#pragma once\nvoid vTaskDelay(unsigned);\n",
            "esp_twai.h": "#pragma once\nusing twai_node_handle_t=void*;\n",
            "freertos/queue.h": "#pragma once\nusing QueueHandle_t=void*;\n",
        }
        harness = r'''
#include "drive/operations.h"
#include <cassert>
#include <cstdint>
#include <vector>
static uint16_t status;
static uint8_t mode, errors;
static uint32_t velocity, target, demand;
static int64_t now, enabled_at, disabled_at;
static bool cancelled, fail_disable, stuck, fault_window, moving_window, bad_mode_window, bad_target_window, read_failure;
static unsigned fail_word, cancel_word, writes;
static bool fail_pre_enable_zero, reject_pre_enable_zero, cancel_pre_enable_zero;
static uint16_t last_write_index;
static std::vector<uint16_t> words;
int64_t esp_timer_get_time() { return now; }
void vTaskDelay(unsigned ms) { now+=ms*1000; }
bool drive_operation_cancelled() { return cancelled; }
bool drive_operation_wait_ms(unsigned ms) {
    now+=ms*1000;
    if (status==0x0237 && now>=50000) {
        if(fault_window) { status=0x1218; errors=1; }
        if(moving_window) velocity=static_cast<uint32_t>(-113);
        if(bad_mode_window) mode=1;
        if(bad_target_window) target=5;
    }
    return !cancelled;
}
int canopen_drive_sdo_read_u8(uint16_t index,uint8_t sub,uint8_t *out) {
    assert(sub==0 && (index==0x6061 || index==0x1001));
    *out=index==0x6061 ? mode : errors; return 0;
}
int canopen_drive_sdo_read_u16(uint16_t index,uint8_t sub,uint16_t *out) {
    assert(index==0x6041 && sub==0);
    if(read_failure && !words.empty() && words.back()!=0) return -1;
    *out=status; return 0;
}
int canopen_drive_sdo_read_u32(uint16_t index,uint8_t sub,uint32_t *out) {
    assert(sub==0 && index==0x60ff);
    *out=target; return 0;
}
int canopen_drive_sdo_write_u32(uint16_t index,uint8_t sub,uint32_t value) {
    assert(index==0x60ff && sub==0 && value==0);
    assert(writes==0 || (writes==2 && words==std::vector<uint16_t>({6,7})));
    ++writes; last_write_index=index;
    target=(writes==3 && reject_pre_enable_zero) ? 5 : 0;
    if(writes==3 && cancel_pre_enable_zero) cancelled=true;
    return writes==3 && fail_pre_enable_zero ? -1 : 0;
}
int canopen_drive_sdo_write_u8(uint16_t index,uint8_t sub,uint8_t value) {
    assert(index==0x6060 && sub==0 && value==3 && writes++==1 && target==0);
    last_write_index=index;
    mode=3; return 0;
}
int canopen_drive_sdo_write_u16(uint16_t index,uint8_t sub,uint16_t value) {
    assert(index==0x6040 && sub==0);
    assert(value==0 || value==6 || value==7 || value==15);
    if(value==15) assert(last_write_index==0x60ff && writes==3);
    last_write_index=index;
    words.push_back(value);
    if(value==0 && fail_disable) return -1;
    if(value==0) { status=0x0250; velocity=213; disabled_at=now; }
    else {
        assert(mode==3 && target==0 && errors==0);
        if(!stuck) status=value==6 ? 0x0231 : value==7 ? 0x0233 : 0x0237;
        if(value==15) enabled_at=now;
        if(value==cancel_word) cancelled=true;
        if(value==fail_word) return -1; // Timeout after accepted command.
    }
    return 0;
}
void reset() {
    assert(!drive_disable_pending());
    status=0x0250; mode=1; errors=0; velocity=0; target=12345; demand=0;
    now=enabled_at=disabled_at=0; writes=fail_word=cancel_word=0; words.clear();
    last_write_index=0; fail_pre_enable_zero=reject_pre_enable_zero=cancel_pre_enable_zero=false;
    cancelled=fail_disable=stuck=fault_window=moving_window=bad_mode_window=bad_target_window=read_failure=false;
}
int main() {
    reset(); assert(drive_test_brake()==0);
    assert((words==std::vector<uint16_t>{6,7,15,0}));
    assert(writes==3);
    assert(disabled_at-enabled_at==10000000 && status==0x0250 && mode==3 && target==0);
    for(uint16_t bad : {uint16_t(0x0237),uint16_t(0x1218),uint16_t(0x0231)}) {
        reset(); status=bad; assert(drive_test_brake()!=0 && words.empty() && writes==0);
    }
    reset(); errors=1; assert(drive_test_brake()!=0 && words.empty() && writes==0);
    reset(); demand=213; assert(drive_test_brake()==0 && disabled_at-enabled_at==10000000);
    reset(); velocity=213; assert(drive_test_brake()==0 && disabled_at-enabled_at==10000000);
    reset(); cancelled=true; assert(drive_test_brake()!=0 && words.empty() && writes==0);
    reset(); fail_pre_enable_zero=true; assert(drive_test_brake()!=0);
    assert((words==std::vector<uint16_t>{6,7,0}) && !drive_disable_pending());
    reset(); reject_pre_enable_zero=true; assert(drive_test_brake()!=0);
    assert((words==std::vector<uint16_t>{6,7,0}) && !drive_disable_pending());
    reset(); cancel_pre_enable_zero=true; assert(drive_test_brake()!=0);
    assert((words==std::vector<uint16_t>{6,7,0}) && !drive_disable_pending());
    for(unsigned word : {6u,7u,15u}) {
        reset(); fail_word=word; assert(drive_test_brake()!=0 && words.back()==0 && !drive_disable_pending());
        reset(); cancel_word=word; assert(drive_test_brake()!=0 && words.back()==0 && !drive_disable_pending());
    }
    reset(); stuck=true; assert(drive_test_brake()==3 && words.back()==0 && now>=1000000);
    reset(); fault_window=true; assert(drive_test_brake()!=0 && words.back()==0 && disabled_at<10000000);
    reset(); moving_window=true; assert(drive_test_brake()==0 && words.back()==0);
    assert(disabled_at-enabled_at==10000000 && velocity==213 && !drive_disable_pending());
    reset(); bad_mode_window=true; assert(drive_test_brake()!=0 && words.back()==0);
    reset(); bad_target_window=true; assert(drive_test_brake()!=0 && words.back()==0);
    reset(); read_failure=true; assert(drive_test_brake()!=0 && words.back()==0);
    reset(); fail_disable=true; assert(drive_test_brake()!=0 && drive_disable_pending());
    const auto before=words.size(); assert(drive_test_brake()!=0 && words.size()==before);
    cancelled=true; fail_disable=false; assert(drive_disable()==0 && !drive_disable_pending());
    assert(words.back()==0 && status==0x0250);
}
'''
        harness += r'''
#include "canopen/feedback.h"
const char *esp_err_to_name(int) {return "test";}
bool drive_position_allowed(float,float) {assert(false);return false;}
float canopen_drive_get_actual_position_feedback() {assert(false);return 0;}
DriveFeedback drive_feedback_snapshot() {assert(false);return {};}
'''
        declarations = (PROJECT/'src/canopen/drive.h').read_text()
        custom = set(re.findall(r'int (canopen_drive_\w+)\(', harness))
        for function, args in re.findall(r'esp_err_t (canopen_drive_\w+)\((.*?)\);', declarations, re.S):
            if function not in custom:
                harness += f'\nint {function}({args}) {{assert(false);return ESP_FAIL;}}\n'
        with tempfile.TemporaryDirectory(prefix="ecvt-brake-probe-") as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / "probe_test.cpp", tmp / "probe_test"
            source.write_text('#include "drive/operation_context.h"\n#include "canopen/drive.h"\n#include <initializer_list>\n' + harness)
            subprocess.run([
                "c++", "-std=c++17", "-I", str(tmp), "-I", str(PROJECT / "src"),
                str(PROJECT / "src/drive/operations.cpp"), str(source), "-o", str(binary),
            ], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("Drive disable confirmed", result.stdout)
