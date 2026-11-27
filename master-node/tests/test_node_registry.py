"""Check node liveness is tracked from boot, with or without a recording."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Just enough ESP-IDF and FreeRTOS to build node_registry.c on the host.
STUBS = {
    'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 0x101\n',
    'esp_log.h': '#pragma once\n#define ESP_LOGI(tag, ...) ((void)(tag))\n#define ESP_LOGW(tag, ...) ((void)(tag))\n',
    'freertos/FreeRTOS.h': '#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\n'
                           'typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n'
                           '#define portENTER_CRITICAL(m) ((void)(m))\n#define portEXIT_CRITICAL(m) ((void)(m))\n'
                           '#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))\n#define pdPASS 1\n',
    'freertos/task.h': '#pragma once\nextern TickType_t test_tick;\n'
                       'static inline TickType_t xTaskGetTickCount(void) { return test_tick; }\n'
                       'static inline void vTaskDelay(TickType_t t) { (void)t; }\n'
                       '#define xTaskCreate(fn, name, stack, arg, prio, handle) ((void)(fn), pdPASS)\n',
    'health/node_health.h': '#pragma once\n#include <stdint.h>\n#include "esp_err.h"\n'
                            '#include "protocol/app_protocol.h"\n'
                            'static inline void node_health_count(health_code_t c, uint32_t n, uint32_t s, esp_err_t e)\n'
                            '{ (void)c; (void)n; (void)s; (void)e; }\n',
}

HARNESS = r'''
#include <assert.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "protocol/app_protocol.h"
#include "node_state/node_registry.h"
TickType_t test_tick;
int main(void)
{
    assert(node_registry_init() == ESP_OK);
    /* No START ever happens: frames still count. */
    test_tick = 100;
    can_message_t brake = {.id = CAN_ID_FRONT_BRAKE, .dlc = 8};
    node_registry_record_sensor_frame(&brake);
    assert(!strcmp(node_registry_state_name(1), "active"));
    test_tick = 100 + 3001;
    assert(!strcmp(node_registry_state_name(1), "waiting"));
    node_registry_record_health_frame(3);
    assert(!strcmp(node_registry_state_name(3), "waiting"));
    for (uint8_t id = 1; id < CAN_NODE_SLOT_COUNT; id++) {
        assert(strcmp(node_registry_state_name(id), "off") != 0);
    }
    assert(!strcmp(node_registry_state_name(6), "disabled"));
    return 0;
}
'''


class NodeRegistryTests(unittest.TestCase):
    def test_tracks_nodes_without_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for name, text in STUBS.items():
                (tmp / name).parent.mkdir(parents=True, exist_ok=True)
                (tmp / name).write_text(text)
            (tmp / 'test.c').write_text(HARNESS)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(tmp), '-I' + str(ROOT / 'src'), '-I' + str(ROOT.parent),
                            str(tmp / 'test.c'), str(ROOT / 'src/node_state/node_registry.c'),
                            '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)
