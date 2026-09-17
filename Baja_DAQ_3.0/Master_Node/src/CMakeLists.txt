idf_component_register(
    SRCS
        "main.c"
        "app/app_control.c"
        "can/can_master.c"
        "console/serial_console.c"
        "gps/gps_receiver.c"
        "logger/data_logger.c"
        "live/live_data.c"
        "node_state/node_registry.c"
        "storage/sd_card.c"
        "time/time_beacon.c"
        "time/gps_time.c"
        "time/absolute_clock.c"
        "web/web_server.c"
        "web/index_html.S"
        "web/analysis_assets.S"
    INCLUDE_DIRS "."
    REQUIRES
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_twai
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        esp_http_server
        esp_netif
        esp_event
        nvs_flash
        espressif__esp_hosted
)

# Re-embed changed browser assets, including after switching implementations.
set_source_files_properties("web/index_html.S" PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set_source_files_properties("web/analysis_assets.S" PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/web/analysis.html;${CMAKE_CURRENT_SOURCE_DIR}/web/cvt_analysis.js;${CMAKE_CURRENT_SOURCE_DIR}/web/cvt_worker.js;${CMAKE_CURRENT_SOURCE_DIR}/web/cvt_ui.js;${CMAKE_CURRENT_SOURCE_DIR}/web/chart.umd.min.js;${CMAKE_CURRENT_SOURCE_DIR}/web/Chart.LICENSE.md")
