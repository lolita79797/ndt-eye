import re

path_repo = r'D:\Espressif\esp-idf-5.5.2\Repo\ndt-eye\main\lcd_driver.c'
path_scratch = r'C:\Users\acer\.gemini\antigravity-ide\scratch\ndt-eye-firmware\main\lcd_driver.c'

for p in [path_repo, path_scratch]:
    with open(p, 'r', encoding='utf-8') as f:
        content = f.read()

    # Comment out esp_lcd_panel_reset
    content = content.replace(
        'ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));',
        '// ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));'
    )

    with open(p, 'w', encoding='utf-8') as f:
        f.write(content)

print("Commented out esp_lcd_panel_reset in lcd_driver.c successfully.")
