#pragma once
#include "app/runtime_internal.h"

bool parse_optional_u32_field(const char *field, uint32_t *out_value, bool *out_valid);

bool parse_optional_f32_field(const char *field, float *out_value, bool *out_valid);

bool parse_replay_row(char *line, ReplayData *row);

void read_sample_data(void *arg);
