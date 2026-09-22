#pragma once
#include <stdint.h>
/* Validated UTC from the shared RMC parser, before emitting GPS samples. */
void absolute_clock_observe_utc(int64_t utc_ms);
/* Zero until first valid GPS UTC. Resolution is ms, not guaranteed accuracy. */
int64_t absolute_clock_sample_utc(uint32_t sample_ms);
