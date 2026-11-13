"""Check the health counters, sensor states, event sequences and panel lines."""
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

MASTER = Path(__file__).resolve().parents[1]
NODE = MASTER.parent / 'sensor-node'


def cut(path, start, end):
    source = path.read_text()
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


# The esp_err_t codes the firmware uses, copied from ESP-IDF's esp_err.h.
ESP_ERR_STUB = r'''
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
'''

# Just enough ESP-IDF and FreeRTOS to build protocol/health_counters.c on the host.
IDF_STUBS = {
    'esp_err.h': ESP_ERR_STUB,
    'esp_attr.h': '#pragma once\n#define RTC_NOINIT_ATTR\n',
    'esp_system.h': '#pragma once\n'
                    'typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW,\n'
                    '               ESP_RST_PANIC } esp_reset_reason_t;\n'
                    'static esp_reset_reason_t test_reset_reason = ESP_RST_POWERON;\n'
                    'static inline esp_reset_reason_t esp_reset_reason(void) { return test_reset_reason; }\n',
    'esp_timer.h': '#pragma once\n#include <stdint.h>\n'
                   'static int64_t test_time_us = 5000000;\n'
                   'static inline int64_t esp_timer_get_time(void) { return test_time_us; }\n',
    'freertos/FreeRTOS.h': '#pragma once\ntypedef int portMUX_TYPE;\n'
                           '#define portMUX_INITIALIZER_UNLOCKED 0\n'
                           '#define portENTER_CRITICAL(m) ((void)(m))\n'
                           '#define portEXIT_CRITICAL(m) ((void)(m))\n'
                           '#define portENTER_CRITICAL_SAFE(m) ((void)(m))\n'
                           '#define portEXIT_CRITICAL_SAFE(m) ((void)(m))\n',
}

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NODE_ID 3
#include "protocol/app_protocol.h"
#include "protocol/health_counters.c"
''' + cut(NODE / 'src/health/node_health.c', 'static sensor_state_t sensor_state(',
          'static bool send_event(') + r'''
static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

int main(void)
{
    uint32_t faults = 0;
    CHECK(health_for(faults, SENSOR_STATE_OK, false) == NODE_HEALTH_OK);
    faults |= HEALTH_BIT(HEALTH_SAMPLES_DROPPED) | HEALTH_BIT(HEALTH_CAN_NO_ACK);
    CHECK(health_for(faults, SENSOR_STATE_OK, false) == NODE_HEALTH_WARNING);
    faults |= HEALTH_BIT(HEALTH_CAN_PASSIVE);
    CHECK(health_for(faults, SENSOR_STATE_OK, false) == NODE_HEALTH_ERROR);
    CHECK(health_for(faults, SENSOR_STATE_OK, true) == NODE_HEALTH_CRITICAL);
    CHECK(health_for(0, SENSOR_STATE_SOME_FAILED, false) == NODE_HEALTH_WARNING);
    CHECK(health_for(0, SENSOR_STATE_NO_READINGS, false) == NODE_HEALTH_ERROR);

    CHECK(sensor_state(true, 100, 0) == SENSOR_STATE_INIT_FAILED);
    CHECK(sensor_state(false, 0, 0) == SENSOR_STATE_OK);
    CHECK(sensor_state(false, 10, 90) == SENSOR_STATE_SOME_FAILED);
    CHECK(sensor_state(false, 9, 91) == SENSOR_STATE_NO_READINGS);

    node_status_frame_t status = {.sensors = 3 | 1 << 2};
    CHECK(node_status_sensor_state(&status, 0) == SENSOR_STATE_INIT_FAILED);
    CHECK(node_status_sensor_state(&status, 1) == SENSOR_STATE_SOME_FAILED);

    health_event_frame_t event = node_health_pack_event(HEALTH_SENSOR_READ, 0x0B1, 69, 40000, ESP_ERR_TIMEOUT, 123456);
    CHECK(event.code == HEALTH_SENSOR_READ && event.subject == 0xB1 && event.sequence == 5);
    CHECK(event.count == HEALTH_EVENT_COUNT_MAX);
    CHECK(event.error == HEALTH_ERROR_TIMEOUT && event.timestamp_ms == 123456);

    if (!failures) printf("OK\n");
    return failures != 0;
}
'''


COUNTER_HARNESS = r'''
#include <stdio.h>
#define HEALTH_SHARED_COUNTERS 2
#include "protocol/health_counters.c"

