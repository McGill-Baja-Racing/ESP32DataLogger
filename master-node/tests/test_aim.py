"""Host checks of the production decoder, routing, and dispatch lock boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AimTests(unittest.TestCase):
    def test_decoder_status_and_dispatch_lock(self):
        can = (ROOT / 'src/can/can_master.c').read_text()
        dispatch = can[can.index('/* Keep dequeue'):can.index('static void dispatch_task(')]
        main = (ROOT / 'src/main.c').read_text()
        start = main.index('static void handle_can_message(')
        handler = main[start:main.index('static void handle_gps_sample(', start)]
        harness = r'''
#include <assert.h>
#include <string.h>
#include "aim/aim_adapter.h"
#include "protocol/app_protocol.h"
#define ESP_OK 0
static unsigned logged, live, handled;
static bool locked, queued, recording;
static int rx_queue;
static can_message_t queued_frame;
#define pdTRUE 1
#define LOGGER_RUNNING 1
static void app_control_lock_can_processing(void) { assert(!locked);locked=true; }
static void app_control_unlock_can_processing(void) { assert(locked);locked=false; }
static int xQueueReceive(int q,can_message_t *out,int wait) {
    (void)q;assert(locked && wait==0);
    if(!queued)return 0;
    *out=queued_frame;queued=false;return pdTRUE;
}
static int data_logger_state(void) { return recording?LOGGER_RUNNING:0; }
static void data_logger_enqueue(const can_message_t *s) { assert(locked);assert(s->id==CAN_ID_AIM_RPM);logged++; }
static void live_data_record(const can_message_t *s) { (void)s;assert(locked);live++; }
static aim_tracker_t app_tracker;
bool aim_adapter_receive(const can_message_t *f,can_message_t *s) { return aim_decode(&app_tracker,f,s); }
static bool node_health_is_frame(const can_message_t *s) { (void)s;return false; }
static uint8_t node_health_node_id(const can_message_t *s) { (void)s;return 0; }
static void node_registry_record_health_frame(uint8_t node) { (void)node; }
static bool node_health_handle_frame(const can_message_t *s) { (void)s;return false; }
static void node_health_count(health_code_t c,uint32_t n,uint32_t id,int e) { (void)c;(void)n;(void)id;(void)e; }
static bool node_registry_is_state_frame(const can_message_t *s) { (void)s;return false; }
static void node_registry_update(const can_message_t *s) { (void)s; }
static void node_registry_record_sensor_frame(const can_message_t *s) { (void)s; }
static bool firmware_update_is_identity_frame(const can_message_t *s) { (void)s;return false; }
static void firmware_update_handle_identity(const can_message_t *s) { (void)s; }
static bool firmware_update_is_reply_frame(const can_message_t *s) { (void)s;return false; }
static void firmware_update_handle_reply(const can_message_t *s) { (void)s; }
#define ESP_LOGW(...) ((void)0)
''' + handler + r'''
static void handle(const can_message_t *s) { assert(locked);handled++;handle_can_message(s); }
static can_message_handler_t application_handler=handle;
''' + dispatch + r'''
int main(void) {
    aim_tracker_t t={0}; can_message_t s;
    assert(!strcmp(aim_tracker_status(&t,0).state,"waiting"));
    assert(aim_tracker_status(&t,0).last_frame_age_ms==-1);
    can_message_t f={.id=AIM_CAN_ID,.dlc=4,.received_us=1234567,.data=0xffff0000};
    assert(aim_decode(&t,&f,&s));
    assert(s.id==0x703 && s.dlc==8 && s.data==((uint64_t)1234<<32));
    assert(!strcmp(aim_tracker_status(&t,f.received_us+499999).state,"receiving"));
    assert(!strcmp(aim_tracker_status(&t,f.received_us+500000).state,"stale"));
    f.data=65535;f.received_us+=1000000; /* counter wraps to zero, maximum RPM */
    assert(aim_decode(&t,&f,&s) && (uint32_t)s.data==65535);
    assert(t.counter_discontinuities==0);
    assert(!strcmp(aim_tracker_status(&t,f.received_us).state,"receiving"));
    assert(aim_decode(&t,&f,&s)); /* repeated counter is diagnostic, value retained */
    f.data=((uint64_t)100<<16)|3000;
    assert(aim_decode(&t,&f,&s));
    f.data=3000;assert(aim_decode(&t,&f,&s)); /* restart */
    assert(t.counter_discontinuities==3);
    f.dlc=8;assert(!aim_decode(&t,&f,&s));f.dlc=4;
    f.extended=true;assert(!aim_decode(&t,&f,&s));f.extended=false;
    f.remote=true;assert(!aim_decode(&t,&f,&s));f.remote=false;
    assert(t.malformed_frames==3 && t.valid_frames==5);
    f.id=0x501;assert(!aim_decode(&t,&f,&s));assert(t.malformed_frames==3);
    f.id=AIM_CAN_ID;
    /* Idle dispatch updates health and live data but does not log. */
    rx_queue=1;queued_frame=f;queued=true;
    dispatch_next_message();assert(!locked && handled==1 && logged==0 && live==1);
    assert(app_tracker.valid_frames==1);
    /* While recording, AiM reaches both the log and live data. */
    app_control_lock_can_processing();recording=true;app_control_unlock_can_processing();
    queued=true;dispatch_next_message();assert(handled==2 && logged==1 && live==2);
    queued=true;dispatch_next_message();assert(logged==2 && live==3); /* repeated values retained */
    /* STOP prevents admission; health and live data keep going. */
    app_control_lock_can_processing();recording=false;app_control_unlock_can_processing();
    queued=true;dispatch_next_message();assert(logged==2 && live==4 && app_tracker.valid_frames==4);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory)
            (tmp/'esp_err.h').write_text('typedef int esp_err_t;\n')
            (tmp/'test.c').write_text(harness)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-I'+str(tmp),'-I'+str(ROOT/'src'),'-I'+str(ROOT.parent),
                            str(tmp/'test.c'),str(ROOT/'src/aim/aim_decode.c'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True)
