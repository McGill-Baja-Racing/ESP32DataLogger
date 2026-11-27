"""Exercise the production standalone CAN receiver with fake TWAI/FreeRTOS."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]

class CanTransportTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'Host C++ compiler unavailable')
    def test_startup_servo_routing_lengths_signed_values_and_timestamps(self):
        headers = {
            'esp_err.h': '#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1, ESP_ERR_NO_MEM=2;\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
            'esp_log.h': '#pragma once\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n',
            'freertos/FreeRTOS.h': '#pragma once\nusing BaseType_t=int;\nconstexpr int pdTRUE=1,pdFALSE=0,pdPASS=1;\nconstexpr unsigned portMAX_DELAY=~0u;\n',
            'freertos/queue.h': '#pragma once\n#include "FreeRTOS.h"\nusing QueueHandle_t=void*;\nQueueHandle_t xQueueCreate(unsigned,unsigned);\nint xQueueSend(QueueHandle_t,const void*,unsigned);\nint xQueueSendFromISR(QueueHandle_t,const void*,BaseType_t*);\nint xQueueReceive(QueueHandle_t,void*,unsigned);\n',
            'freertos/task.h': '#pragma once\n#include "FreeRTOS.h"\nint xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*);\n',
            'esp_twai.h': '''#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
using twai_node_handle_t=void*;
struct twai_rx_done_event_data_t {};
struct twai_frame_t {struct {uint32_t id; uint8_t dlc;} header; uint8_t *buffer; size_t buffer_len;};
struct twai_event_callbacks_t {bool (*on_rx_done)(twai_node_handle_t,const twai_rx_done_event_data_t*,void*);};
int twai_node_receive_from_isr(twai_node_handle_t,twai_frame_t*);
int twai_node_register_event_callbacks(twai_node_handle_t,const twai_event_callbacks_t*,void*);
int twai_node_enable(twai_node_handle_t);
''',
            'esp_twai_onchip.h': '''#pragma once
#include "esp_twai.h"
constexpr int GPIO_NUM_5=5,GPIO_NUM_4=4;
struct twai_onchip_node_config_t {struct {int tx,rx;} io_cfg; struct {int bitrate;} bit_timing; unsigned tx_queue_depth;};
int twai_new_node_onchip(const twai_onchip_node_config_t*,twai_node_handle_t*);
''',
        }
        harness = r'''
#include "can/transport.h"
#include "canopen/drive.h"
#include "esp_twai_onchip.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <vector>
#include <cmath>
struct Idle {};
struct Queue { std::deque<canopen_drive_can_rx_item_t> items; };
static QueueHandle_t raw, sdo;
static twai_event_callbacks_t callbacks;
static void(*worker)(void*);
static canopen_drive_can_rx_item_t incoming;
static unsigned queues, initializations;
static float actual_position;
struct Feedback {unsigned pdo; int32_t value; int64_t time;};
static std::vector<Feedback> feedback;
int64_t esp_timer_get_time() {return 1234567;}
QueueHandle_t xQueueCreate(unsigned depth,unsigned size) {
    assert(depth==256 && size==sizeof(incoming));
    auto *q=new Queue;
    if(queues++==0) raw=q; else sdo=q;
    return q;
}
int xQueueSend(QueueHandle_t q,const void *value,unsigned) {
    static_cast<Queue*>(q)->items.push_back(*static_cast<const canopen_drive_can_rx_item_t*>(value));return 1;
}
int xQueueSendFromISR(QueueHandle_t q,const void *value,BaseType_t *woken) {*woken=1;return xQueueSend(q,value,0);}
int xQueueReceive(QueueHandle_t q,void *value,unsigned) {
    auto &items=static_cast<Queue*>(q)->items;
    if(items.empty()) throw Idle{};
    *static_cast<canopen_drive_can_rx_item_t*>(value)=items.front();items.pop_front();return 1;
}
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void*,unsigned,void*) {worker=fn;return 1;}
int twai_new_node_onchip(const twai_onchip_node_config_t *cfg,twai_node_handle_t *node) {
    assert(cfg->io_cfg.tx==5 && cfg->io_cfg.rx==4 && cfg->bit_timing.bitrate==500000 && cfg->tx_queue_depth==8);
    *node=reinterpret_cast<void*>(1);return 0;
}
int twai_node_register_event_callbacks(twai_node_handle_t,const twai_event_callbacks_t *cb,void*) {callbacks=*cb;return 0;}
int twai_node_enable(twai_node_handle_t) {return 0;}
int twai_node_receive_from_isr(twai_node_handle_t,twai_frame_t *frame) {
    frame->header.id=incoming.id;frame->header.dlc=incoming.dlc;
    std::memcpy(frame->buffer,incoming.data,8);return 0;
}
int canopen_drive_init(twai_node_handle_t,QueueHandle_t q,uint8_t node,const char*) {
    assert(q==sdo && node==1);++initializations;return 0;
}
int canopen_drive_set_actual_position_feedback(float value) {actual_position=value;return 0;}
void drive_feedback_record(unsigned pdo,int32_t value,int64_t time) {feedback.push_back({pdo,value,time});}
void frame(uint32_t id,uint8_t length,uint32_t value) {
    incoming={};incoming.id=id;incoming.dlc=length;
    for(unsigned i=0;i<4;++i)incoming.data[i]=uint8_t(value>>(8*i));
    assert(callbacks.on_rx_done(reinterpret_cast<void*>(1),nullptr,nullptr));
    try {worker(nullptr);} catch(const Idle&) {}
}
int main() {
    assert(servo_can_start()==0 && initializations==1 && queues==2 && feedback.empty());
    assert(servo_can_start()==ESP_ERR_INVALID_STATE && initializations==1);
    frame(0x581,8,0x12345678); auto &items=static_cast<Queue*>(sdo)->items;
    assert(items.size()==1 && items.front().received_us==1234567 && items.front().data[0]==0x78);
    frame(0x581,7,0);frame(0x582,8,0);
    assert(items.size()==1);
    frame(0x181,2,0x0237);frame(0x281,4,uint32_t(-12500));
    frame(0x381,4,uint32_t(-113));frame(0x481,2,uint16_t(-123));
    assert(feedback.size()==4 && feedback[0].pdo==1 && feedback[0].value==0x0237);
    assert(feedback[1].pdo==2 && feedback[1].value==-12500 && std::fabs(actual_position+1.25f)<0.0001f);
    assert(feedback[2].pdo==3 && feedback[2].value==-113);
    assert(feedback[3].pdo==4 && feedback[3].value==-123);
    for(auto &sample:feedback) assert(sample.time==1234567);
    frame(0x181,1,0);frame(0x281,3,0);frame(0x381,3,0);frame(0x481,1,0);
    frame(0x182,8,0);frame(0x123,8,0);frame(0x701,1,0);
    assert(feedback.size()==4 && actual_position==-1.25f);
    delete static_cast<Queue*>(raw);delete static_cast<Queue*>(sdo);
}
'''
        with tempfile.TemporaryDirectory(prefix='ecvt-can-') as directory:
            tmp=Path(directory)
            for name,content in headers.items():
                path=tmp/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
            source=tmp/'can.cpp';source.write_text(harness)
            binary=tmp/'can'
            subprocess.run(['c++','-std=c++17','-I',str(tmp),'-I',str(PROJECT/'src'),
                            str(PROJECT/'src/can/transport.cpp'),str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