static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

int main(void)
{
    uint8_t resets = 9, reason = 9;
    node_health_count_boot(&resets, &reason);
    CHECK(resets == 0 && reason == ESP_RST_POWERON);
    test_reset_reason = ESP_RST_PANIC;
    node_health_count_boot(&resets, &reason);
    CHECK(resets == 1 && reason == ESP_RST_PANIC);

    /* Two shared counters: the fourth subject joins the code's own counter. */
    node_health_count(HEALTH_CAN_TX, 2, 0xB1, ESP_FAIL);
    node_health_count(HEALTH_CAN_TX, 1, 0xB2, ESP_OK);
    node_health_count(HEALTH_CAN_TX, 1, 0xB3, ESP_OK);
    node_health_count(HEALTH_CAN_TX, 4, 0xB4, ESP_ERR_TIMEOUT);
    node_health_count(HEALTH_CAN_TX, 0, 0xB5, ESP_OK);
    node_health_count(HEALTH_SAMPLES_MISSED, 1, CAN_ID_GPS_SPEED, ESP_OK);
    health_state_t now;
    node_health_copy_counts(&now);
    const health_counter_t *own = &now.counters[HEALTH_CAN_TX];
    const health_counter_t *shared = &now.counters[HEALTH_CODE_COUNT];
    CHECK(own->count == 6 && own->subject == 0xB4 && own->error == ESP_ERR_TIMEOUT);
    CHECK(own->last_ms == 5000);
    CHECK(shared[0].code == HEALTH_CAN_TX && shared[0].subject == 0xB2 && shared[0].count == 1);
    CHECK(shared[1].code == HEALTH_CAN_TX && shared[1].subject == 0xB3 && shared[1].count == 1);
    CHECK(now.counters[HEALTH_SAMPLES_MISSED].subject == HEALTH_SUBJECT_GPS_BASE);
    CHECK(now.counters[HEALTH_BOOT].count == 2 && now.counters[HEALTH_BOOT].subject == ESP_RST_PANIC);
    reset_count_magic = 0;  /* a reset that lost the kept count is still a reset */
    node_health_count_boot(&resets, &reason);
    CHECK(resets == 1);

    health_state_t previous = now;
    node_health_set(HEALTH_CAN_PASSIVE, true);
    node_health_set(HEALTH_CAN_PASSIVE, true);
    node_health_copy_counts(&now);
    CHECK(now.counters[HEALTH_CAN_PASSIVE].count == 1);
    CHECK(node_health_get_faults(&now, &previous) == HEALTH_BIT(HEALTH_CAN_PASSIVE));
    node_health_set(HEALTH_CAN_PASSIVE, false);
    previous = now;
    node_health_copy_counts(&now);
    CHECK(node_health_get_faults(&now, &previous) == 0);

    if (!failures) printf("OK\n");
    return failures != 0;
}
'''


SEQUENCE_HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "protocol/app_protocol.h"
''' + cut(MASTER / 'src/health/node_health.c', '/* Used only from CAN dispatch. */',
    'bool node_health_handle_frame(') + r'''
/* 99 marks a frame dropped as received twice. */
static uint32_t missed(unsigned node, uint8_t code, uint8_t sequence)
{
    health_event_frame_t event = {.code = code, .sequence = sequence};
    uint32_t count;
    return check_sequence(node, &event, &count) ? count : 99;
}

int main(void)
{
    int failures = 0;
    const uint32_t expected[][4] = {
        {3, HEALTH_SAMPLES_MISSED, 17, 0},  /* first event seen syncs */
        {3, HEALTH_SAMPLES_MISSED, 18, 0},
        {3, HEALTH_CAN_TX, 21, 2},
        {3, HEALTH_CAN_TX, 21, 99},         /* received twice */
        {3, HEALTH_SENSOR_READ, 63, 41},
        {3, HEALTH_SENSOR_READ, 1, 1},      /* wraps */
        {5, HEALTH_SAMPLES_MISSED, 40, 0},  /* per node */
        {3, HEALTH_BOOT, 0, 0},             /* reboot */
        {3, HEALTH_SENSOR_READ, 3, 2},
    };
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        uint32_t got = missed(expected[i][0], (uint8_t)expected[i][1], (uint8_t)expected[i][2]);
        if (got != expected[i][3]) {
            printf("FAIL step %u: missed %u\n", i, (unsigned)got);
            failures++;
        }
    }
    if (!failures) printf("OK\n");
    return failures != 0;
}
'''


