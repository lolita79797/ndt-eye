idf_component_register(
    SRCS "main.c" "nvs_mgr.c" "sdcard_mgr.c" "lcd_driver.c" "playback_engine.c" "web_server.c" "button_mgr.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash fatfs driver esp_lcd esp_wifi esp_event esp_http_server esp_timer
)

