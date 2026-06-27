set(master_sources "main.c")

if(MASTER_GPS_ENABLED)
    list(APPEND master_sources "gps.c")
endif()

if(MASTER_RPM_ENABLED)
    list(APPEND master_sources "rpm_sampler.c")
endif()

idf_component_register(
    SRCS
        ${master_sources}
    INCLUDE_DIRS "."
    REQUIRES
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_twai
        esp_driver_uart
        esp_driver_gpio
        esp_adc
        esp_timer
        json
)
