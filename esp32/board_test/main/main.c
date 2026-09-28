// Bring-up test for running luantiserver on the Waveshare
// ESP32-S3-Touch-LCD-2.8B. It measures the numbers the port depends on:
// free memory, PSRAM bandwidth, float vs double speed (Lua uses doubles),
// TF card throughput and Wi-Fi UDP round trips.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "board.h"
#include "sd_test.h"
#include "net_test.h"
#include "status_web.h"

static void print_memory(const char *when)
{
	printf("Memory (%s): internal free %u KB (largest block %u KB), "
		"PSRAM free %u KB (largest block %u KB)\n", when,
		(unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
		(unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
		(unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
		(unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

static void chip_info(void)
{
	esp_chip_info_t info;
	esp_chip_info(&info);
	uint32_t flash = 0;
	esp_flash_get_size(NULL, &flash);
	printf("Chip: ESP32-S3 rev %d.%d, %d cores, flash %lu MB, PSRAM %u MB\n",
		info.revision / 100, info.revision % 100, info.cores,
		(unsigned long)(flash >> 20), (unsigned)(esp_psram_get_size() >> 20));
}

// memcpy bandwidth: MapBlock (de)serialization and zstd are mostly this
static void memcpy_bench(const char *name, uint32_t caps, size_t sz)
{
	uint8_t *a = heap_caps_malloc(sz, caps);
	uint8_t *b = heap_caps_malloc(sz, caps);
	if (!a || !b) {
		printf("memcpy %s: alloc failed\n", name);
		free(a);
		free(b);
		return;
	}
	memset(a, 0x5a, sz);
	const int reps = (4 * 1024 * 1024) / sz; // copy 4 MB in total
	int64_t t0 = esp_timer_get_time();
	for (int i = 0; i < reps; i++)
		memcpy(b, a, sz);
	int64_t us = esp_timer_get_time() - t0;
	printf("memcpy %-8s %.1f MB/s\n", name, (double)sz * reps / (1024.0 * 1024.0) / (us / 1e6));
	free(a);
	free(b);
}

// Lua 5.1 numbers are doubles; the S3 FPU only does single precision.
// volatile keeps the compiler from folding the loops away.
static void fp_bench(void)
{
	const int n = 1000000;
	volatile float fa = 1.0001f, fx = 0.5f;
	volatile double da = 1.0001, dx = 0.5;

	int64_t t0 = esp_timer_get_time();
	for (int i = 0; i < n; i++)
		fx = fx * fa + 0.25f;
	int64_t tf = esp_timer_get_time() - t0;

	t0 = esp_timer_get_time();
	for (int i = 0; i < n; i++)
		dx = dx * da + 0.25;
	int64_t td = esp_timer_get_time() - t0;

	printf("FP: float %.1f Mops/s, double %.1f Mops/s (double is %.1fx slower)\n",
		n / (double)tf, n / (double)td, (double)td / tf);
}

void app_main(void)
{
	printf("\n==== Luanti ESP32-S3 board test ====\n");
	chip_info();
	print_memory("boot");
	memcpy_bench("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, 32 * 1024);
	memcpy_bench("PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, 256 * 1024);
	fp_bench();

	board_backlight_off();
	if (board_i2c_init() != ESP_OK) {
		printf("I2C init failed; stopping\n");
		return;
	}
	board_i2c_scan();
	if (board_expander_init() != ESP_OK) {
		printf("TCA9554 not responding at 0x%02x; stopping\n", BOARD_TCA9554_ADDR);
		return;
	}
	board_rtc_print();
	printf("Battery: %.2f V (reads ~USB voltage or 0 with no battery attached)\n",
		board_battery_volts());

	stats_start();
	// Network first, so the SD benchmark shows up live on the status page
	bool net = net_connect();
	if (net) {
		stats_attach_netif(net_netif());
		if (status_web_start(CONFIG_LBT_STATUS_PORT) == ESP_OK) {
			esp_netif_ip_info_t ip;
			esp_netif_get_ip_info(net_netif(), &ip);
			printf("Status page: http://" IPSTR ":%d/\n", IP2STR(&ip.ip),
				CONFIG_LBT_STATUS_PORT);
		}
	}

	if (sd_mount()) {
		stats_attach_sd(sd_card());
		sd_benchmark(CONFIG_LBT_SD_TEST_MB);
	}
	print_memory("after SD + Wi-Fi init");
	board_beep(80);
	printf("==== Hardware tests done ====\n");

	if (net)
		net_udp_echo_forever();
}
