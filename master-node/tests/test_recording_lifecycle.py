"""Exercise production START/STOP ordering with CAN enabled and disabled."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RecordingLifecycleTests(unittest.TestCase):
    def test_start_stop_boundaries(self):
        source = (ROOT / 'src/app/app_control.c').read_text()
        lifecycle = source[source.index('app_control_result_t app_control_start_logging('):
                           source.index('app_control_result_t app_control_start_live_data(')]
        harness = r'''
#include <assert.h>
#include <string.h>
#include "app/app_control.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int lifecycle_mutex=1;
static bool locked, log_pending, can_pending;
static unsigned step, resets;
static logger_state_t state=LOGGER_IDLE;
static data_logger_start_result_t prepare_result=DATA_LOGGER_START_OK;
static int xSemaphoreTake(int mutex,int wait) { (void)wait;assert(mutex && !locked);locked=true;return pdTRUE; }
static void xSemaphoreGive(int mutex) { assert(mutex && locked);locked=false; }
logger_state_t data_logger_state(void) { return state; }
const char *data_logger_state_name(void) { return "test"; }
data_logger_start_result_t data_logger_prepare(const char *filename) {
    (void)filename;assert(locked && state==LOGGER_IDLE && step==0);
    if(prepare_result==DATA_LOGGER_START_OK)step=1;
    return prepare_result;
}
void data_logger_clear_queue(void) { assert(locked && step==1 && state==LOGGER_IDLE);log_pending=false;step=2;resets++; }
void can_master_clear_receive_queue(void) { assert(locked && step==2 && state==LOGGER_IDLE);can_pending=false;step=3;resets++; }
void data_logger_start(void) {
    assert(locked && !log_pending && step==(MASTER_CAN_ENABLED?3:2));
    if(MASTER_CAN_ENABLED)assert(!can_pending);
    state=LOGGER_RUNNING;step=4;
}
void node_registry_set_monitoring(bool enabled) { assert(locked && (enabled?state==LOGGER_RUNNING:state==LOGGER_STOPPING)); }
void time_beacon_set_recording(bool enabled) { assert(locked && (enabled?state==LOGGER_RUNNING:state==LOGGER_STOPPING)); }
esp_err_t can_master_start_nodes(void) { assert(locked && step==4);return ESP_OK; }
esp_err_t can_master_stop_nodes(void) { assert(locked && state==LOGGER_STOPPING);return ESP_OK; }
void live_data_force_stop(void) { assert(locked && state==LOGGER_RUNNING); }
bool data_logger_stop(void) { assert(locked && state==LOGGER_RUNNING);state=LOGGER_STOPPING;return true; }
''' + lifecycle + r'''
int main(void) {
    log_pending=can_pending=true;
    prepare_result=DATA_LOGGER_START_OPEN_FAILED;
    assert(app_control_start_logging("test.bin")==APP_CONTROL_FAILED);
    assert(!locked && resets==0 && log_pending && can_pending && state==LOGGER_IDLE);
    prepare_result=DATA_LOGGER_START_OK;
    assert(app_control_start_logging("test.bin")==APP_CONTROL_OK);
    assert(!locked && state==LOGGER_RUNNING && resets==(MASTER_CAN_ENABLED?2:1));
    assert(app_control_start_logging(NULL)==APP_CONTROL_CONFLICT);
    log_pending=true;
    assert(app_control_stop_logging()==APP_CONTROL_OK);
    assert(!locked && state==LOGGER_STOPPING && log_pending); /* drain, do not clear */
    assert(app_control_start_logging(NULL)==APP_CONTROL_CONFLICT);
    assert(app_control_stop_logging()==APP_CONTROL_CONFLICT);
    state=LOGGER_IDLE;step=0;can_pending=true; /* writer finished draining */
    assert(app_control_start_logging(NULL)==APP_CONTROL_OK);
    assert(!locked && !log_pending && state==LOGGER_RUNNING);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'esp_err.h').write_text('typedef int esp_err_t;\n')
            (tmp / 'test.c').write_text(harness)
            for enabled in (0, 1):
                with self.subTest(can_enabled=enabled):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    f'-DMASTER_CAN_ENABLED={enabled}', '-I'+str(tmp),
                                    '-I'+str(ROOT/'src'), '-I'+str(ROOT.parent),
                                    str(tmp/'test.c'), '-o', str(tmp/'test')], check=True)
                    subprocess.run([str(tmp/'test')], check=True)
