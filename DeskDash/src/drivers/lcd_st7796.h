// ST7796 SPI panel driver for the Waveshare ESP32-C5-Touch-LCD-3.5 (320 x 480).
// Init sequence and pins come from Waveshare's 07_lvgl_demo Arduino example.
#pragma once
#include <Arduino.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 480

bool lcd_begin();
// Sends a block of RGB565 pixels and waits for the DMA transfer to finish.
bool lcd_send_area(int x1, int y1, int x2, int y2, const void *pixels, size_t bytes);
