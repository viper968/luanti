// Pin map and helpers for the Waveshare ESP32-S3-Touch-LCD-2.8B.
// Taken from the board schematic and Waveshare's Arduino demo drivers.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// Shared I2C bus: TCA9554 expander, PCF85063 RTC, QMI8658 IMU, touch
#define BOARD_I2C_SCL       7
#define BOARD_I2C_SDA       15
#define BOARD_I2C_HZ        400000   // TCA9554/PCF85063 are rated for 400 kHz

#define BOARD_TCA9554_ADDR  0x20
#define BOARD_PCF85063_ADDR 0x51

// TCA9554 port bits (Waveshare's EXIO_PIN1..8 map to bits 0..7)
#define EXIO_LCD_RST  (1u << 0)
#define EXIO_TP_RST   (1u << 1)
#define EXIO_LCD_CS   (1u << 2)
#define EXIO_SD_D3    (1u << 3)
#define EXIO_IMU_INT1 (1u << 4)
#define EXIO_IMU_INT2 (1u << 5)
#define EXIO_RTC_INT  (1u << 6)
#define EXIO_BUZZER   (1u << 7)

// TF card, 1-bit SDMMC through the GPIO matrix (D3 is driven by the expander)
#define BOARD_SD_CLK  2    // shared with LCD_SCK (panel config bus)
#define BOARD_SD_CMD  1    // shared with LCD_SDA
#define BOARD_SD_D0   42

#define BOARD_BL_PWM  6    // LCD backlight
#define BOARD_BAT_ADC 4    // battery voltage through a 1/3 divider

extern i2c_master_bus_handle_t board_i2c;

esp_err_t board_i2c_init(void);
void board_i2c_scan(void);

// Configures the expander for headless use: LCD and touch held in reset,
// LCD_CS high (panel ignores GPIO1/2), SD D3 high (card starts in SD mode),
// buzzer off, interrupt lines as inputs.
esp_err_t board_expander_init(void);
esp_err_t board_expander_set(uint8_t mask, bool level);

void board_backlight_off(void);
void board_beep(int ms);

void board_rtc_print(void);
float board_battery_volts(void);
