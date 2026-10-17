// Force-included into every firmware C file by pio/cargo.py. The kernel supports only the options
// the firmware uses, so naming any other option of a supported ESP-IDF API is a compile error.
// The headers come first because poisoning only rejects uses after this point, and a macro is
// undefined before it is poisoned.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"

#undef xQueueSendToFront
#undef xQueueSendToFrontFromISR
#undef xQueueOverwrite
#undef xQueueOverwriteFromISR
#undef xSemaphoreCreateBinary
#undef vSemaphoreCreateBinary
#pragma GCC poison xQueueSendToFront xQueueSendToFrontFromISR xQueueOverwrite xQueueOverwriteFromISR
#pragma GCC poison xSemaphoreCreateBinary vSemaphoreCreateBinary
#pragma GCC poison ADC_UNIT_2 ADC_ULP_MODE_FSM ADC_ULP_MODE_RISCV ADC_ULP_MODE_LP_CORE
#pragma GCC poison ADC_ATTEN_DB_0 ADC_ATTEN_DB_2_5 ADC_ATTEN_DB_6 ADC_ATTEN_DB_11
#pragma GCC poison ADC_BITWIDTH_9 ADC_BITWIDTH_10 ADC_BITWIDTH_11 ADC_BITWIDTH_13