PANEL_HARNESS = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "protocol/app_protocol.h"
''' + cut(MASTER / 'src/health/node_health.h', '#define NODE_HEALTH_TEXT_MAX', '\n') + '\n' \
    + cut(MASTER / 'src/health/node_health.h', 'typedef struct {', 'bool node_health_get(') \
    + cut(MASTER / 'src/health/node_health.c', 'static const char *const code_names',
          '#define SUMMARY_FRESH_MS') \
    + cut(MASTER / 'src/health/node_health.c', 'static const char *const fault_labels',
          'void node_health_list_problems(') + r'''
static int failures;

static void expect(const node_health_view_t *view, const char *line)
{
    char text[NODE_HEALTH_TEXT_MAX] = "";
    if (!describe_node(view, true, text, sizeof(text)) || strcmp(text, line) != 0) {
        printf("FAIL: got \"%s\", want \"%s\"\n", text, line);
        failures++;
    }
}

int main(void)
{
    node_health_view_t mpu = {.name = "mpu_node_3", .fresh = true, .level = NODE_HEALTH_CRITICAL,
                              .sensor_count = 6};
    for (int i = 0; i < 6; i++) {
        mpu.sensors[i].name = "mpu";
        mpu.sensors[i].state = SENSOR_STATE_NO_READINGS;
    }
    expect(&mpu, "critical: mpu_node_3: all sensors: no readings");

    node_health_view_t engine = {.name = "engine_node_5", .fresh = true, .level = NODE_HEALTH_CRITICAL,
                                 .sensors = {{"engine_rpm", SENSOR_STATE_INIT_FAILED}},
                                 .sensor_count = 1, .resets = 2, .last_reset = "brownout"};
    expect(&engine, "critical: engine_node_5: engine_rpm: failed to start; restarted 2 times, last: brownout");

    node_health_view_t master = {.name = "master", .fresh = true, .level = NODE_HEALTH_WARNING,
                                 .faults = HEALTH_BIT(HEALTH_GPS_NO_FIX)};
    expect(&master, "warning: master: GPS has no fix");

    node_health_view_t silent = {.name = "brake_node_1"};
    expect(&silent, "critical: brake_node_1: not responding");

    node_health_view_t worst = {.name = "mpu_node_3", .fresh = true, .level = NODE_HEALTH_CRITICAL,
                                .sensor_count = 6, .resets = 255, .last_reset = "interrupt_watchdog"};
    const char *const axes[] = {"mpu_accel_x", "mpu_accel_y", "mpu_accel_z",
                                "mpu_gyro_x", "mpu_gyro_y", "mpu_gyro_z"};
    for (int i = 0; i < 6; i++) {
        worst.sensors[i].name = axes[i];
        worst.sensors[i].state = i % 2 ? SENSOR_STATE_SOME_FAILED : SENSOR_STATE_NO_READINGS;
    }
    for (int fault = HEALTH_SAMPLES_MISSED; fault <= HEALTH_CAN_BUS_OFF; fault++) {
        worst.faults |= HEALTH_BIT(fault);
    }
    expect(&worst, "critical: mpu_node_3: mpu_accel_x: no readings; mpu_accel_y: some readings failed; "
                   "mpu_accel_z: no readings; mpu_gyro_x: some readings failed; mpu_gyro_y: no readings; "
                   "mpu_gyro_z: some readings failed; sampling fell behind; samples were lost before sending; "
                   "samples have bad times; no clock sync from the master; CAN sends failed; "
                   "CAN receive overflowed; no other node answers on CAN; CAN wiring errors; "
                   "too many CAN errors; off the CAN bus; restarted 255 times, last: interrupt_watchdog");

    if (!failures) printf("OK\n");
    return failures != 0;
}
'''


SLOT_HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "protocol/app_protocol.h"

int main(void)
{
    for (unsigned node = 1; node < CAN_NODE_SLOT_COUNT; node++) {
        printf("%u:", node);
        for (unsigned slot = 0; slot < NODE_SENSOR_SLOTS; slot++) {
            uint32_t id = protocol_slot_sensor_id((uint8_t)node, slot);
            if (id) printf(" %u%s", (unsigned)id, protocol_sensor_slot((uint8_t)node, id) == slot ? "" : " bad");
        }
        printf("\n");
    }
    return 0;
}
'''

