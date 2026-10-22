// The ESP-IDF headers declaring what the kernel implements. A file, not bindgen's
// `header_contents`: it evaluates macros with casts (`pdPASS`, ...) only then.
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
