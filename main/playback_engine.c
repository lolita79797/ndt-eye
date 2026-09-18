#include <esp_memory_utils.h>
#include "playback_engine.h"
#include "lcd_driver.h"
#include "sdcard_mgr.h"
#include "rgbv_format.h"
#include "nvs_mgr.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include "ff.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PLAYBACK_ENGINE";

#define FRAME_BUF_SIZE (LCD_WIDTH * LCD_HEIGHT * 2) // 259,200 bytes
#define NUM_BUFFERS 6
#define READ_CHUNK_SIZE (64 * 1024)


static uint8_t *s_buffers[NUM_BUFFERS] = {NULL};
static uint8_t *s_sd_bounce_buf = NULL;

static QueueHandle_t s_free_queue = NULL;
static QueueHandle_t s_display_queue = NULL;
static SemaphoreHandle_t s_dma_done_sem = NULL;
static SemaphoreHandle_t s_engine_mutex = NULL;
static SemaphoreHandle_t s_tasks_stopped_sem = NULL;

static TaskHandle_t s_reader_task_handle = NULL;
static TaskHandle_t s_display_task_handle = NULL;

static bool s_is_playing = false;
static volatile bool s_stop_requested = false;
static char s_current_file[256] = {0};

static IRAM_ATTR bool on_lcd_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t high_task_awoken = pdFALSE;
    if (s_dma_done_sem) {
        xSemaphoreGiveFromISR(s_dma_done_sem, &high_task_awoken);
    }
    return high_task_awoken == pdTRUE;
}

static void sd_reader_task(void *pvParameters) {
    ESP_LOGI(TAG, "Task đọc SD (FatFs Direct DMA) đã khởi chạy trên Core %d", xPortGetCoreID());

    char fatfs_path[300];
    if (s_current_file[0] == '/') {
        snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", s_current_file);
    } else {
        snprintf(fatfs_path, sizeof(fatfs_path), "0:/%s", s_current_file);
    }

    FIL file;
    FRESULT res = f_open(&file, fatfs_path, FA_READ);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Không thể mở tệp FatFs '%s' (mã lỗi %d)", fatfs_path, res);
        s_is_playing = false;
        s_reader_task_handle = NULL;
        if (s_tasks_stopped_sem) xSemaphoreGive(s_tasks_stopped_sem);
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_FATFS_USE_FASTSEEK
    // Thiết lập Fast Seek link map để tối ưu seek khi loop video
    DWORD seek_map[128];
    file.cltbl = seek_map;
    seek_map[0] = 128;
    if (f_lseek(&file, CREATE_LINKMAP) == FR_OK) {
        ESP_LOGI(TAG, "Tạo bảng liên kết Fast Seek thành công cho %s", fatfs_path);
    } else {
        ESP_LOGW(TAG, "Bỏ qua hoặc thất bại khi tạo bảng liên kết Fast Seek");
        file.cltbl = NULL;
    }
#endif

    // Sử dụng SD bounce buffer 64KB đã được pre-allocate trong Internal SRAM trước khi Wi-Fi khởi tạo
    uint8_t *bounce_buf = s_sd_bounce_buf;
    if (!bounce_buf) {
        ESP_LOGE(TAG, "Bộ đệm trung gian SD Bounce buffer không khả dụng!");
        f_close(&file);
        s_is_playing = false;
        s_reader_task_handle = NULL;
        if (s_tasks_stopped_sem) xSemaphoreGive(s_tasks_stopped_sem);
        vTaskDelete(NULL);
        return;
    }

    rgbv_header_t header;
    UINT br = 0;
    res = f_read(&file, &header, sizeof(header), &br);
    if (res != FR_OK || br != sizeof(header)) {
        ESP_LOGE(TAG, "Cấu trúc header RGBV không hợp lệ trong %s", fatfs_path);
        f_close(&file);
        s_is_playing = false;
        s_reader_task_handle = NULL;
        if (s_tasks_stopped_sem) xSemaphoreGive(s_tasks_stopped_sem);
        vTaskDelete(NULL);
        return;
    }

    if (memcmp(header.magic, "RGBV", 4) != 0) {
        ESP_LOGE(TAG, "Header Magic không khớp! Nhận được: %.4s", header.magic);
        f_close(&file);
        s_is_playing = false;
        s_reader_task_handle = NULL;
        if (s_tasks_stopped_sem) xSemaphoreGive(s_tasks_stopped_sem);
        vTaskDelete(NULL);
        return;
    }

    // Di chuyển con trỏ đọc SD tới vị trí bắt đầu của khung hình thứ 0 (sau Header và Thumbnail)
    f_lseek(&file, header.header_len);

    uint8_t *buf = NULL;

    while (!s_stop_requested) {
        if (xQueueReceive(s_free_queue, &buf, portMAX_DELAY) == pdTRUE) {
            size_t bytes_needed = FRAME_BUF_SIZE;
            size_t frame_offset = 0;

            while (bytes_needed > 0 && !s_stop_requested) {
                size_t to_read = (bytes_needed > READ_CHUNK_SIZE) ? READ_CHUNK_SIZE : bytes_needed;
                UINT chunk_br = 0;
                FRESULT read_res = f_read(&file, s_sd_bounce_buf, to_read, &chunk_br);

                if (read_res != FR_OK || chunk_br == 0) {
                    // Lỗi đọc hoặc chạm cuối tệp -> lặp lại video từ vị trí frame 0
                    f_lseek(&file, header.header_len);
                    break;
                }

                memcpy(buf + frame_offset, s_sd_bounce_buf, chunk_br);
                frame_offset += chunk_br;
                bytes_needed -= chunk_br;
            }
            if (frame_offset == FRAME_BUF_SIZE) {
                if (xQueueSend(s_display_queue, &buf, pdMS_TO_TICKS(100)) != pdTRUE) {
                    xQueueSend(s_free_queue, &buf, 0);
                }
            } else {
                xQueueSend(s_free_queue, &buf, 0);
            }
        }
    }

    f_close(&file);
    ESP_LOGI(TAG, "Task đọc SD đã tự dọn dẹp và dừng thành công.");
    s_reader_task_handle = NULL;
    if (s_tasks_stopped_sem) {
        xSemaphoreGive(s_tasks_stopped_sem);
    }
    vTaskDelete(NULL);
}

