#ifndef SDCARD_MGR_H
#define SDCARD_MGR_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDCARD_MOUNT_POINT "/sdcard"

esp_err_t sdcard_mgr_init(void);
void sdcard_mgr_benchmark(void);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_MGR_H
