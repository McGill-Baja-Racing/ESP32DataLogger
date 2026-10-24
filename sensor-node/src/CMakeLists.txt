idf_component_register(
    SRCS
        "main.c"
        "can/can_node.c"
        "health/node_health.c"
        "sampler/sampler.c"
        "sensors/sensor_registry.c"
        "sensors/adc_input.c"
        "sensors/front_brake.c"
        "sensors/rear_brake.c"
        "sensors/bearing_encoder.c"
        "sensors/generic_adc.c"
        "sensors/engine_rpm.c"
        "sensors/mpu6500.c"
        "time/time_sync.c"
        "update/firmware_update.c"
    INCLUDE_DIRS
        "."
        "../.."
        "can"
        "sampler"
        "sensors"
        "time"
)

# Nothing references the image identity, so the linker would drop it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u firmware_identity")
