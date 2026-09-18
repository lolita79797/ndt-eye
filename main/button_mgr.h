#ifndef BUTTON_MGR_H
#define BUTTON_MGR_H

#include <esp_err.h>
#include <stdbool.h>

typedef void (*button_press_cb_t)(bool is_long_press);

/**
 * @brief Initialize GPIO 0 BOOT button monitoring task.
 * 
 * @param on_press_cb Callback function to be executed when BOOT button is pressed.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t button_mgr_init(button_press_cb_t on_press_cb);

#endif // BUTTON_MGR_H