ROUTING_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "can/can_master.h"
#include "protocol/app_protocol.h"
''' + cut(MASTER / 'src/health/node_health.c', 'static uint32_t frame_base(',
          'static const char *const code_names') + r'''
int main(void)
{
    uint32_t offline_bits = 0;
    assert(protocol_offline_node_bit(0) == 0);
    for (uint8_t node = 1; node < CAN_NODE_SLOT_COUNT; node++) {
        uint8_t bit = protocol_offline_node_bit(node);
        assert(bit != 0 && !(offline_bits & bit));
        offline_bits |= bit;
        const uint32_t bases[] = {CAN_ID_NODE_STATUS_BASE, CAN_ID_NODE_EVENT_BASE,
                                  CAN_ID_NODE_DIAGNOSTICS_BASE};
        for (unsigned i = 0; i < 3; i++) {
            can_message_t frame = {.id = bases[i] + node};
            assert(node_health_is_frame(&frame));
            assert(node_health_node_id(&frame) == node);
        }
        can_message_t state = {.id = CAN_ID_NODE_STATE_BASE + node};
        assert(!node_health_is_frame(&state));
    }
    assert(protocol_health_subject_id(protocol_health_subject(CAN_ID_AIM_WIRE)) == CAN_ID_AIM_WIRE);
    assert(protocol_health_subject_id(protocol_health_subject(CAN_ID_AIM_RPM)) == CAN_ID_AIM_RPM);
    assert(protocol_health_subject(CAN_ID_AIM_WIRE) <= 255);
    assert(protocol_health_subject(CAN_ID_AIM_RPM) <= 255);
    assert(offline_bits == 255 && protocol_offline_node_bit(8) == 1);
    can_message_t rear = {.id = CAN_ID_REAR_LIN_2};
    assert(!node_health_is_frame(&rear) && protocol_sensor_node_id(rear.id) == 8);
    assert(protocol_sensor_slot(8, rear.id) == 1);
    printf("OK\n");
    return 0;
}
'''

HEALTH_RESPONSE_HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "health/node_health.h"
#include "live/live_data.h"
#include "aim/aim_adapter.h"
#include "node_state/node_registry.h"
#define ESP_OK 0
#define ESP_FAIL -1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static uint32_t now_ms(void) { return 10000; }
''' + cut(MASTER / 'src/live/live_data.c', 'static const live_signal_metadata_t signals[',
          'static live_sample_t latest[') \
    + cut(MASTER / 'src/live/live_data.c', 'const live_signal_metadata_t *live_data_signals(',
          'void live_data_record(') \
    + cut(MASTER / 'src/health/node_health.c', 'static const char *const code_names',
          '/* Used only from CAN dispatch. */') + r'''
typedef int httpd_req_t;
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
static char response[16384];
static bool allocation_fails, send_fails;
static unsigned allocations, response_status;
void *test_malloc(size_t size) {
    if (allocation_fails) return NULL;
    void *memory = malloc(size);
    if (memory) allocations++;
    return memory;
}
void test_free(void *memory) {
    assert(memory && allocations);
    allocations--;
    free(memory);
}
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type) {
    (void)request; assert(!strcmp(type, "text/plain")); return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value) {
    (void)request;
    assert(!strcmp(name, "Cache-Control") && !strcmp(value, "no-store"));
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *text) {
    (void)request;
    assert(strlen(text) < sizeof(response));
    strcpy(response, text);
    response_status = 200;
    return send_fails ? ESP_FAIL : ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message) {
    (void)request; (void)message;
    response_status = status;
    return ESP_OK;
}
#define malloc test_malloc
#define free test_free
''' + cut(MASTER / 'src/web/web_server.c', 'static esp_err_t health_handler(',
          'static esp_err_t command_handler(') + r'''
#undef malloc
#undef free

int main(void) {
    httpd_req_t request = 0;
    unsigned nodes_reported = 0;
    for (unsigned node = 0; node < CAN_NODE_SLOT_COUNT; node++) {
        if (node && !(CAN_NODE_ID_MASK & (1U << node))) continue;
        node_summary_t *entry = &summary[node];
        entry->status_seen = true;
        entry->status.timestamp_ms = now_ms();
        entry->status.health = NODE_HEALTH_CRITICAL;
        entry->status.faults = (1U << HEALTH_FAULT_COUNT) - 1;
        entry->diagnostics.resets = 255;
        entry->diagnostics.reset_reason = 5; /* interrupt_watchdog */
        for (unsigned slot = 0; slot < NODE_SENSOR_SLOTS; slot++) {
            entry->status.sensors |= (slot % 2 ? SENSOR_STATE_SOME_FAILED :
                                      SENSOR_STATE_NO_READINGS) << (2 * slot);
        }
        nodes_reported++;
    }
    assert(health_handler(&request) == ESP_OK && response_status == 200);
    assert(strlen(response) > 2048 && allocations == 0);
    unsigned lines = 0;
    for (const char *p = response; *p; p++) if (*p == '\n') lines++;
    assert(lines == nodes_reported);
    for (unsigned node = 0; node < CAN_NODE_SLOT_COUNT; node++) {
        if (node && !(CAN_NODE_ID_MASK & (1U << node))) continue;
        char name[64];
        snprintf(name, sizeof(name), "%s:", node_names[node]);
        assert(strstr(response, name));
    }
    char complete[sizeof(response)];
    node_health_list_problems(complete, sizeof(complete));
    assert(!strcmp(response, complete));

    /* A failed network send must still release the response buffer. */
    send_fails = true;
    assert(health_handler(&request) == ESP_FAIL && allocations == 0);
    send_fails = false;
    allocation_fails = true;
    assert(health_handler(&request) == ESP_OK);
    assert(response_status == 500 && allocations == 0);
    allocation_fails = false;

    /* Healthy and missing-node responses remain well-formed. */
    for (unsigned node = 0; node < CAN_NODE_SLOT_COUNT; node++) {
        summary[node] = (node_summary_t){.status_seen = true};
        summary[node].status.timestamp_ms = now_ms();
    }
    assert(health_handler(&request) == ESP_OK && response[0] == '\0');
    summary[8].status_seen = false;
    assert(health_handler(&request) == ESP_OK);
    assert(!strcmp(response, "critical: rear_shock_node_8: not responding\n"));
    assert(allocations == 0);
    printf("OK\n");
    return 0;
}
'''


