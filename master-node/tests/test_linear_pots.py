"""Exercise linear pot drivers and check sample IDs against the control range."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SENSORS = ROOT / 'sensor-node/src/sensors'


class LinearPotTests(unittest.TestCase):
    def test_drivers_and_adc_failures(self):
        harness = r'''
#include <assert.h>
#include "sensor.h"
#include "adc_input.h"
#include "protocol/app_protocol.h"
static int init_calls, channel_seen;
static esp_err_t result;
static const int millivolts[] = {750, 2250};
esp_err_t adc_input_init(void) { init_calls++; return result; }
esp_err_t adc_input_read_mv(uint8_t gpio, int *mv) {
    assert(gpio < 2); channel_seen = gpio;
    if (result == ESP_OK) *mv = millivolts[gpio];
    return result;
}
int main(void) {
    size_t count;
    sensor_t *sensors = sensor_registry(&count);
    assert(count == 2);
    const uint32_t ids[] = {
#if NODE_FIXED_FRONT_SHOCK_CONFIG
        CAN_ID_FRONT_LIN_1, CAN_ID_FRONT_LIN_2
#else
        CAN_ID_REAR_LIN_1, CAN_ID_REAR_LIN_2
#endif
    };
    for (unsigned i = 0; i < count; i++) {
        sensor_t *s = &sensors[i];
        assert(s->can_id == ids[i] && s->period_us == 10000);
        assert(protocol_sensor_node_id(s->can_id) == NODE_ID);
        assert(s->init(s) == ESP_OK);
        int32_t value = -123;
        assert(s->read(s, &value) == ESP_OK);
        assert(channel_seen == (int)i && value == (i ? 75 : 25));
        result = ESP_FAIL;
        assert(s->init(s) == ESP_FAIL);
        value = -123;
        assert(s->read(s, &value) == ESP_FAIL && value == -123);
        result = ESP_OK;
    }
    assert(init_calls == 4);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'esp_err.h').write_text(
                '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n')
            (temp / 'test.c').write_text(harness)
            for config, node in [('FRONT', 7), ('REAR', 8)]:
                with self.subTest(config=config):
                    subprocess.run([
                        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        f'-DNODE_FIXED_{config}_SHOCK_CONFIG=1', f'-DNODE_ID={node}',
                        '-I' + str(temp), '-I' + str(ROOT), '-I' + str(SENSORS),
                        str(temp / 'test.c'), str(SENSORS / 'sensor_registry.c'),
                        str(SENSORS / 'shock_displacement.c'), '-o', str(temp / 'test')
                    ], check=True)
                    subprocess.run([str(temp / 'test')], check=True)

    def test_sample_ids_do_not_overlap_node_state_frames(self):
        harness = r'''
#include <assert.h>
#include "protocol/app_protocol.h"
int main(void) {
    for (uint32_t id = 0; id <= 0x7ff; id++) {
        uint8_t node = protocol_sensor_node_id(id);
        if (!node) continue;
        assert(node < CAN_NODE_SLOT_COUNT && (CAN_NODE_ID_MASK & (1U << node)));
        assert(id != CAN_ID_START && id != CAN_ID_STOP && id != CAN_ID_MASTER_TIME);
        assert(id <= CAN_ID_NODE_STATE_BASE ||
               id >= CAN_ID_NODE_STATE_BASE + CAN_NODE_SLOT_COUNT);
    }
    assert(protocol_sensor_node_id(CAN_ID_FRONT_LIN_1) == 7);
    assert(protocol_sensor_node_id(CAN_ID_FRONT_LIN_2) == 7);
    assert(protocol_sensor_node_id(CAN_ID_REAR_LIN_1) == 8);
    assert(protocol_sensor_node_id(CAN_ID_REAR_LIN_2) == 8);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'test.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT), str(temp / 'test.c'),
                            '-o', str(temp / 'test')], check=True)
            subprocess.run([str(temp / 'test')], check=True)
