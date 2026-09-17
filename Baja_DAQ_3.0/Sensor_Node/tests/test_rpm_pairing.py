"""Compile the master's pairing logic on the host and exercise freshness."""
from pathlib import Path
import subprocess
import tempfile
master = Path(__file__).resolve().parents[2] / 'Master_Node'
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    (temp / 'esp_err.h').write_text('typedef int esp_err_t;\n')
    (temp / 'test.c').write_text(r'''
#include <assert.h>
#include "protocol/rpm_pairing.h"
static can_message_t msg(uint32_t id, uint32_t ms, int32_t value) {
    return (can_message_t){id, 8, ((uint64_t)ms << 32) | (uint32_t)value};
}
int main(void) {
    rpm_pairing_t p = {0}; can_message_t out;
    can_message_t e = msg(CAN_ID_ENGINE_RPM, 1000, 3000);
    assert(!rpm_pairing_update(&p, &e, 1000000, &out));
    can_message_t w = msg(CAN_ID_BEARING_ENCODER, 980, -123);
    assert(!rpm_pairing_update(&p, &w, 990000, &out));
    assert(rpm_pairing_update(&p, &e, 1000000, &out));
    assert(out.id == CAN_ID_ENGINE_WHEEL_RPM && out.dlc == 8);
    assert((uint32_t)(out.data >> 32) == 1000 && (int32_t)out.data == -123);
    e = msg(CAN_ID_ENGINE_RPM, 1080, 0);
    assert(rpm_pairing_update(&p, &e, 1090000, &out));
    assert((int32_t)out.data == -123); /* stopped engine, moving wheel */
    assert(!rpm_pairing_update(&p, &e, 1090001, &out));
    e = msg(CAN_ID_ENGINE_RPM, 1081, 3000);
    assert(!rpm_pairing_update(&p, &e, 1090000, &out));
    e = msg(CAN_ID_ENGINE_RPM, 979, 3000);
    assert(!rpm_pairing_update(&p, &e, 1000000, &out));
    w = msg(CAN_ID_BEARING_ENCODER, UINT32_MAX - 9, 0);
    rpm_pairing_update(&p, &w, 2000000, &out);
    e = msg(CAN_ID_ENGINE_RPM, 10, 0);
    assert(rpm_pairing_update(&p, &e, 2020000, &out));
    assert((int32_t)out.data == 0 && (uint32_t)(out.data >> 32) == 10);
    e.dlc = 7; assert(!rpm_pairing_update(&p, &e, 2020000, &out));
    p.valid = false; e.dlc = 8;
    assert(!rpm_pairing_update(&p, &e, 2020000, &out));
}
''')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(temp), '-I'+str(master/'src'), str(temp/'test.c'), '-o', str(temp/'test')], check=True)
    subprocess.run([str(temp/'test')], check=True)
print('RPM pairing tests passed')
