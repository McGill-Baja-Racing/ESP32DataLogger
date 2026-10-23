"""Check the node's health frames against the shared protocol and the sample record layout."""
from pathlib import Path
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
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_WIFI_BASE 0x3000
'''

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define NODE_ID 3
#include "protocol/app_protocol.h"
#include "esp_err.h"
''' + cut(NODE / 'src/health/node_health.c', 'static health_error_t health_error(',
          'static bool send_event(') + r'''
static int failures;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

/* The record the master logs: the frame bytes as one little-endian uint64. */
static uint64_t logged(const void *frame)
{
    const uint8_t *bytes = frame;
    uint64_t data = 0;
    for (int i = 0; i < 8; i++) data |= (uint64_t)bytes[i] << (8 * i);
    return data;
}

int main(void)
{
    uint16_t faults = 0;
    CHECK(health_for(faults, false) == NODE_HEALTH_OK);
    faults |= HEALTH_BIT(HEALTH_SAMPLES_DROPPED) | HEALTH_BIT(HEALTH_CAN_NO_ACK);
    CHECK(health_for(faults, false) == NODE_HEALTH_WARNING);
    faults |= HEALTH_BIT(HEALTH_CAN_PASSIVE);
    CHECK(health_for(faults, false) == NODE_HEALTH_ERROR);
    CHECK(health_for(faults, true) == NODE_HEALTH_CRITICAL);

    node_status_frame_t status = {.faults = 0x1234, .health = 2, .mode = 1, .timestamp_ms = 987654};
    uint64_t record = logged(&status);
    CHECK((uint32_t)(record >> 32) == 987654);
    CHECK((record & 0xFFFF) == 0x1234);

    health_event_frame_t event = event_frame(HEALTH_SENSOR_READ, 0x0B1, 300, ESP_ERR_TIMEOUT, 123456);
    CHECK(event.code == HEALTH_SENSOR_READ && event.subject == 0xB1 && event.count == 255);
    CHECK(event.error == HEALTH_ERROR_TIMEOUT && event.timestamp_ms == 123456);
    record = logged(&event);
    CHECK((uint32_t)(record >> 32) == 123456);
    CHECK((record & 0xFF) == HEALTH_SENSOR_READ);

    CHECK(health_error(ESP_OK) == HEALTH_ERROR_NONE);
    CHECK(health_error(ESP_FAIL) == HEALTH_ERROR_FAIL);
    CHECK(health_error(ESP_ERR_INVALID_STATE) == HEALTH_ERROR_INVALID_STATE);
    CHECK(health_error(ESP_ERR_WIFI_BASE + 1) == HEALTH_ERROR_OTHER);
    event = event_frame(HEALTH_BAD_TIMESTAMP, 0x0BB, 1, 0, -5);
    CHECK(event.timestamp_ms == (uint32_t)-5);

    if (!failures) printf("OK\n");
    return failures != 0;
}
'''


class NodeHealthTest(unittest.TestCase):
    def test_frames_use_the_sample_record_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'test.c').write_text(HARNESS)
            (temp / 'esp_err.h').write_text(ESP_ERR_STUB)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(NODE / 'src'),
                            '-I' + str(MASTER.parent), '-I' + str(temp),
                            str(temp / 'test.c'), '-o', str(temp / 'test')], check=True)
            result = subprocess.run([str(temp / 'test')], capture_output=True, text=True)
            self.assertEqual(result.stdout, 'OK\n', result.stdout)
            self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
