#include "diagnostics/diagnostic_registry.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "protocol/app_protocol.h"
static SemaphoreHandle_t lock;
static diagnostic_event_t history[DIAGNOSTIC_HISTORY_COUNT];
static size_t history_count;
static const char *TAG="Diagnostics";
const char *diagnostic_code_name(uint16_t c){switch(c){case DIAG_CAN_TX_FAILED:return"CAN TX failed";case DIAG_CAN_RX_OVERFLOW:return"CAN RX overflow";case DIAG_CAN_WARNING:return"CAN warning";case DIAG_CAN_PASSIVE:return"CAN passive";case DIAG_CAN_BUS_OFF:return"CAN bus-off";case DIAG_CAN_RECOVERY_FAILED:return"CAN recovery failed";case DIAG_TIME_STALE:return"Time sync stale";case DIAG_SAMPLE_DEADLINE:return"Sampling deadline missed";case DIAG_SAMPLE_QUEUE_OVERFLOW:return"Sample queue overflow";case DIAG_QUEUE_OVERFLOW:return"Diagnostic queue overflow";case DIAG_BEARING_TRANSITION:return"Invalid bearing transition";case DIAG_BEARING_RPM:return"Bearing RPM implausible";case DIAG_ENGINE_REJECTED_PULSE:return"Rejected engine pulses";case DIAG_ENGINE_RPM:return"Engine RPM implausible";default:return"Unknown diagnostic";}}
const char *diagnostic_severity_name(uint8_t f){switch((f&DIAG_SEVERITY_MASK)>>DIAG_SEVERITY_SHIFT){case 0:return"info";case 1:return"warning";case 2:return"error";default:return"critical";}}
esp_err_t diagnostic_registry_init(void){lock=xSemaphoreCreateMutex();return lock?ESP_OK:ESP_ERR_NO_MEM;}
bool diagnostic_registry_is_frame(const can_message_t*m){return m&&m->id>CAN_ID_DIAGNOSTIC_BASE&&m->id<CAN_ID_DIAGNOSTIC_BASE+7&&m->dlc==8;}
bool diagnostic_registry_update(const can_message_t*m,diagnostic_event_t*out){if(!diagnostic_registry_is_frame(m))return false;diagnostic_event_t e={.node_id=m->id-CAN_ID_DIAGNOSTIC_BASE,.code=m->data&0xffff,.flags=(m->data>>16)&0xff,.count=(m->data>>24)&0xff,.timestamp_ms=m->data>>32};xSemaphoreTake(lock,portMAX_DELAY);for(size_t i=0;i<history_count;i++){diagnostic_event_t*p=&history[i];if(p->node_id==e.node_id&&p->code==e.code){if(p->flags==e.flags&&p->count==e.count&&p->timestamp_ms==e.timestamp_ms){xSemaphoreGive(lock);return false;}memmove(p,p+1,(history_count-i-1)*sizeof(e));history_count--;break;}}if(history_count<DIAGNOSTIC_HISTORY_COUNT)history_count++;memmove(&history[1],&history[0],(history_count-1)*sizeof(e));history[0]=e;xSemaphoreGive(lock);ESP_LOGW(TAG,"Node %u %s %s severity=%s count=%u timestamp=%"PRIu32" ms%s",e.node_id,(e.flags&DIAG_FLAG_ACTIVE)?"ACTIVE":"CLEARED",diagnostic_code_name(e.code),diagnostic_severity_name(e.flags),e.count,e.timestamp_ms,(e.flags&DIAG_FLAG_TIME_VALID)?"":" (local/unsynchronized)");if(out)*out=e;return true;}
size_t diagnostic_registry_snapshot(diagnostic_event_t*out,size_t cap){xSemaphoreTake(lock,portMAX_DELAY);size_t n=0;for(int active_first=1;active_first>=0;active_first--)for(size_t i=0;i<history_count&&n<cap;i++)if(!!(history[i].flags&DIAG_FLAG_ACTIVE)==active_first)out[n++]=history[i];xSemaphoreGive(lock);return n;}
