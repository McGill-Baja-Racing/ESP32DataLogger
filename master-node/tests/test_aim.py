"""Host checks of the production decoder, routing, and dispatch lock boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AimTests(unittest.TestCase):
    def test_decoder_status_and_dispatch_lock(self):
        can = (ROOT / 'src/can/can_master.c').read_text()
        dispatch = can[can.index('void can_master_clear_receive_queue('):can.index('static void dispatch_task(')]
        main = (ROOT / 'src/main.c').read_text()
        start = main.index('static void handle_can_message(')
        handler = main[start:main.index('\n#endif', start)]
        harness = r'''
#include <assert.h>
#include <string.h>
#include "aim/aim_adapter.h"
#include "protocol/app_protocol.h"
static unsigned logged, live, handled;
static bool locked, queued, recording;
static int rx_queue;
static can_message_t queued_frame;
#define pdTRUE 1
#define LOGGER_RUNNING 1
static void app_control_lock_can_processing(void) { assert(!locked);locked=true; }
static void app_control_unlock_can_processing(void) { assert(locked);locked=false; }
static int xQueueReset(int q) { (void)q;assert(locked);queued=false;return pdTRUE; }
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
static bool node_registry_is_state_frame(const can_message_t *s) { (void)s;return false; }
static void node_registry_update(const can_message_t *s) { (void)s; }
static void node_registry_record_sensor_frame(const can_message_t *s) { (void)s; }
#define ESP_LOGW(...) ((void)0)
''' + handler + r'''
static void handle(const can_message_t *s) { assert(locked);handled++;handle_can_message(s); }
static can_message_handler_t application_handler=handle;
''' + dispatch + r'''
int main(void) {
    aim_tracker_t t={0}; can_message_t s;
    assert(!strcmp(aim_tracker_status(&t,0,false).state,"unavailable"));
    assert(!strcmp(aim_tracker_status(&t,0,true).state,"waiting"));
    assert(aim_tracker_status(&t,0,true).last_frame_age_ms==-1);
    can_message_t f={.id=AIM_CAN_ID,.dlc=4,.received_us=1234567,.data=0xffff0000};
    assert(aim_decode(&t,&f,&s));
    assert(s.id==0x703 && s.dlc==8 && s.data==((uint64_t)1234<<32));
    assert(!strcmp(aim_tracker_status(&t,f.received_us+499999,true).state,"receiving"));
    assert(!strcmp(aim_tracker_status(&t,f.received_us+500000,true).state,"stale"));
    f.data=65535;f.received_us+=1000000; /* counter wraps to zero, maximum RPM */
    assert(aim_decode(&t,&f,&s) && (uint32_t)s.data==65535);
    assert(t.counter_discontinuities==0);
    assert(!strcmp(aim_tracker_status(&t,f.received_us,true).state,"receiving"));
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
    /* Idle dispatch updates health but does not log. */
    rx_queue=1;queued_frame=f;queued=true;
    dispatch_next_message();assert(!locked && handled==1 && logged==0);
    assert(app_tracker.valid_frames==1);
    /* START clears idle backlog under the same lock as dequeue. */
    queued=true;app_control_lock_can_processing();
    recording=true;can_master_clear_receive_queue();app_control_unlock_can_processing();
    dispatch_next_message();assert(handled==1 && logged==0); /* queue cleared after peek */
    queued=true;dispatch_next_message();assert(handled==2 && logged==1 && live==1);
    queued=true;dispatch_next_message();assert(logged==2); /* repeated values retained */
    /* STOP prevents admission but leaves health monitoring active. */
    app_control_lock_can_processing();recording=false;app_control_unlock_can_processing();
    queued=true;dispatch_next_message();assert(logged==2 && app_tracker.valid_frames==4);
    queued=true;app_control_lock_can_processing();
    can_master_clear_receive_queue();recording=true;app_control_unlock_can_processing();
    dispatch_next_message();assert(logged==2);
    queued=true;dispatch_next_message();assert(logged==3 && live==3);
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
