#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_WIDTH   360
#define LCD_HEIGHT  360

esp_err_t lcd_driver_init(esp_lcd_panel_io_color_trans_done_cb_t trans_done_cb, void *user_ctx);
esp_err_t lcd_driver_draw_frame(const void *frame_buf);
esp_err_t lcd_driver_fill_color(uint16_t color565);
void lcd_driver_run_sanity_test(void);
void lcd_driver_set_brightness(uint8_t brightness_pct);
void lcd_driver_free_staging_buffers(void);

#ifdef __cplusplus

}
#endif

#endif // LCD_DRIVER_H
