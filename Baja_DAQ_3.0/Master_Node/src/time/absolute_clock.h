#pragma once
#include <stdint.h>
/* Called by the existing GPS UART reader before emitting samples. */
void absolute_clock_observe_rmc(const char *line);
/* Zero until first valid GPS UTC. Resolution is ms, not guaranteed accuracy. */
int64_t absolute_clock_sample_utc(uint32_t sample_ms);
