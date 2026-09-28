#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "ff.h"
#include "diskio_impl.h"
#include "status_web.h"
#include "stats_internal.h"

#define SAMPLE_MS 1000
#define STORAGE_EVERY 30 // f_getfree can scan the whole FAT, so do it rarely

uint32_t g_sd_rd_bytes, g_sd_wr_bytes, g_sd_ops;
uint32_t g_net_rx_bytes, g_net_tx_bytes, g_net_rx_pkts, g_net_tx_pkts;
esp_netif_t *g_stats_netif;

static SemaphoreHandle_t s_lock;
static stats_snapshot_t s_snap;
static stats_sample_t s_hist[HISTORY_LEN];
static unsigned s_hist_head, s_hist_count;

static uint32_t take(uint32_t *counter, uint32_t *last)
{
	uint32_t now = __atomic_load_n(counter, __ATOMIC_RELAXED);
	uint32_t d = now - *last; // unsigned math survives wraparound
	*last = now;
	return d;
}

static uint32_t per_second(uint32_t delta, int64_t dt_us)
{
	return dt_us > 0 ? (uint32_t)((uint64_t)delta * 1000000 / dt_us) : 0;
}

static void sample_storage(stats_snapshot_t *s)
{
	BYTE pdrv = stats_sd_pdrv();
	if (pdrv == FF_DRV_NOT_USED)
		return;
	char drv[3] = {(char)('0' + pdrv), ':', 0};
	FATFS *fs;
	DWORD free_clust;
	if (f_getfree(drv, &free_clust, &fs) != FR_OK)
		return;
	uint64_t cluster = (uint64_t)fs->csize * FF_MIN_SS;
#if FF_MAX_SS != FF_MIN_SS
	cluster = (uint64_t)fs->csize * fs->ssize;
#endif
	s->sd_size = (uint64_t)(fs->n_fatent - 2) * cluster;
	s->sd_used = s->sd_size - (uint64_t)free_clust * cluster;
	s->sd_present = true;
}

static void sampler(void *arg)
{
	uint32_t l_sdr = 0, l_sdw = 0, l_sdo = 0, l_rx = 0, l_tx = 0, l_rxp = 0, l_txp = 0;
	uint32_t l_idle[2] = {0, 0};
	int64_t last = esp_timer_get_time();
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
	for (int c = 0; c < 2; c++)
		l_idle[c] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);
#endif
	unsigned tick = 0;
	stats_snapshot_t s = {0};

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
		int64_t now = esp_timer_get_time();
		int64_t dt = now - last;
		last = now;

		stats_sample_t *m = &s.now;
		for (int c = 0; c < 2; c++) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
			// Run-time counter and esp_timer both tick in microseconds
			uint32_t idle = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);
			int64_t busy = 1000 - (int64_t)(idle - l_idle[c]) * 1000 / dt;
			l_idle[c] = idle;
			m->cpu[c] = busy < 0 ? 0 : busy > 1000 ? 1000 : busy;
#else
			m->cpu[c] = -1;
#endif
		}

		uint32_t d;
		d = take(&g_net_rx_bytes, &l_rx); s.net_rx_total += d; m->net_rx = per_second(d, dt);
		d = take(&g_net_tx_bytes, &l_tx); s.net_tx_total += d; m->net_tx = per_second(d, dt);
		s.net_rx_pkts += take(&g_net_rx_pkts, &l_rxp);
		s.net_tx_pkts += take(&g_net_tx_pkts, &l_txp);
		d = take(&g_sd_rd_bytes, &l_sdr); s.sd_rd_total += d; m->sd_rd = per_second(d, dt);
		d = take(&g_sd_wr_bytes, &l_sdw); s.sd_wr_total += d; m->sd_wr = per_second(d, dt);
		s.sd_ops += take(&g_sd_ops, &l_sdo);

		s.int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
		s.int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
		s.int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
		s.int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
		s.ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
		s.ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
		s.ps_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
		s.ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
		s.tasks = uxTaskGetNumberOfTasks();
		s.uptime_us = now;

		s.net_up = false;
		if (g_stats_netif && esp_netif_is_netif_up(g_stats_netif)) {
			esp_netif_ip_info_t ip;
			if (esp_netif_get_ip_info(g_stats_netif, &ip) == ESP_OK && ip.ip.addr) {
				s.net_up = true;
				snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&ip.ip));
			}
			wifi_ap_record_t ap;
			s.rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
		}

		// Retry every second until a card is attached, then refresh rarely
		if (!s.sd_present || tick % STORAGE_EVERY == 0)
			sample_storage(&s);
		tick++;

		xSemaphoreTake(s_lock, portMAX_DELAY);
		s_snap = s;
		s_hist[s_hist_head] = s.now;
		s_hist_head = (s_hist_head + 1) % HISTORY_LEN;
		if (s_hist_count < HISTORY_LEN)
			s_hist_count++;
		xSemaphoreGive(s_lock);
	}
}

esp_err_t stats_start(void)
{
	if (s_lock)
		return ESP_OK;
	s_lock = xSemaphoreCreateMutex();
	if (!s_lock)
		return ESP_ERR_NO_MEM;
	// Low priority: stats must never steal time from the game server
	return xTaskCreate(sampler, "stats", 4096, NULL, 2, NULL) == pdPASS ?
		ESP_OK : ESP_ERR_NO_MEM;
}

void stats_get(stats_snapshot_t *out)
{
	xSemaphoreTake(s_lock, portMAX_DELAY);
	*out = s_snap;
	xSemaphoreGive(s_lock);
}

unsigned stats_get_history(stats_sample_t *out, unsigned max)
{
	xSemaphoreTake(s_lock, portMAX_DELAY);
	unsigned n = s_hist_count < max ? s_hist_count : max;
	unsigned start = (s_hist_head + HISTORY_LEN - n) % HISTORY_LEN;
	for (unsigned i = 0; i < n; i++)
		out[i] = s_hist[(start + i) % HISTORY_LEN];
	xSemaphoreGive(s_lock);
	return n;
}
