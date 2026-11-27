"""Exercise production START/STOP ordering and the locks it takes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def build_and_run(harness):
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        (tmp / 'esp_err.h').write_text('typedef int esp_err_t;\n')
        (tmp / 'test.c').write_text(harness)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(tmp),
                        '-I'+str(ROOT/'src'), '-I'+str(ROOT.parent),
                        str(tmp/'test.c'), '-o', str(tmp/'test')], check=True)
        subprocess.run([str(tmp/'test')], check=True)


# Two mutexes: Start/Stop hold LIFECYCLE throughout, and take PROCESSING (the
# one CAN dispatch takes per frame) only around the quick state change.
LOCK_STUBS = r'''
#include <assert.h>
#include <string.h>
#include "app/app_control.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY (-1)
#define LIFECYCLE_LOCK_TIMEOUT_MS 1000
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
enum { LIFECYCLE = 1, PROCESSING = 2 };
static int lifecycle_mutex __attribute__((unused)) = LIFECYCLE;
static int processing_mutex __attribute__((unused)) = PROCESSING;
static bool locked[3];
static int xSemaphoreTake(int mutex, int wait) { (void)wait; assert(mutex == LIFECYCLE || mutex == PROCESSING); assert(!locked[mutex]); locked[mutex] = true; return pdTRUE; }
static void xSemaphoreGive(int mutex) { assert(mutex == LIFECYCLE || mutex == PROCESSING); assert(locked[mutex]); locked[mutex] = false; }
'''


class RecordingLifecycleTests(unittest.TestCase):
    def test_start_stop_boundaries(self):
        source = (ROOT / 'src/app/app_control.c').read_text()
        lifecycle = source[source.index('app_control_result_t app_control_start_logging('):
                           source.index('app_control_result_t app_control_claim_firmware(')]
        harness = LOCK_STUBS + r'''
static bool log_pending;
static unsigned step, resets, health_starts;
static logger_state_t state=LOGGER_IDLE;
static data_logger_start_result_t prepare_result=DATA_LOGGER_START_OK;
static bool firmware_claimed;
bool firmware_update_on_trial(void) { return false; }
bool firmware_update_node_protocol_differs(void) { return false; }
logger_state_t data_logger_state(void) { return state; }
const char *data_logger_state_name(void) { return "test"; }
/* The GPS naming wait happens here: CAN dispatch must not be blocked by it. */
data_logger_start_result_t data_logger_prepare(const char *filename) {
    (void)filename;assert(locked[LIFECYCLE] && !locked[PROCESSING] && state==LOGGER_IDLE && step==0);
    if(prepare_result==DATA_LOGGER_START_OK)step=1;
    return prepare_result;
}
void data_logger_clear_queue(void) { assert(locked[PROCESSING] && step==1 && state==LOGGER_IDLE);log_pending=false;step=2;resets++; }
void node_health_recording_started(void) { assert(locked[PROCESSING] && step==2 && state==LOGGER_IDLE);health_starts++;step=3; }
void data_logger_start(void) { assert(locked[PROCESSING] && !log_pending && step==3);state=LOGGER_RUNNING;step=4; }
void time_beacon_set_recording(bool enabled) { assert(locked[PROCESSING] && (enabled?state==LOGGER_RUNNING:state==LOGGER_STOPPING)); }
esp_err_t can_master_start_nodes(void) { assert(locked[LIFECYCLE] && !locked[PROCESSING] && step==4);return ESP_OK; }
esp_err_t can_master_stop_nodes(void) { assert(locked[LIFECYCLE] && !locked[PROCESSING] && state==LOGGER_STOPPING);return ESP_OK; }
bool data_logger_stop(void) { assert(locked[PROCESSING] && state==LOGGER_RUNNING);state=LOGGER_STOPPING;return true; }
''' + lifecycle + r'''
int main(void) {
    log_pending=true;
    prepare_result=DATA_LOGGER_START_OPEN_FAILED;
    assert(app_control_start_logging("test.bin")==APP_CONTROL_FAILED);
    assert(!locked[LIFECYCLE] && !locked[PROCESSING] && resets==0 && health_starts==0);
    assert(log_pending && state==LOGGER_IDLE);
    prepare_result=DATA_LOGGER_START_OK;
    assert(app_control_start_logging("test.bin")==APP_CONTROL_OK);
    assert(!locked[LIFECYCLE] && !locked[PROCESSING] && state==LOGGER_RUNNING);
    assert(resets==1 && health_starts==1);
    assert(app_control_start_logging(NULL)==APP_CONTROL_CONFLICT);
    log_pending=true;
    assert(app_control_stop_logging()==APP_CONTROL_OK);
    assert(!locked[LIFECYCLE] && !locked[PROCESSING] && state==LOGGER_STOPPING && log_pending); /* drain, do not clear */
    assert(app_control_start_logging(NULL)==APP_CONTROL_CONFLICT);
    assert(app_control_stop_logging()==APP_CONTROL_CONFLICT);
    state=LOGGER_IDLE;step=0; /* writer finished draining */
    assert(app_control_start_logging(NULL)==APP_CONTROL_OK);
    assert(!log_pending && state==LOGGER_RUNNING && health_starts==2);
    return 0;
}
'''
        build_and_run(harness)

    def test_live_data_starts_while_idle(self):
        source = (ROOT / 'src/app/app_control.c').read_text()
        live = source[source.index('app_control_result_t app_control_start_live_data('):
                      source.index('void app_control_lock_can_processing(')]
        harness = LOCK_STUBS + r'''
#include "live/live_data.h"
static unsigned starts;
logger_state_t data_logger_state(void) { return LOGGER_IDLE; }
live_data_result_t live_data_start(uint32_t *token) { *token = 7; starts++; return LIVE_DATA_OK; }
''' + live + r'''
int main(void) {
    uint32_t token = 0;
    assert(app_control_start_live_data(&token) == APP_CONTROL_OK);
    assert(token == 7 && starts == 1 && !locked[LIFECYCLE]);
    return 0;
}
'''
        build_and_run(harness)