def compile_and_run(source, include, stubs=None):
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        (temp / 'test.c').write_text(source)
        for name, text in (stubs or {}).items():
            (temp / name).parent.mkdir(parents=True, exist_ok=True)
            (temp / name).write_text(text)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(include),
                        '-I' + str(MASTER.parent), '-I' + str(temp), str(temp / 'test.c'),
                        '-o', str(temp / 'test')],
                       check=True)
        return subprocess.run([str(temp / 'test')], capture_output=True, text=True)


class NodeHealthTest(unittest.TestCase):
    def check(self, source, include, stubs=None):
        result = compile_and_run(source, include, stubs)
        self.assertEqual(result.stdout, 'OK\n', result.stdout)
        self.assertEqual(result.returncode, 0)

    def test_node_states_and_event_packing(self):
        self.check(HARNESS, NODE / 'src', IDF_STUBS)

    def test_shared_counters(self):
        self.check(COUNTER_HARNESS, MASTER / 'src', IDF_STUBS)

    def test_master_counts_missed_event_sequences(self):
        self.check(SEQUENCE_HARNESS, MASTER / 'src')

    def test_master_panel_lines(self):
        self.check(PANEL_HARNESS, MASTER / 'src')

    def test_status_slots_match_the_decoder(self):
        result = compile_and_run(SLOT_HARNESS, MASTER / 'src')
        decoder = runpy.run_path(str(MASTER / 'tools/decode_log.py'))
        expected = ''.join(f"{node}:{''.join(f' {can_id}' for can_id in ids[:6])}\n"
                           for node, ids in sorted(decoder['SENSOR_IDS_BY_NODE'].items()))
        self.assertEqual(result.stdout, expected)

    def test_shock_health_routing_and_offline_mask(self):
        self.check(ROUTING_HARNESS, MASTER / 'src', {'esp_err.h': ESP_ERR_STUB})

    def test_health_response_keeps_all_nodes(self):
        self.check(HEALTH_RESPONSE_HARNESS, MASTER / 'src', {'esp_err.h': ESP_ERR_STUB})


if __name__ == '__main__':
    unittest.main()
