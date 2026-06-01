#include <stdio.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "../lib/can/frames.c"
#include "time_sync.hpp"

#include <atomic>


/*
Using an atomic value for the timestamp since atomic operations do not block.
Readers can safely access the latest fully written value while another thread updates it.
A reader will see either the old value or the new value, never a partially updated value.
*/

static volatile std::atomic<int64_t> node_time_offset_us = 0;


void update_time_offset(uint64_t t_main_us){
    // us is micro seconds
    int64_t t_local_us = esp_timer_get_time();
    node_time_offset_us.store((int64_t)t_main_us - t_local_us);
}

uint64_t package_timestamp_with_data(uint32_t data){
    
    int64_t t_local_sample_us = esp_timer_get_time();
    int64_t t_main_sample_us = t_local_sample_us + node_time_offset_us.load();
    int32_t timestamp = (int32_t)t_main_sample_us/1000 ; // Convert it from us to ms
        
    return ((uint64_t)(uint32_t)timestamp << 32) | ((uint64_t)(uint32_t)data );
}

     