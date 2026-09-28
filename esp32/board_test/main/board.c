#include "board.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

#define TCA9554_REG_INPUT  0x00
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03

i2c_master_bus_handle_t board_i2c;
static i2c_master_dev_handle_t s_tca;
static i2c_master_dev_handle_t s_rtc;
static uint8_t s_exio_out;

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len)
{
	return i2c_master_transmit_receive(dev, &reg, 1, out, len, 100);
}

static esp_err_t add_dev(uint8_t addr, i2c_master_dev_handle_t *out)
{
	i2c_device_config_t cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = addr,
		.scl_speed_hz = BOARD_I2C_HZ,
	};
	return i2c_master_bus_add_device(board_i2c, &cfg, out);
}

esp_err_t board_i2c_init(void)
{
	i2c_master_bus_config_t cfg = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = BOARD_I2C_SDA,
		.scl_io_num = BOARD_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &board_i2c), TAG, "i2c bus");
	ESP_RETURN_ON_ERROR(add_dev(BOARD_TCA9554_ADDR, &s_tca), TAG, "tca9554");
	ESP_RETURN_ON_ERROR(add_dev(BOARD_PCF85063_ADDR, &s_rtc), TAG, "pcf85063");
	return ESP_OK;
}

void board_i2c_scan(void)
{
	printf("I2C devices:");
	int found = 0;
	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		if (i2c_master_probe(board_i2c, addr, 20) == ESP_OK) {
			printf(" 0x%02x", addr);
			found++;
		}
	}
	printf("%s\n", found ? "" : " none");
	printf("  expected: 0x20 TCA9554, 0x51 PCF85063, 0x6a or 0x6b QMI8658 "
		"(touch is held in reset, so it won't show)\n");
}

esp_err_t board_expander_init(void)
{
	// Set the output latch before switching pins to outputs, so nothing glitches
	s_exio_out = EXIO_LCD_CS | EXIO_SD_D3;
	ESP_RETURN_ON_ERROR(reg_write(s_tca, TCA9554_REG_OUTPUT, s_exio_out), TAG, "exio out");
	uint8_t inputs = EXIO_IMU_INT1 | EXIO_IMU_INT2 | EXIO_RTC_INT;
	ESP_RETURN_ON_ERROR(reg_write(s_tca, TCA9554_REG_CONFIG, inputs), TAG, "exio cfg");
	vTaskDelay(pdMS_TO_TICKS(10));
	return ESP_OK;
}

esp_err_t board_expander_set(uint8_t mask, bool level)
{
	s_exio_out = level ? (s_exio_out | mask) : (s_exio_out & ~mask);
	return reg_write(s_tca, TCA9554_REG_OUTPUT, s_exio_out);
}

void board_backlight_off(void)
{
	gpio_reset_pin(BOARD_BL_PWM);
	gpio_set_direction(BOARD_BL_PWM, GPIO_MODE_OUTPUT);
	gpio_set_level(BOARD_BL_PWM, 0);
}

void board_beep(int ms)
{
	board_expander_set(EXIO_BUZZER, true);
	vTaskDelay(pdMS_TO_TICKS(ms));
	board_expander_set(EXIO_BUZZER, false);
}

static int bcd(uint8_t v)
{
	return (v >> 4) * 10 + (v & 0x0f);
}

void board_rtc_print(void)
{
	uint8_t r[7];
	if (reg_read(s_rtc, 0x04, r, sizeof(r)) != ESP_OK) {
		printf("RTC: read failed\n");
		return;
	}
	// Waveshare's driver stores the year as an offset from 1970
	printf("RTC: %d-%02d-%02d %02d:%02d:%02d%s\n",
		1970 + bcd(r[6]), bcd(r[5] & 0x1f), bcd(r[3] & 0x3f),
		bcd(r[2] & 0x3f), bcd(r[1] & 0x7f), bcd(r[0] & 0x7f),
		(r[0] & 0x80) ? "  (oscillator stopped: time not valid)" : "");
}

float board_battery_volts(void)
{
	// GPIO4 is ADC1 channel 3 on the ESP32-S3
	adc_oneshot_unit_handle_t adc;
	adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
	if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK)
		return -1;
	adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
	adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &ccfg);

	adc_cali_handle_t cali = NULL;
	adc_cali_curve_fitting_config_t kcfg = {
		.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3,
		.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	adc_cali_create_scheme_curve_fitting(&kcfg, &cali);

	int raw = 0, mv = 0;
	adc_oneshot_read(adc, ADC_CHANNEL_3, &raw);
	if (cali)
		adc_cali_raw_to_voltage(cali, raw, &mv);
	else
		mv = raw * 3100 / 4095;

	if (cali)
		adc_cali_delete_scheme_curve_fitting(cali);
	adc_oneshot_del_unit(adc);
	// Same scaling as Waveshare's BAT_Driver: 1/3 divider plus a trim factor
	return mv * 3.0f / 1000.0f / 0.992857f;
}
