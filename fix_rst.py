import re

path = r'D:\Espressif\esp-idf-5.5.2\Repo\ndt-eye\main\lcd_driver.c'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Replace PIN_NUM_LCD_RST 47 with -1
content = re.sub(r'#define\s+PIN_NUM_LCD_RST\s+47', '#define PIN_NUM_LCD_RST      -1', content)

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)

print("Updated PIN_NUM_LCD_RST to -1 in lcd_driver.c")
