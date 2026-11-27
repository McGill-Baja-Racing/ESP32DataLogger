"""Check when a sensor node starts sampling after boot.

A node that reboots during a recording must not send samples on its own clock:
the master logs them. It waits briefly for the master's beacon, and samples
anyway without a master.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NODE = ROOT.parent / 'sensor-node'

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "protocol/app_protocol.h"
#define SENSOR_SERIAL_TEST 0
#define ESP_OK 0
typedef int esp_err_t;
#define ESP_ERROR_CHECK(x) ((void)(x))
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) ((ms) / 10)   /* 100 Hz FreeRTOS tick */
typedef struct {
    void (*on_start)(void); void (*on_stop)(void); void (*on_time_beacon)(uint64_t);
    void (*on_bus_off)(void); void (*on_update_command)(const uint8_t *, uint8_t);
    void (*on_update_data)(const uint8_t *, uint8_t);
} can_node_callbacks_t;

static long now_ms, beacon_at_ms = -1, sampler_started_at = -1;
static bool synced_when_started;
static bool time_sync_is_current(uint32_t max_age_ms) { (void)max_age_ms; return beacon_at_ms >= 0 && now_ms >= beacon_at_ms; }
static void vTaskDelay(int ticks)
{
    now_ms += (ticks > 0 ? ticks : 1) * 10;
    if (now_ms > 3000 + 2000) {          /* app_main then waits forever to confirm the image */
        assert(sampler_started_at >= 0);
        exit(0);
    }
}
static void sampler_start(void) { sampler_started_at = now_ms; synced_when_started = time_sync_is_current(300); }
static esp_err_t firmware_update_init(void) { return ESP_OK; }
static esp_err_t sampler_init(void) { return ESP_OK; }
static esp_err_t can_node_init(const can_node_callbacks_t *c, uint8_t r) { (void)c; (void)r; return ESP_OK; }
static void can_node_report_state(node_state_t s, node_state_reason_t r) { (void)s; (void)r; }
static void firmware_update_report_identity(void) {}
static esp_err_t node_health_init(void) { return ESP_OK; }
static void node_health_recording_started(void) {}
static void firmware_update_confirm(void) { assert(sampler_started_at >= 0); exit(0); }
static int esp_reset_reason(void) { return 1; }
static void time_sync_update(uint64_t t) { (void)t; }
static void sampler_discard_pending(void) {}
static void firmware_update_handle_command(const uint8_t *p, uint8_t l) { (void)p; (void)l; }
static void firmware_update_handle_data(const uint8_t *p, uint8_t l) { (void)p; (void)l; }
''' + '%s' + r'''
int main(int argc, char **argv)
{
    (void)argc;
    beacon_at_ms = atol(argv[1]);        /* -1: no master */
    long latest_start_ms = atol(argv[2]);
    bool must_be_synced = argv[3][0] == '1';
    sampler_started_at = -1;
    /* Check right after sampler_start() by wrapping the call. */
    app_main();
    (void)latest_start_ms; (void)must_be_synced;
    return 0;
}
'''


def app_main_source():
    source = (NODE / 'src/main.c').read_text()
    start = source.index('#define SENSOR_STARTUP_DELAY_MS')
    return source[start:]


class NodeBootTests(unittest.TestCase):
    def run_boot(self, beacon_at_ms, latest_start_ms, must_be_synced):
        body = app_main_source().replace(
            'sampler_start();',
            'sampler_start(); assert(sampler_started_at <= %d); assert(!%d || synced_when_started);'
            % (latest_start_ms, 1 if must_be_synced else 0))
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / 'test.c').write_text(HARNESS % body)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function', '-Werror',
                            '-I' + str(ROOT.parent), str(tmp / 'test.c'), '-o', str(tmp / 'test')],
                           check=True)
            subprocess.run([str(tmp / 'test'), str(beacon_at_ms), str(latest_start_ms),
                            '1' if must_be_synced else '0'], check=True)

    def test_waits_for_the_beacon_before_sampling(self):
        # The beacon arrives 150 ms after CAN starts: the first sample is on master time.
        self.run_boot(beacon_at_ms=3150, latest_start_ms=3200, must_be_synced=True)

    def test_samples_without_a_master(self):
        # No beacon ever: sampling still starts, about 300 ms after CAN starts.
        self.run_boot(beacon_at_ms=-1, latest_start_ms=3400, must_be_synced=False)
