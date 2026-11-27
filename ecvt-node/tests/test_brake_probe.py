"""Exercise the real brake state sequence and zero-velocity preparation against a simulated drive."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class BrakeProbeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
    def test_state_sequence_window_guards_and_disable_cleanup(self):
        headers = {
            "esp_err.h": "#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=1, ESP_ERR_TIMEOUT=3;\n",
            "esp_timer.h": "#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n",
            "motor/runtime.h": "#pragma once\nbool motor_operation_cancelled();\nbool motor_operation_wait_ms(unsigned);\n",
            "freertos/FreeRTOS.h": "#pragma once\n#define pdMS_TO_TICKS(x) (x)\n",
            "freertos/task.h": "#pragma once\nvoid vTaskDelay(unsigned);\n",
            "servo/ECVT_motor.h": """#pragma once
#include <cstdint>
#include "esp_err.h"
esp_err_t ecvt_motor_sdo_read_u8(uint16_t,uint8_t,uint8_t*);
esp_err_t ecvt_motor_sdo_read_u16(uint16_t,uint8_t,uint16_t*);
esp_err_t ecvt_motor_sdo_read_u32(uint16_t,uint8_t,uint32_t*);
esp_err_t ecvt_motor_sdo_write_u8(uint16_t,uint8_t,uint8_t);
esp_err_t ecvt_motor_sdo_write_u16(uint16_t,uint8_t,uint16_t);
esp_err_t ecvt_motor_sdo_write_u32(uint16_t,uint8_t,uint32_t);
""",
        }
        harness = r'''
#include "experiments/brake_probe.h"
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
bool motor_operation_cancelled() { return cancelled; }
bool motor_operation_wait_ms(unsigned ms) {
    now+=ms*1000;
    if (status==0x0237 && now>=50000) {
        if(fault_window) { status=0x1218; errors=1; }
        if(moving_window) velocity=static_cast<uint32_t>(-113);
        if(bad_mode_window) mode=1;
        if(bad_target_window) target=5;
    }
    return !cancelled;
}
int ecvt_motor_sdo_read_u8(uint16_t index,uint8_t sub,uint8_t *out) {
    assert(sub==0 && (index==0x6061 || index==0x1001));
    *out=index==0x6061 ? mode : errors; return 0;
}
int ecvt_motor_sdo_read_u16(uint16_t index,uint8_t sub,uint16_t *out) {
    assert(index==0x6041 && sub==0);
    if(read_failure && !words.empty() && words.back()!=0) return -1;
    *out=status; return 0;
}
int ecvt_motor_sdo_read_u32(uint16_t index,uint8_t sub,uint32_t *out) {
    assert(sub==0 && index==0x60ff);
    *out=target; return 0;
}
int ecvt_motor_sdo_write_u32(uint16_t index,uint8_t sub,uint32_t value) {
    assert(index==0x60ff && sub==0 && value==0);
    assert(writes==0 || (writes==2 && words==std::vector<uint16_t>({6,7})));
    ++writes; last_write_index=index;
    target=(writes==3 && reject_pre_enable_zero) ? 5 : 0;
    if(writes==3 && cancel_pre_enable_zero) cancelled=true;
    return writes==3 && fail_pre_enable_zero ? -1 : 0;
}
int ecvt_motor_sdo_write_u8(uint16_t index,uint8_t sub,uint8_t value) {
    assert(index==0x6060 && sub==0 && value==3 && writes++==1 && target==0);
    last_write_index=index;
    mode=3; return 0;
}
int ecvt_motor_sdo_write_u16(uint16_t index,uint8_t sub,uint16_t value) {
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
    assert(!brake_probe_restore_pending());
    status=0x0250; mode=1; errors=0; velocity=0; target=12345; demand=0;
    now=enabled_at=disabled_at=0; writes=fail_word=cancel_word=0; words.clear();
    last_write_index=0; fail_pre_enable_zero=reject_pre_enable_zero=cancel_pre_enable_zero=false;
    cancelled=fail_disable=stuck=fault_window=moving_window=bad_mode_window=bad_target_window=read_failure=false;
}
int main() {
    reset(); assert(brake_probe_run()==0);
    assert((words==std::vector<uint16_t>{6,7,15,0}));
    assert(writes==3);
    assert(disabled_at-enabled_at==10000000 && status==0x0250 && mode==3 && target==0);
    for(uint16_t bad : {uint16_t(0x0237),uint16_t(0x1218),uint16_t(0x0231)}) {
        reset(); status=bad; assert(brake_probe_run()!=0 && words.empty() && writes==0);
    }
    reset(); errors=1; assert(brake_probe_run()!=0 && words.empty() && writes==0);
    reset(); demand=213; assert(brake_probe_run()==0 && disabled_at-enabled_at==10000000);
    reset(); velocity=213; assert(brake_probe_run()==0 && disabled_at-enabled_at==10000000);
    reset(); cancelled=true; assert(brake_probe_run()!=0 && words.empty() && writes==0);
    reset(); fail_pre_enable_zero=true; assert(brake_probe_run()!=0);
    assert((words==std::vector<uint16_t>{6,7,0}) && !brake_probe_restore_pending());
    reset(); reject_pre_enable_zero=true; assert(brake_probe_run()!=0);
    assert((words==std::vector<uint16_t>{6,7,0}) && !brake_probe_restore_pending());
    reset(); cancel_pre_enable_zero=true; assert(brake_probe_run()!=0);
    assert((words==std::vector<uint16_t>{6,7,0}) && !brake_probe_restore_pending());
    for(unsigned word : {6u,7u,15u}) {
        reset(); fail_word=word; assert(brake_probe_run()!=0 && words.back()==0 && !brake_probe_restore_pending());
        reset(); cancel_word=word; assert(brake_probe_run()!=0 && words.back()==0 && !brake_probe_restore_pending());
    }
    reset(); stuck=true; assert(brake_probe_run()==3 && words.back()==0 && now>=1000000);
    reset(); fault_window=true; assert(brake_probe_run()!=0 && words.back()==0 && disabled_at<10000000);
    reset(); moving_window=true; assert(brake_probe_run()==0 && words.back()==0);
    assert(disabled_at-enabled_at==10000000 && velocity==213 && !brake_probe_restore_pending());
    reset(); bad_mode_window=true; assert(brake_probe_run()!=0 && words.back()==0);
    reset(); bad_target_window=true; assert(brake_probe_run()!=0 && words.back()==0);
    reset(); read_failure=true; assert(brake_probe_run()!=0 && words.back()==0);
    reset(); fail_disable=true; assert(brake_probe_run()!=0 && brake_probe_restore_pending());
    const auto before=words.size(); assert(brake_probe_run()!=0 && words.size()==before);
    cancelled=true; fail_disable=false; assert(brake_probe_restore()==0 && !brake_probe_restore_pending());
    assert(words.back()==0 && status==0x0250);
}
'''
        with tempfile.TemporaryDirectory(prefix="ecvt-brake-probe-") as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / "probe_test.cpp", tmp / "probe_test"
            source.write_text("#include <initializer_list>\n" + harness)
            subprocess.run([
                "c++", "-std=c++17", "-I", str(tmp), "-I", str(PROJECT / "src"),
                str(PROJECT / "src/experiments/brake_probe.cpp"),
                str(PROJECT / "src/servo/velocity_zero.cpp"),
                str(PROJECT / "src/servo/drive_state.cpp"), str(source), "-o", str(binary),
            ], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("Drive disable confirmed", result.stdout)
