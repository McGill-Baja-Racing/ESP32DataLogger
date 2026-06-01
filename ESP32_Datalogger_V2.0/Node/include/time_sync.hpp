#ifndef TIME_SYNC_HPP
#define TIME_SYNC_HPP

#include <cstdint>

// Your own C++ declarations go outside extern "C"
void update_time_offset(uint64_t t_main_us);
uint64_t package_timestamp_with_data(uint32_t data);

#endif