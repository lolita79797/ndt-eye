#include "button_mgr.h"
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "BUTTON_MGR";
#define BOOT_BUTTON_GPIO GPIO_NUM_0

static button_press_cb_t s_on_press_cb = NULL;
static TaskHandle_t s_button_task_handle = NULL;

static void IRAM_ATTR gpio_boot_button_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_button_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_button_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static void button_task(void *pvParameters) {
    ESP_LOGI(TAG, "Task giám sát nút BOOT (GPIO 0) đã khởi chạy (Sử dụng ngắt Interrupt, Ưu tiên 7)");

    while (1) {
        // Wait for interrupt notification
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count > 0) {
            // Debounce delay
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                // Đo thời gian giữ nút bấm
                int press_ms = 0;
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    press_ms += 100;
                    if (press_ms >= 3000) {
                        break;
                    }
                }

                if (press_ms >= 3000) {
                    ESP_LOGI(TAG, "=== ĐÃ NHẤN GIỮ NÚT BOOT (3 GIÂY) -> CHẾ ĐỘ NẠP QUA USB TYPE-C ===");
                    if (s_on_press_cb) {
                        s_on_press_cb(true); // true = Long press 3s USB MSC Mode
                    }
                } else {
                    ESP_LOGI(TAG, "=== ĐÃ NHẤN NHẢ NÚT BOOT -> CHẾ ĐỘ WI-FI WEB SERVER ===");
                    if (s_on_press_cb) {
                        s_on_press_cb(false); // false = Normal press Wi-Fi Mode
                    }
                }

                gpio_isr_handler_remove(BOOT_BUTTON_GPIO);
                ESP_LOGI(TAG, "Task nút bấm tự hủy sau khi xử lý sự kiện.");
                s_button_task_handle = NULL;
                vTaskDelete(NULL);
                return;
            }
        }
    }
}

esp_err_t button_mgr_init(button_press_cb_t on_press_cb) {
    s_on_press_cb = on_press_cb;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Không thể cấu hình chân GPIO 0: %s", esp_err_to_name(ret));
        return ret;
    }

    // High priority (7) ensures immediate execution when GPIO interrupt fires, higher than sd_reader_task (5)
    BaseType_t task_ret = xTaskCreatePinnedToCore(button_task, "button_task", 3072, NULL, 7, &s_button_task_handle, 0);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Không thể tạo task giám sát nút bấm");
        return ESP_FAIL;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Không thể cài đặt dịch vụ ISR GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add(BOOT_BUTTON_GPIO, gpio_boot_button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Không thể thêm ISR handler cho GPIO 0: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

