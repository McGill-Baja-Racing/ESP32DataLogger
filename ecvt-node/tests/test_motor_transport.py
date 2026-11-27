"""Exercise the actual C servo driver with a fake CAN transport, no hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class MotorTransportTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
    def test_payloads_cancellation_and_pending_buffer_lifetime(self):
        headers = {
            "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_TIMEOUT 3
""",
            "esp_check.h": """#pragma once
#define ESP_RETURN_ON_ERROR(expr,tag,message) do { int error=(expr); if(error) return error; } while(0)
""",
            "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
            "esp_log.h": """#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
""",
            "esp_twai.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef void *twai_node_handle_t;
typedef struct { struct { uint32_t id; bool ide; uint8_t dlc; } header; uint8_t *buffer; size_t buffer_len; } twai_frame_t;
esp_err_t twai_node_transmit(twai_node_handle_t node, const twai_frame_t *frame, int timeout);
esp_err_t twai_node_transmit_wait_all_done(twai_node_handle_t node, int timeout);
""",
            "freertos/FreeRTOS.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
""",
            "freertos/queue.h": """#pragma once
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
int xQueueReceive(QueueHandle_t queue, void *out, TickType_t timeout);
""",
            "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
""",
            "freertos/semphr.h": "#pragma once\n",
        }
        harness = r'''
#include "canopen/drive.h"
#include "drive/operation_context.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
static uint8_t bytes[8];
static unsigned id, dlc, transmissions, ticks;
static bool cancelled, allowed=true, wait_fails, stale_reply, wrong_width, abort_reply;
static const uint8_t *held_buffer;
bool drive_operation_cancelled(void) { return cancelled; }
bool drive_position_allowed(float position, float speed) { return allowed && isfinite(position) && speed>0; }
void drive_operation_error(esp_err_t error) { (void)error; }
TickType_t xTaskGetTickCount(void) { return ticks++; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
void vTaskDelay(TickType_t time) { ticks+=time; }
int xQueueReceive(QueueHandle_t queue, void *out, TickType_t timeout) {
    (void)queue; (void)timeout;
    canopen_drive_can_rx_item_t *reply=out;
    memset(reply,0,sizeof(*reply)); reply->id=0x581; reply->dlc=8;
    reply->received_us = stale_reply ? -1 : esp_timer_get_time();
    reply->data[0]=bytes[0]==0x40 ? 0x4f : 0x60;
    reply->data[4]=1;
    if (bytes[0]==0x40 && bytes[1]==0x41) { reply->data[0]=0x4b; reply->data[4]=0x40; reply->data[5]=0x12; }
    if (bytes[0]==0x40 && bytes[1]==0xfe) {
        reply->data[0]=0x43; reply->data[4]=0x78; reply->data[5]=0x56; reply->data[6]=0x34; reply->data[7]=0x92;
    }
    if (wrong_width) reply->data[0]=0x4f;
    if (abort_reply) reply->data[0]=0x80;
    memcpy(reply->data+1,bytes+1,3); return pdTRUE;
}
esp_err_t twai_node_transmit(twai_node_handle_t node,const twai_frame_t *frame,int timeout) {
    (void)node; assert(timeout==100); id=frame->header.id; dlc=frame->header.dlc;
    memcpy(bytes,frame->buffer,dlc); held_buffer=frame->buffer; ++transmissions; return ESP_OK;
}
esp_err_t twai_node_transmit_wait_all_done(twai_node_handle_t node,int timeout) {
    (void)node; assert(timeout==100); return wait_fails ? ESP_ERR_TIMEOUT : ESP_OK;
}
static int32_t word(unsigned offset) {
    uint32_t v=(uint32_t)bytes[offset]|((uint32_t)bytes[offset+1]<<8)|((uint32_t)bytes[offset+2]<<16)|((uint32_t)bytes[offset+3]<<24);
    return (int32_t)v;
}
int main(void) {
    assert(canopen_drive_init((void*)1,(void*)2,1,"test")==ESP_OK);
    uint8_t mode=0;
    assert(canopen_drive_sdo_read_u8(0x6061,0,&mode)==ESP_OK && mode==1);
    assert(transmissions==1 && id==0x601 && dlc==8);
    assert(bytes[0]==0x40 && bytes[1]==0x61 && bytes[2]==0x60 && bytes[3]==0);
    for(unsigned i=4;i<8;++i) assert(bytes[i]==0); // Only an SDO upload request.
    stale_reply=true;
    assert(canopen_drive_sdo_read_u8(0x6061,0,&mode)==ESP_FAIL);
    stale_reply=false;
    assert(canopen_drive_sdo_read_u8(0x6061,0,0)==ESP_ERR_INVALID_ARG);
    uint16_t status=0;
    uint32_t outputs=0;
    assert(canopen_drive_sdo_read_u16(0x6041,0,&status)==ESP_OK && status==0x1240);
    assert(canopen_drive_sdo_read_u32(0x60fe,1,&outputs)==ESP_OK && outputs==0x92345678u);
    assert(id==0x601 && bytes[0]==0x40 && bytes[1]==0xfe && bytes[2]==0x60 && bytes[3]==1);
    wrong_width=true;
    assert(canopen_drive_sdo_read_u32(0x60fe,1,&outputs)==ESP_FAIL && outputs==0x92345678u);
    wrong_width=false; stale_reply=true;
    assert(canopen_drive_sdo_read_u16(0x6041,0,&status)==ESP_FAIL && status==0x1240);
    stale_reply=false; abort_reply=true;
    assert(canopen_drive_sdo_read_u32(0x60fe,2,&outputs)==ESP_FAIL && outputs==0x92345678u);
    abort_reply=false;
    assert(canopen_drive_sdo_read_u16(0x6041,0,0)==ESP_ERR_INVALID_ARG);
    assert(canopen_drive_sdo_read_u32(0x60fe,1,0)==ESP_ERR_INVALID_ARG);
    assert(canopen_drive_send_target_position_and_speed(-1.25f,2000)==ESP_OK);
    assert(id==0x301 && dlc==8 && word(0)==-12500 && word(4)==333333);
    assert(canopen_drive_send_profile_accel_decel(1666,2300)==ESP_OK);
    assert(id==0x401 && word(0)==277667 && word(4)==383333);
    assert(canopen_drive_send_controlword(0x010f)==ESP_OK && id==0x201 && dlc==2 && bytes[0]==15 && bytes[1]==1);
    canopen_drive_set_actual_position_feedback(12.5f);
    canopen_drive_capture_current_position_as_zero();
    assert(canopen_drive_send_target_position_and_speed(1,60)==ESP_OK);
    assert(word(0)==135000 && word(4)==10000);
    unsigned before=transmissions;
    assert(canopen_drive_send_target_position_and_speed(300000,60)==ESP_ERR_INVALID_ARG && transmissions==before);
    allowed=false;
    assert(canopen_drive_send_target_position_and_speed(1,60)==ESP_ERR_INVALID_ARG && transmissions==before);
    allowed=true; cancelled=true;
    assert(canopen_drive_enable_position()==ESP_ERR_INVALID_STATE && transmissions==before);
    assert(canopen_drive_send_target_position_and_speed(1,60)==ESP_ERR_INVALID_STATE && transmissions==before);
    // The stop path must still be able to send a Halt after cancellation.
    assert(canopen_drive_sdo_write_u16(0x6040,0,0x010f)==ESP_OK);
    cancelled=false; wait_fails=true;
    assert(canopen_drive_send_target_position_and_speed(2,60)==ESP_ERR_TIMEOUT);
    uint8_t pending_copy[8]; memcpy(pending_copy,held_buffer,8); before=transmissions;
    assert(canopen_drive_send_target_position_and_speed(3,60)==ESP_ERR_TIMEOUT);
    assert(transmissions==before && memcmp(pending_copy,held_buffer,8)==0);
    // Mapping failures propagate instead of aborting/rebooting.
    assert(canopen_drive_configure_rpdo2()==ESP_ERR_TIMEOUT);
    wait_fails=false;
    assert(canopen_drive_configure_rpdo2()==ESP_OK);
}
'''
        with tempfile.TemporaryDirectory(prefix="ecvt-transport-") as directory:
            tmp = Path(directory)
            for name, content in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            source, binary = tmp / "motor_test.c", tmp / "motor_test"
            source.write_text(harness)
            subprocess.run([
                "cc", "-std=c11", "-I", str(tmp), "-I", str(PROJECT / "src"),
                "-I", str(PROJECT / "src/servo"), str(PROJECT / "src/canopen/drive.c"),
                str(source), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
