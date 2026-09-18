#include "sdcard_mgr.h"
#include "ff.h"
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "SDCARD_MGR";

// SDMMC 4-bit Pinout (Matching NSX Hardware)
#define PIN_NUM_D0 2
#define PIN_NUM_D1 1
#define PIN_NUM_D2 6
#define PIN_NUM_D3 5
#define PIN_NUM_CLK 3
#define PIN_NUM_CMD 4

static sdmmc_card_t *s_card = NULL;

esp_err_t sdcard_mgr_init(void) {
    ESP_LOGI(TAG, "Đang khởi tạo ngoại vi SDMMC 4-bit (Cố định 40MHz High-Speed)...");

    // Explicitly enable internal pull-ups on GPIO matrix pins for ESP32-S3
    gpio_pullup_en(PIN_NUM_CMD);
    gpio_pullup_en(PIN_NUM_D0);
    gpio_pullup_en(PIN_NUM_D1);
    gpio_pullup_en(PIN_NUM_D2);
    gpio_pullup_en(PIN_NUM_D3);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024, // 64KB (128 sectors * 512B) tối ưu cluster FATFS chuẩn AU & sector 512B
        .disk_status_check_enable = false
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; // Fixed 40MHz High Speed

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4; // Fixed 4-bit mode
    slot_config.clk = PIN_NUM_CLK;
    slot_config.cmd = PIN_NUM_CMD;
    slot_config.d0 = PIN_NUM_D0;
    slot_config.d1 = PIN_NUM_D1;
    slot_config.d2 = PIN_NUM_D2;
    slot_config.d3 = PIN_NUM_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Thất bại khi gắn (mount) VFS thẻ nhớ SD (0x%x: %s)", ret, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Gắn thẻ nhớ SD thành công tại %s (Cố định 4-bit @ 40MHz)", SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sdcard_mgr_benchmark(void) {}
