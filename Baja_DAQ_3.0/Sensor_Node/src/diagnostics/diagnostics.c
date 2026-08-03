#include "diagnostics/diagnostics.h"
#include <string.h>
#include "can/can_node.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol/app_protocol.h"
#include "time/time_sync.h"
#define EVENT_COUNT 16
#define QUEUE_COUNT 16
typedef struct{uint16_t code;uint8_t flags;uint8_t count;uint32_t timestamp;}event_t;
static event_t active[EVENT_COUNT];static QueueHandle_t queue;static SemaphoreHandle_t state_lock;static bool overflow;
static event_t*find(uint16_t c,bool create){event_t*free_slot=NULL;for(int i=0;i<EVENT_COUNT;i++){if(active[i].code==c)return&active[i];if(!active[i].code)free_slot=&active[i];}if(create&&free_slot){free_slot->code=c;return free_slot;}return NULL;}
static void enqueue(event_t e){if(xQueueSend(queue,&e,0)!=pdTRUE)overflow=true;}
static void report_locked(uint16_t code,bool on,diag_severity_t sev,bool degraded,bool occurrence){event_t*e=find(code,on);if(!e)return;bool was=e->flags&DIAG_FLAG_ACTIVE;if(occurrence&&e->count<255)e->count++;if(was==on)return;if(on&&!occurrence&&e->count<255)e->count++;e->flags=(on?DIAG_FLAG_ACTIVE:0)|(sev<<DIAG_SEVERITY_SHIFT)|(degraded?DIAG_FLAG_DATA_DEGRADED:0)|(time_sync_is_valid()?DIAG_FLAG_TIME_VALID:0);e->timestamp=time_sync_timestamp_ms();enqueue(*e);}
void diagnostics_report(uint16_t code,bool on,diag_severity_t sev,bool degraded){xSemaphoreTake(state_lock,portMAX_DELAY);report_locked(code,on,sev,degraded,false);xSemaphoreGive(state_lock);}
void diagnostics_occurrence(uint16_t c,diag_severity_t s,bool d){xSemaphoreTake(state_lock,portMAX_DELAY);report_locked(c,true,s,d,true);xSemaphoreGive(state_lock);}
void diagnostics_reannounce(void){xSemaphoreTake(state_lock,portMAX_DELAY);for(int i=0;i<EVENT_COUNT;i++)if(active[i].code&&(active[i].flags&DIAG_FLAG_ACTIVE))enqueue(active[i]);xSemaphoreGive(state_lock);}
static void task(void*a){event_t e;(void)a;for(;;){xQueueReceive(queue,&e,portMAX_DELAY);uint8_t p[8]={e.code,e.code>>8,e.flags,e.count,e.timestamp,e.timestamp>>8,e.timestamp>>16,e.timestamp>>24};bool sent=true;for(int i=0;i<3;i++){if(can_node_send(CAN_ID_DIAGNOSTIC,p,8)!=ESP_OK){sent=false;break;}vTaskDelay(pdMS_TO_TICKS(20));}if(!sent){if(xQueueSendToFront(queue,&e,0)!=pdTRUE)overflow=true;vTaskDelay(pdMS_TO_TICKS(250));continue;}if(overflow){overflow=false;diagnostics_occurrence(DIAG_QUEUE_OVERFLOW,DIAG_WARNING,true);}else if(e.code!=DIAG_QUEUE_OVERFLOW){diagnostics_report(DIAG_QUEUE_OVERFLOW,false,DIAG_WARNING,true);}}}
esp_err_t diagnostics_init(void){state_lock=xSemaphoreCreateMutex();queue=xQueueCreate(QUEUE_COUNT,sizeof(event_t));if(!state_lock||!queue)return ESP_ERR_NO_MEM;return xTaskCreate(task,"diagnostics",3072,NULL,4,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;}