static void lcd_display_task(void *pvParameters) {
    ESP_LOGI(TAG, "Task hiển thị LCD đã khởi chạy trên Core %d", xPortGetCoreID());

    uint8_t *current_buf = NULL;
    uint8_t *prev_buf = NULL;

    uint32_t frame_count = 0;
    int64_t last_fps_time = esp_timer_get_time();

    while (!s_stop_requested) {
        if (xQueueReceive(s_display_queue, &current_buf, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (current_buf != NULL) {
                xSemaphoreTake(s_dma_done_sem, 0);
                if (lcd_driver_draw_frame(current_buf) == ESP_OK) {
                    xSemaphoreTake(s_dma_done_sem, pdMS_TO_TICKS(50));
                }

                if (prev_buf) {
                    xQueueSend(s_free_queue, &prev_buf, 0);
                }
                prev_buf = current_buf;
                frame_count++;
            }

            int64_t now = esp_timer_get_time();
            if (now - last_fps_time >= 30000000) { // Giám sát FPS định kỳ mỗi 30 giây
                float elapsed_sec = (float)(now - last_fps_time) / 1000000.0f;
                float actual_fps = (float)frame_count / elapsed_sec;
                float read_speed_mbps = ((float)frame_count * (float)FRAME_BUF_SIZE) / (elapsed_sec * 1024.0f * 1024.0f);
                ESP_LOGI(TAG, "[FPS Monitor] File: '%s' | Read Speed: %.2f MB/s | Actual FPS: %.1f", s_current_file, read_speed_mbps, actual_fps);
                frame_count = 0;
                last_fps_time = now;
            }
        }
    }

    if (prev_buf) {
        xQueueSend(s_free_queue, &prev_buf, 0);
    }

    ESP_LOGI(TAG, "Task hiển thị LCD đã tự dọn dẹp và dừng thành công.");
    s_display_task_handle = NULL;
    if (s_tasks_stopped_sem) {
        xSemaphoreGive(s_tasks_stopped_sem);
    }
    vTaskDelete(NULL);
}

esp_err_t playback_engine_init(void) {
    s_engine_mutex = xSemaphoreCreateMutex();
    s_dma_done_sem = xSemaphoreCreateBinary();
    s_tasks_stopped_sem = xSemaphoreCreateCounting(2, 0);

    // Cấp phát s_sd_bounce_buf trong Internal SRAM TRƯỚC để luôn lấy được block 64KB liên tục
    if (!s_sd_bounce_buf) {
        s_sd_bounce_buf = (uint8_t *)heap_caps_aligned_alloc(32, READ_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_sd_bounce_buf) {
            ESP_LOGE(TAG, "Thất bại khi cấp phát bộ đệm SD DMA 64KB trong SRAM nội bộ!");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Cấp phát bộ đệm SD DMA 64KB trong SRAM nội bộ thành công");
    }

    ESP_ERROR_CHECK(lcd_driver_init(on_lcd_trans_done, NULL));

    s_free_queue = xQueueCreate(NUM_BUFFERS, sizeof(uint8_t *));
    s_display_queue = xQueueCreate(NUM_BUFFERS, sizeof(uint8_t *));

    for (int i = 0; i < NUM_BUFFERS; i++) {
        s_buffers[i] = heap_caps_aligned_alloc(64, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_buffers[i]) {
            ESP_LOGE(TAG, "Thất bại khi cấp phát bộ đệm khung hình PSRAM %d", i);
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(s_free_queue, &s_buffers[i], 0);
    }

    ESP_LOGI(TAG, "Engine phát video đã khởi tạo với %d bộ đệm PSRAM (tổng %d KB)", NUM_BUFFERS, (FRAME_BUF_SIZE * NUM_BUFFERS) / 1024);
    return ESP_OK;
}


void playback_engine_stop(void) {
    xSemaphoreTake(s_engine_mutex, portMAX_DELAY);
    if (s_is_playing || s_reader_task_handle != NULL || s_display_task_handle != NULL) {
        s_stop_requested = true;
        s_is_playing = false;

        int tasks_to_wait = 0;
        if (s_reader_task_handle != NULL) tasks_to_wait++;
        if (s_display_task_handle != NULL) tasks_to_wait++;

        if (tasks_to_wait > 0) {
            if (s_tasks_stopped_sem) {
                while (xSemaphoreTake(s_tasks_stopped_sem, 0) == pdTRUE);
            }

            // Đánh thức các task đang ngủ chờ hàng đợi/semaphore
            if (s_free_queue) {
                uint8_t *dummy = NULL;
                xQueueSend(s_free_queue, &dummy, 0);
            }
            if (s_display_queue) {
                uint8_t *dummy = NULL;
                xQueueSend(s_display_queue, &dummy, 0);
            }
            if (s_dma_done_sem) {
                xSemaphoreGive(s_dma_done_sem);
            }

            for (int i = 0; i < tasks_to_wait; i++) {
                if (s_tasks_stopped_sem) {
                    if (xSemaphoreTake(s_tasks_stopped_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
                        ESP_LOGI(TAG, "Đã nhận xác nhận tự dừng từ Task (%d/%d)", i + 1, tasks_to_wait);
                    } else {
                        ESP_LOGW(TAG, "Cảnh báo: Task chưa xác nhận tự dừng sau 2s timeout (%d/%d)", i + 1, tasks_to_wait);
                    }
                }
            }
        }

        s_reader_task_handle = NULL;
        s_display_task_handle = NULL;

        uint8_t *dummy = NULL;
        while (s_display_queue && xQueueReceive(s_display_queue, &dummy, 0) == pdTRUE) {
            xQueueSend(s_free_queue, &dummy, 0);
        }

        ESP_LOGI(TAG, "Đã dừng phát video an toàn.");
    }
    xSemaphoreGive(s_engine_mutex);
}

esp_err_t playback_engine_play_file(const char *filename) {
    xSemaphoreTake(s_engine_mutex, portMAX_DELAY);

    if (s_is_playing || s_reader_task_handle != NULL || s_display_task_handle != NULL) {
        s_stop_requested = true;
        s_is_playing = false;

        int tasks_to_wait = 0;
        if (s_reader_task_handle != NULL) tasks_to_wait++;
        if (s_display_task_handle != NULL) tasks_to_wait++;

        if (tasks_to_wait > 0) {
            if (s_tasks_stopped_sem) {
                while (xSemaphoreTake(s_tasks_stopped_sem, 0) == pdTRUE);
            }

            if (s_free_queue) {
                uint8_t *dummy = NULL;
                xQueueSend(s_free_queue, &dummy, 0);
            }
            if (s_display_queue) {
                uint8_t *dummy = NULL;
                xQueueSend(s_display_queue, &dummy, 0);
            }
            if (s_dma_done_sem) {
                xSemaphoreGive(s_dma_done_sem);
            }

            for (int i = 0; i < tasks_to_wait; i++) {
                if (s_tasks_stopped_sem) {
                    xSemaphoreTake(s_tasks_stopped_sem, pdMS_TO_TICKS(2000));
                }
            }
        }

        s_reader_task_handle = NULL;
        s_display_task_handle = NULL;

        uint8_t *dummy = NULL;
        while (s_display_queue && xQueueReceive(s_display_queue, &dummy, 0) == pdTRUE) {
            xQueueSend(s_free_queue, &dummy, 0);
        }
    }

    strncpy(s_current_file, filename, sizeof(s_current_file) - 1);
    nvs_mgr_set_current_file(filename);

    s_stop_requested = false;
    s_is_playing = true;

    BaseType_t r1 = xTaskCreatePinnedToCore(sd_reader_task, "sd_reader", 8192, NULL, 5, &s_reader_task_handle, 0);
    BaseType_t r2 = xTaskCreatePinnedToCore(lcd_display_task, "lcd_display", 4096, NULL, 6, &s_display_task_handle, 1);

    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "Thất bại khi tạo Task phát video (sd_reader: %d, lcd_display: %d)", r1, r2);
        s_is_playing = false;
        s_reader_task_handle = NULL;
        s_display_task_handle = NULL;
        xSemaphoreGive(s_engine_mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_engine_mutex);
    return ESP_OK;
}

bool playback_engine_is_playing(void) {
    return s_is_playing;
}

const char* playback_engine_get_current_file(void) {
    return s_current_file;
}

void playback_engine_stop_and_free(void) {
    xSemaphoreTake(s_engine_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "Đang dừng engine phát video và giải phóng TOÀN BỘ tài nguyên cho chế độ Web Server Wi-Fi...");

    s_stop_requested = true;
    s_is_playing = false;

    int tasks_to_wait = 0;
    if (s_reader_task_handle != NULL) tasks_to_wait++;
    if (s_display_task_handle != NULL) tasks_to_wait++;

    if (tasks_to_wait > 0) {
        if (s_tasks_stopped_sem) {
            while (xSemaphoreTake(s_tasks_stopped_sem, 0) == pdTRUE);
        }

        // Đánh thức các task đang ngủ chờ hàng đợi/semaphore
        if (s_free_queue) {
            uint8_t *dummy = NULL;
            xQueueSend(s_free_queue, &dummy, 0);
        }
        if (s_display_queue) {
            uint8_t *dummy = NULL;
            xQueueSend(s_display_queue, &dummy, 0);
        }
        if (s_dma_done_sem) {
            xSemaphoreGive(s_dma_done_sem);
        }

        // Chờ từng task tự giải phóng và gửi thông báo hoàn tất
        for (int i = 0; i < tasks_to_wait; i++) {
            if (s_tasks_stopped_sem) {
                if (xSemaphoreTake(s_tasks_stopped_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    ESP_LOGI(TAG, "Đã nhận xác nhận tự dừng từ Task (%d/%d)", i + 1, tasks_to_wait);
                } else {
                    ESP_LOGW(TAG, "Cảnh báo: Task chưa xác nhận tự dừng sau 2s timeout (%d/%d)", i + 1, tasks_to_wait);
                }
            }
        }
    }

    s_reader_task_handle = NULL;
    s_display_task_handle = NULL;

    if (s_sd_bounce_buf) {
        heap_caps_free(s_sd_bounce_buf);
        s_sd_bounce_buf = NULL;
        ESP_LOGI(TAG, "Đã giải phóng bộ đệm SD DMA 64KB từ SRAM nội bộ.");
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (s_buffers[i]) {
            heap_caps_free(s_buffers[i]);
            s_buffers[i] = NULL;
        }
    }
    ESP_LOGI(TAG, "Đã giải phóng %d bộ đệm khung hình PSRAM.", NUM_BUFFERS);

    if (s_free_queue) {
        vQueueDelete(s_free_queue);
        s_free_queue = NULL;
    }

    if (s_display_queue) {
        vQueueDelete(s_display_queue);
        s_display_queue = NULL;
    }

    lcd_driver_free_staging_buffers();

    ESP_LOGI(TAG, "TOÀN BỘ tài nguyên RAM phát video & LCD đã được giải phóng thành công!");
    xSemaphoreGive(s_engine_mutex);
}

