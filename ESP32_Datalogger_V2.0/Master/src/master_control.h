#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"

bool master_start_logging(void);
bool master_stop_logging(void);
esp_err_t master_format_status_json(char *out, size_t out_size);
esp_err_t master_format_files_json(char *out, size_t out_size);
esp_err_t master_format_health_json(char *out, size_t out_size);
esp_err_t master_open_sd_file_for_download(const char *filename, FILE **out_file, long long *out_size);
esp_err_t master_get_node_config_text(char **out_text, bool *out_from_sd);
esp_err_t master_save_node_config_text(const char *text, size_t len, bool apply_now);
esp_err_t master_reload_node_config(void);
esp_err_t master_set_log_mode_text(const char *mode_text);
bool master_submit_local_sample(uint32_t can_id,
                                uint32_t value,
                                uint32_t timestamp_ms,
                                bool preview_enabled);
