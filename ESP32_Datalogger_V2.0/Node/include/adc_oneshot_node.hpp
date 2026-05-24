#ifndef ADC_ONESHOT_NODE_HPP
#define ADC_ONESHOT_NODE_HPP

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>


bool adc_oneshot_and_calib_init();
void adc_oneshot_and_calib_deinit(bool calib_performed);
void adc_oneshot_main(bool *loop_condition,bool do_calibration1_chan0);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif