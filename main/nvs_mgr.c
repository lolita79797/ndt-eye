#include "nvs_mgr.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "NVS_MGR";
#define NVS_NAMESPACE "ndt_eye"
#define KEY_CURRENT_FILE "cur_file"

esp_err_t nvs_mgr_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t nvs_mgr_get_current_file(char *out_filename, size_t max_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t required_size = max_len;
    err = nvs_get_str(handle, KEY_CURRENT_FILE, out_filename, &required_size);
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Đã đọc tệp hiện tại từ NVS: %s", out_filename);
    }
    return err;
}

esp_err_t nvs_mgr_set_current_file(const char *filename) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_CURRENT_FILE, filename);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "Đã lưu tệp hiện tại vào NVS: %s", filename);
    }
    nvs_close(handle);
    return err;
}
