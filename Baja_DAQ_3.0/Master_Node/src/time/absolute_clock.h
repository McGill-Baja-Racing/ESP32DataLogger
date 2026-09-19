#pragma once
#include <stdint.h>
/* Called by the existing GPS UART reader before emitting samples. */
void absolute_clock_observe_rmc(const char *line);
/* Zero until first valid GPS UTC. Resolution is ms, not guaranteed accuracy. */
int64_t absolute_clock_sample_utc(uint32_t sample_ms);
/* Current UTC in milliseconds, or zero until the GPS clock is synchronized.
 * First synchronization also sets the system clock used by FAT timestamps. */
int64_t absolute_clock_now_utc(void);
