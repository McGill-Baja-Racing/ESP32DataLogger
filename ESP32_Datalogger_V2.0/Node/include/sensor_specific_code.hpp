#ifndef SENSOR_SPECIFIC_HPP
#define SENSOR_SPECIFIC_HPP

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

bool sensor_specific_data_mapping(int voltage, uint64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif // SENSOR_SPECIFIC_HPP