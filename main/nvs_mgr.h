#ifndef NVS_MGR_H
#define NVS_MGR_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_mgr_init(void);
esp_err_t nvs_mgr_get_current_file(char *out_filename, size_t max_len);
esp_err_t nvs_mgr_set_current_file(const char *filename);

#ifdef __cplusplus
}
#endif

#endif // NVS_MGR_H
