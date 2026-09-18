#include <stdio.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include "nvs_mgr.h"
#include "sdcard_mgr.h"
#include "lcd_driver.h"
#include "playback_engine.h"
#include "web_server.h"
#include "button_mgr.h"

static const char *TAG = "MAIN";

static void log_ram_milestone(int step, const char *label) {
    (void)step;
    (void)label;
}

static void on_boot_button_pressed(bool is_long_press) {
    ESP_LOGI(TAG, "==================================================");
    if (is_long_press) {
        ESP_LOGI(TAG, "=== ĐÃ GIỮ NÚT BOOT 3S -> KÍCH HOẠT CHẾ ĐỘ NẠP QUA USB TYPE-C & WI-FI ===");
    } else {
        ESP_LOGI(TAG, "=== ĐÃ NHẤN NÚT BOOT -> KÍCH HOẠT CHẾ ĐỘ WI-FI WEB SERVER ===");
    }
    ESP_LOGI(TAG, "==================================================");

    log_ram_milestone(10, "Before Stopping Playback Engine");
    playback_engine_stop_and_free();
    log_ram_milestone(11, "After Stopping Playback & Freeing RAM");

    ESP_LOGI(TAG, "Đang khởi chạy Wi-Fi SoftAP & HTTP Web Server...");
    wifi_init_softap();
    web_server_start();
    log_ram_milestone(12, "After Wi-Fi & HTTP Web Server Started");
    ESP_LOGI(TAG, "Wi-Fi Web Server đang hoạt động. Kết nối SSID: NDT-EYE (Mật khẩu: prodndt2310) -> http://192.168.4.1");

    if (is_long_press) {
        ESP_LOGI(TAG, "👉 [CỔNG USB TYPE-C] Cắm cáp kết nối GPIO 19 (D-) & GPIO 20 (D+) vào PC để nạp dữ liệu.");
    }
}

void app_main(void) {
    // Tắt bớt log rác hệ thống, nhưng giữ log INFO cho PLAYBACK_ENGINE và MAIN
    esp_log_level_set("*", ESP_LOG_INFO);

    // 1. Initialize NVS Manager
    esp_err_t ret = nvs_mgr_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo NVS thất bại: %s", esp_err_to_name(ret));
    }
    log_ram_milestone(2, "Post NVS Init");

    // 2. Initialize SD Card Manager
    ret = sdcard_mgr_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo thẻ nhớ SD thất bại: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD Card mounted successfully at %s", SDCARD_MOUNT_POINT);
    }
    log_ram_milestone(3, "Post SD Mount");

    // 3. Initialize Playback Engine (also initializes LCD driver)
    ret = playback_engine_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo Engine phát video thất bại: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Engine phát video đã khởi tạo thành công");
    log_ram_milestone(4, "Post Playback Engine & LCD Init (Wi-Fi OFF)");

    // 4. Initialize BOOT Button Manager (GPIO 0)
    ret = button_mgr_init(on_boot_button_pressed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo Trình quản lý nút BOOT thất bại: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Trình quản lý nút BOOT đã khởi tạo. Nhấn nút BOOT (GPIO0) bất kỳ lúc nào để bật Wi-Fi.");
    }

    // 5. Check for last played file or auto-play first .rgbv file from SD card
    char play_target[128] = {0};
    nvs_mgr_get_current_file(play_target, sizeof(play_target));

    if (play_target[0] != '\0') {
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s/%s", SDCARD_MOUNT_POINT, play_target);
        struct stat st;
        if (stat(full_path, &st) == 0) {
            ESP_LOGI(TAG, "Tự động phát tệp lần trước từ NVS: %s", play_target);
            playback_engine_play_file(play_target);
        } else {
            play_target[0] = '\0';
        }
    }

    if (play_target[0] == '\0') {
        DIR *dir = opendir(SDCARD_MOUNT_POINT);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, ".rgbv") || strstr(entry->d_name, ".RGBV")) {
                    strncpy(play_target, entry->d_name, sizeof(play_target) - 1);
                    ESP_LOGI(TAG, "Tự động phát tệp .rgbv đầu tiên tìm thấy trên thẻ SD: %s", play_target);
                    playback_engine_play_file(play_target);
                    break;
                }
            }
            closedir(dir);
        }
    }

    if (play_target[0] == '\0') {
        ESP_LOGW(TAG, "Không tìm thấy tệp video .rgbv nào trên thẻ nhớ SD. Nhấn nút BOOT (GPIO0) để bật Wi-Fi & Web UI tải tệp lên.");
    }
    log_ram_milestone(5, "Post Auto-play Triggered (Wi-Fi OFF)");
}


