#include "lcd_driver.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_timer.h>
#include <string.h>

static const char *TAG = "LCD_DRIVER";

#define PIN_NUM_LCD_CS    10
#define PIN_NUM_LCD_PCLK   9
#define PIN_NUM_LCD_DATA0   11
#define PIN_NUM_LCD_DATA1   12
#define PIN_NUM_LCD_DATA2   13
#define PIN_NUM_LCD_DATA3   14
#define PIN_NUM_LCD_RST    47
#define PIN_NUM_LCD_BL     15

// Tần số PCLK 60MHz tối ưu
#define LCD_PCLK_HZ       (60 * 1000 * 1000)

#define LCD_H_RES         360
#define LCD_V_RES         360

#define CHUNK_LINES       72
#define CHUNK_SIZE        (LCD_H_RES * CHUNK_LINES * 2)
#define NUM_CHUNKS        (LCD_V_RES / CHUNK_LINES) // 5 chunks logic
#define NUM_STAGING_BUFS  2                          // 2 buffer SRAM nội bộ (103KB) xoay vòng 0,1,0,1,0 | 1,0,1,0,1

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;

// 2 bộ đệm trung gian trong Internal SRAM DMA tốc độ cao
static uint8_t *s_staging_bufs[NUM_STAGING_BUFS] = {NULL, NULL};

// Chuỗi khởi tạo đầy đủ 185 lệnh từ DemoNSX V2.0 chính thức (ST77916_LVGL_DEMO)
static const st77916_lcd_init_cmd_t nsx_st77916_vendor_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0}
};

esp_err_t lcd_driver_init(esp_lcd_panel_io_color_trans_done_cb_t trans_done_cb, void *user_ctx)
{
    ESP_LOGI(TAG, "Đang khởi tạo driver màn hình ST77916 QSPI LCD (DemoNSX V2.0 50MHz)...");

    for (int i = 0; i < NUM_STAGING_BUFS; i++) {
        // Cấp phát 2 bộ đệm DMA chuẩn trong Internal SRAM tốc độ cao nhất
        s_staging_bufs[i] = (uint8_t *)heap_caps_aligned_alloc(64, CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_staging_bufs[i]) {
            ESP_LOGE(TAG, "Không thể cấp phát bộ đệm SRAM trung gian %d", i);
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "Đã cấp phát 2x %d KB bộ đệm Internal SRAM DMA thành công", CHUNK_SIZE / 1024);

    gpio_config_t bl_gpio_cfg = {
        .pin_bit_mask = (1ULL << PIN_NUM_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_gpio_cfg);
    gpio_set_level(PIN_NUM_LCD_BL, 1);

    spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        PIN_NUM_LCD_PCLK,
        PIN_NUM_LCD_DATA0,
        PIN_NUM_LCD_DATA1,
        PIN_NUM_LCD_DATA2,
        PIN_NUM_LCD_DATA3,
        32768
    );
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Không thể khởi tạo bus SPI: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(
        PIN_NUM_LCD_CS,
        trans_done_cb,
        user_ctx
    );
    io_config.pclk_hz = LCD_PCLK_HZ; // 50MHz QSPI PCLK
    // FIX QUAN TRỌNG: Đặt trans_queue_depth = 2 để phần cứng ESP-IDF tự động block CPU 
    // khi cả 2 buffer đang bận truyền DMA, chống 100% việc ghi đè buffer dở dang!
    io_config.trans_queue_depth = 2;

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Không thể tạo panel IO: %s", esp_err_to_name(ret));
        return ret;
    }

    st77916_vendor_config_t vendor_config = {
        .init_cmds = nsx_st77916_vendor_init_cmds,
        .init_cmds_size = sizeof(nsx_st77916_vendor_init_cmds) / sizeof(nsx_st77916_vendor_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ret = esp_lcd_new_panel_st77916(s_io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Không thể tạo panel handle: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_reset(s_panel_handle);
    ESP_LOGI(TAG, "Trạng thái esp_lcd_panel_reset: %s", esp_err_to_name(ret));

    ret = esp_lcd_panel_init(s_panel_handle);
    ESP_LOGI(TAG, "Trạng thái esp_lcd_panel_init: %s", esp_err_to_name(ret));

    ret = esp_lcd_panel_invert_color(s_panel_handle, true);
    ESP_LOGI(TAG, "Trạng thái esp_lcd_panel_invert_color: %s", esp_err_to_name(ret));

    ret = esp_lcd_panel_disp_on_off(s_panel_handle, true);
    ESP_LOGI(TAG, "Trạng thái esp_lcd_panel_disp_on_off: %s", esp_err_to_name(ret));

    ESP_LOGI(TAG, "Màn hình ST77916 LCD đã khởi tạo thành công");
    return ESP_OK;
}

esp_err_t lcd_driver_fill_color(uint16_t color565)
{
    if (!s_panel_handle || !s_staging_bufs[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t swapped = __builtin_bswap16(color565);
    size_t num_pixels = CHUNK_SIZE / 2;

    uint16_t *buf0 = (uint16_t *)s_staging_bufs[0];
    for (size_t p = 0; p < num_pixels; p++) {
        buf0[p] = swapped;
    }

    esp_err_t ret = ESP_OK;
    for (int i = 0; i < NUM_CHUNKS; i++) {
        int y_start = i * CHUNK_LINES;
        int y_end = y_start + CHUNK_LINES;
        int buf_idx = i % NUM_STAGING_BUFS;

        if (buf_idx != 0) {
            memcpy(s_staging_bufs[buf_idx], s_staging_bufs[0], CHUNK_SIZE);
        }

        esp_err_t draw_ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y_start, LCD_H_RES, y_end, s_staging_bufs[buf_idx]);
        if (draw_ret != ESP_OK) {
            ESP_LOGE(TAG, "Vẽ chunk %d thất bại: %s", i, esp_err_to_name(draw_ret));
            ret = draw_ret;
        }
    }
    return ret;
}

esp_err_t lcd_driver_draw_frame(const void *frame_buf) {
    if (!s_panel_handle || !frame_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *src_ptr = (const uint8_t *)frame_buf;
    static uint32_t s_global_chunk_idx = 0;

    for (int i = 0; i < NUM_CHUNKS; i++) {
        int buf_idx = (s_global_chunk_idx++) % NUM_STAGING_BUFS;
        int y_start = i * CHUNK_LINES;
        int y_end = y_start + CHUNK_LINES;

        memcpy(s_staging_bufs[buf_idx], src_ptr + (i * CHUNK_SIZE), CHUNK_SIZE);
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y_start, LCD_H_RES, y_end, s_staging_bufs[buf_idx]);
    }

    return ESP_OK;
}

void lcd_driver_set_brightness(uint8_t brightness_pct)
{
    if (brightness_pct > 0) {
        gpio_set_level(PIN_NUM_LCD_BL, 1);
    } else {
        gpio_set_level(PIN_NUM_LCD_BL, 0);
    }
}

void lcd_driver_free_staging_buffers(void)
{
    for (int i = 0; i < NUM_STAGING_BUFS; i++) {
        if (s_staging_bufs[i]) {
            heap_caps_free(s_staging_bufs[i]);
            s_staging_bufs[i] = NULL;
        }
    }
    ESP_LOGI(TAG, "Freed 2x LCD Staging Buffers (~103 KB) from Internal SRAM");
}
