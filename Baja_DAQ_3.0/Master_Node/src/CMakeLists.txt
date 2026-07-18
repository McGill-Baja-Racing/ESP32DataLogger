idf_component_register(
    SRCS
        "main.c"
        "can/can_master.c"
        "console/serial_console.c"
        "logger/data_logger.c"
        "node_state/node_registry.c"
        "storage/sd_card.c"
        "time/time_beacon.c"
    INCLUDE_DIRS "."
    REQUIRES
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_twai
        esp_driver_gpio
        esp_timer
)
