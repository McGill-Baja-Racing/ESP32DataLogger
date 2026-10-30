// Force-included into every firmware C file by kernel/firmware.rs. The kernel supports only the options
// the firmware uses, so naming any other option of a supported ESP-IDF API is a compile error.
// The headers come first because poisoning only rejects uses after this point, and a macro is
// undefined before it is poisoned.
#pragma once

#include "../sys/wrapper.h"

#undef xQueueSendToFront
#undef xQueueSendToFrontFromISR
#undef xQueueOverwrite
#undef xQueueOverwriteFromISR
#undef xSemaphoreCreateBinary
#undef vSemaphoreCreateBinary
#undef xSemaphoreCreateRecursiveMutex
#undef queueSEND_TO_FRONT
#undef queueOVERWRITE
#undef queueQUEUE_TYPE_BINARY_SEMAPHORE
#undef queueQUEUE_TYPE_COUNTING_SEMAPHORE
#undef queueQUEUE_TYPE_RECURSIVE_MUTEX
#pragma GCC poison xQueueSendToFront xQueueSendToFrontFromISR xQueueOverwrite xQueueOverwriteFromISR
#pragma GCC poison xSemaphoreCreateBinary vSemaphoreCreateBinary xSemaphoreCreateRecursiveMutex
#pragma GCC poison queueSEND_TO_FRONT queueOVERWRITE queueQUEUE_TYPE_BINARY_SEMAPHORE
#pragma GCC poison queueQUEUE_TYPE_COUNTING_SEMAPHORE queueQUEUE_TYPE_RECURSIVE_MUTEX
#pragma GCC poison enable_self_test enable_loopback enable_listen_only no_receive_rtr
#pragma GCC poison quanta_clk_out bus_off_indicator fail_retry_cnt timestamp_resolution_hz
#pragma GCC poison data_timing sp_permill ssp_permill on_tx_done on_error intr_priority
#pragma GCC poison backup_before_sleep allow_pd sleep_allow_pd fdf brs
#if CONFIG_IDF_TARGET_ESP32C3
#pragma GCC poison ADC_UNIT_2 ADC_ULP_MODE_FSM ADC_ULP_MODE_RISCV ADC_ULP_MODE_LP_CORE
#pragma GCC poison ADC_ATTEN_DB_0 ADC_ATTEN_DB_2_5 ADC_ATTEN_DB_6 ADC_ATTEN_DB_11
#pragma GCC poison ADC_BITWIDTH_9 ADC_BITWIDTH_10 ADC_BITWIDTH_11 ADC_BITWIDTH_13
#pragma GCC poison GPIO_MODE_INPUT_OUTPUT GPIO_MODE_OUTPUT_OD GPIO_MODE_INPUT_OUTPUT_OD
#pragma GCC poison GPIO_PULLDOWN_ENABLE GPIO_INTR_NEGEDGE GPIO_INTR_LOW_LEVEL GPIO_INTR_HIGH_LEVEL
#pragma GCC poison GPIO_MODE_DISABLE GPIO_INTR_MAX
#pragma GCC poison I2C_ADDR_BIT_LEN_10 I2C_CLK_SRC_XTAL I2C_CLK_SRC_RC_FAST trans_queue_depth
#pragma GCC poison scl_wait_us disable_ack_check
#pragma GCC poison GPTIMER_COUNT_DOWN GPTIMER_CLK_SRC_APB GPTIMER_CLK_SRC_XTAL intr_shared
#pragma GCC poison auto_reload_on_alarm reload_count
#endif
#if CONFIG_IDF_TARGET_ESP32P4
#pragma GCC poison UART_DATA_5_BITS UART_DATA_6_BITS UART_DATA_7_BITS UART_PARITY_EVEN UART_PARITY_ODD
#pragma GCC poison UART_STOP_BITS_1_5 UART_STOP_BITS_2 UART_HW_FLOWCTRL_RTS UART_HW_FLOWCTRL_CTS
#pragma GCC poison UART_HW_FLOWCTRL_CTS_RTS rx_flow_ctrl_thresh lp_source_clk
#pragma GCC poison UART_SCLK_XTAL UART_SCLK_RTC
#undef O_RDWR
#undef O_APPEND
#undef O_TRUNC
#undef SDMMC_SLOT_FLAG_WP_ACTIVE_HIGH
#undef SDMMC_SLOT_FLAG_UHS1
#pragma GCC poison O_RDWR O_APPEND O_TRUNC disk_status_check_enable use_one_fat
// The kernel mounts the card itself, so the host's function pointers are never used.
#undef SDMMC_HOST_DEFAULT
#define SDMMC_HOST_DEFAULT() {.flags = SDMMC_HOST_FLAG_4BIT, .slot = SDMMC_HOST_SLOT_1, .max_freq_khz = SDMMC_FREQ_DEFAULT}
#pragma GCC poison SDMMC_SLOT_FLAG_WP_ACTIVE_HIGH SDMMC_SLOT_FLAG_UHS1
#pragma GCC poison WIFI_MODE_NULL WIFI_MODE_STA WIFI_MODE_APSTA WIFI_MODE_NAN WIFI_IF_STA WIFI_IF_NAN
#pragma GCC poison WIFI_AUTH_WEP WIFI_AUTH_WPA_PSK WIFI_AUTH_WPA2_PSK WIFI_AUTH_WPA_WPA2_PSK
#pragma GCC poison WIFI_AUTH_ENTERPRISE WIFI_AUTH_WPA2_ENTERPRISE WIFI_AUTH_WPA3_PSK
#pragma GCC poison WIFI_AUTH_WPA2_WPA3_PSK WIFI_AUTH_WAPI_PSK WIFI_AUTH_OWE WIFI_AUTH_WPA3_ENT_192
#pragma GCC poison WIFI_AUTH_WPA3_EXT_PSK WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE WIFI_AUTH_DPP
#pragma GCC poison WIFI_AUTH_WPA3_ENTERPRISE WIFI_AUTH_WPA2_WPA3_ENTERPRISE WIFI_AUTH_WPA_ENTERPRISE
#pragma GCC poison ssid_hidden beacon_interval pairwise_cipher ftm_responder pmf_cfg sae_pwe_h2e
#pragma GCC poison csa_count dtim_period transition_disable sae_ext bss_max_idle_cfg gtk_rekey_interval
#pragma GCC poison core_id task_caps max_req_hdr_len max_uri_len ctrl_port max_open_sockets
#pragma GCC poison max_resp_headers backlog_conn lru_purge_enable recv_wait_timeout send_wait_timeout
#pragma GCC poison global_user_ctx global_user_ctx_free_fn global_transport_ctx
#pragma GCC poison global_transport_ctx_free_fn enable_so_linger linger_timeout keep_alive_enable
#pragma GCC poison keep_alive_idle keep_alive_interval keep_alive_count open_fn close_fn uri_match_fn
#pragma GCC poison user_ctx is_websocket handle_ws_control_frames supported_subprotocol
#endif
