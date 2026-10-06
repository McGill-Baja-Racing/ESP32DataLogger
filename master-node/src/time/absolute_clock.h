#pragma once
#include <stdint.h>
/* Validated UTC from the shared RMC parser, before emitting GPS samples. */
void absolute_clock_observe_utc(int64_t utc_ms);
/* Zero until first valid GPS UTC. Resolution is ms, not guaranteed accuracy. */
int64_t absolute_clock_sample_utc(uint32_t sample_ms);
/* Current UTC in milliseconds, or zero until the GPS clock is synchronized.
 * First synchronization also sets the system clock used by FAT timestamps. */
int64_t absolute_clock_now_utc(void);
