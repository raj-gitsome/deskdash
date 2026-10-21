#include "lcd_st7796.h"
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_io_interface.h>

#define LCD_MISO 2
#define LCD_MOSI 7
#define LCD_SCLK 6
#define LCD_CS   8
#define LCD_DC   5

static esp_lcd_panel_io_handle_t lcd_io = nullptr;
static volatile bool lcd_transfer_done = false;

static bool lcd_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  lcd_transfer_done = true;
  return false;
}

static bool lcd_command(uint8_t command, const uint8_t *data = nullptr, size_t length = 0) {
  return esp_lcd_panel_io_tx_param(lcd_io, command, data, length) == ESP_OK;
}

bool lcd_begin() {
  spi_bus_config_t bus_config = {};
  bus_config.sclk_io_num = LCD_SCLK;
  bus_config.mosi_io_num = LCD_MOSI;
  bus_config.miso_io_num = LCD_MISO;
  bus_config.quadwp_io_num = -1;
  bus_config.quadhd_io_num = -1;
  bus_config.max_transfer_sz = LCD_WIDTH * 40 * 2;

  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("[lcd] SPI bus init failed: %s\n", esp_err_to_name(err));
    return false;
  }

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.dc_gpio_num = LCD_DC;
  io_config.cs_gpio_num = LCD_CS;
  io_config.pclk_hz = 60000000;
  io_config.spi_mode = 0;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done = lcd_color_done;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &lcd_io);
  if (err != ESP_OK) {
    Serial.printf("[lcd] panel IO init failed: %s\n", esp_err_to_name(err));
    return false;
  }

  lcd_command(0x01);  // software reset
  delay(20);
  lcd_command(0x11);  // sleep out
  delay(100);
  const uint8_t madctl = 0x48;  // BGR + mirror X
  const uint8_t colmod = 0x55;  // RGB565
  lcd_command(0x36, &madctl, 1);
  lcd_command(0x3A, &colmod, 1);

  const uint8_t d_f0_c3[] = {0xC3}; lcd_command(0xF0, d_f0_c3, sizeof(d_f0_c3));
  const uint8_t d_f0_96[] = {0x96}; lcd_command(0xF0, d_f0_96, sizeof(d_f0_96));
  const uint8_t d_b4[] = {0x01}; lcd_command(0xB4, d_b4, sizeof(d_b4));
  const uint8_t d_b7[] = {0xC6}; lcd_command(0xB7, d_b7, sizeof(d_b7));
  const uint8_t d_e8[] = {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}; lcd_command(0xE8, d_e8, sizeof(d_e8));
  const uint8_t d_c1[] = {0x06}; lcd_command(0xC1, d_c1, sizeof(d_c1));
  const uint8_t d_c2[] = {0xA7}; lcd_command(0xC2, d_c2, sizeof(d_c2));
  const uint8_t d_c5[] = {0x18}; lcd_command(0xC5, d_c5, sizeof(d_c5));
  const uint8_t d_e0[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}; lcd_command(0xE0, d_e0, sizeof(d_e0));
  const uint8_t d_e1[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2D, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}; lcd_command(0xE1, d_e1, sizeof(d_e1));
  const uint8_t d_f0_3c[] = {0x3C}; lcd_command(0xF0, d_f0_3c, sizeof(d_f0_3c));
  const uint8_t d_f0_69[] = {0x69}; lcd_command(0xF0, d_f0_69, sizeof(d_f0_69));
  lcd_command(0x21);  // inversion on
  lcd_command(0x29);  // display on
  delay(120);
  return true;
}

bool lcd_send_area(int x1, int y1, int x2, int y2, const void *pixels, size_t bytes) {
  uint8_t column[] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  uint8_t row[] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
  if (!lcd_command(0x2A, column, sizeof(column)) || !lcd_command(0x2B, row, sizeof(row))) return false;
  lcd_transfer_done = false;
  if (esp_lcd_panel_io_tx_color(lcd_io, 0x2C, pixels, bytes) != ESP_OK) return false;
  uint32_t started = millis();
  while (!lcd_transfer_done && millis() - started < 1000) delay(1);
  return lcd_transfer_done;
}
