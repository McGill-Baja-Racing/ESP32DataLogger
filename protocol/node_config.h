#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H


/**
 * @file node_config.h
 * @brief Compile-time node configuration fallbacks.
 * 
 * This file assigns a safe default value (0) to any configuration macro 
 * that was not explicitly defined via the platformio.ini node build environment flags.
 */

// =============================================================================
// Sensor Configuration Fallbacks (Option A Pattern)
// =============================================================================
#ifndef NODE_FIXED_BRAKE_CONFIG
    #define NODE_FIXED_BRAKE_CONFIG 0
#endif

#ifndef NODE_FIXED_ENCODER_CONFIG
    #define NODE_FIXED_ENCODER_CONFIG 0
#endif

#ifndef NODE_FIXED_ENGINE_CONFIG
    #define NODE_FIXED_ENGINE_CONFIG 0
#endif

#ifndef NODE_FIXED_FRONT_SHOCK_CONFIG
    #define NODE_FIXED_FRONT_SHOCK_CONFIG 0
#endif

#ifndef NODE_FIXED_REAR_SHOCK_CONFIG
    #define NODE_FIXED_REAR_SHOCK_CONFIG 0
#endif

#ifndef NODE_FIXED_ADC_CONFIG
    #define NODE_FIXED_ADC_CONFIG 0
#endif

#ifndef NODE_FIXED_MPU_CONFIG
    #define NODE_FIXED_MPU_CONFIG 0
#endif

// =============================================================================
// Test Parameters
// =============================================================================
#ifndef SENSOR_SERIAL_TEST
    #define SENSOR_SERIAL_TEST 0
#endif

#endif // NODE_CONFIG_H