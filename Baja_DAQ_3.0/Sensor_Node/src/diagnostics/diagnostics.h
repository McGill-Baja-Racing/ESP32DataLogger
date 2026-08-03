#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum{DIAG_INFO=0,DIAG_WARNING=1,DIAG_ERROR=2,DIAG_CRITICAL=3}diag_severity_t;
esp_err_t diagnostics_init(void);
void diagnostics_report(uint16_t code,bool active,diag_severity_t severity,bool degraded);
void diagnostics_occurrence(uint16_t code,diag_severity_t severity,bool degraded);
void diagnostics_reannounce(void);
