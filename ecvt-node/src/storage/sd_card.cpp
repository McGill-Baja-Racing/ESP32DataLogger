#include "app/runtime_internal.h"
#include "storage/sd_card.h"

bool mount_sd_card(void)
{
    /* --------- SD mount --------- */
    ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = NULL;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

#if SOC_SDMMC_IO_POWER_EXTERNAL
    // Power SD IO via on-chip LDO channel 4
    sd_pwr_ctrl_ldo_config_t cfg = {};
    cfg.ldo_chan_id = 4;

    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&cfg, &pwr);
    if (pwr_err == ESP_OK) {
        host.pwr_ctrl_handle = pwr;
        ESP_LOGI(TAG, "SD IO power control enabled (on-chip LDO chan %d)", cfg.ldo_chan_id);
    } else {
        ESP_LOGW(TAG, "SD IO power control NOT enabled: %s", esp_err_to_name(pwr_err));
    }
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    /* P4 SDMMC pins */
    slot_config.clk = (gpio_num_t)43;
    slot_config.cmd = (gpio_num_t)44;
    slot_config.d0  = (gpio_num_t)39;
    slot_config.d1  = (gpio_num_t)40;
    slot_config.d2  = (gpio_num_t)41;
    slot_config.d3  = (gpio_num_t)42;

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;

    vTaskDelay(pdMS_TO_TICKS(30));

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return false;
    }
    sdmmc_card_print_info(stdout, card);

    return true;
}
