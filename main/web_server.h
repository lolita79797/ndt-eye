#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init_softap(void);
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
