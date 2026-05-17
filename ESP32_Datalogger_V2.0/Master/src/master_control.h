#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

bool master_start_logging(void);
bool master_stop_logging(void);
esp_err_t master_format_status_json(char *out, size_t out_size);
esp_err_t master_format_files_json(char *out, size_t out_size);
esp_err_t master_open_sd_file_for_download(const char *filename, FILE **out_file, long long *out_size);
