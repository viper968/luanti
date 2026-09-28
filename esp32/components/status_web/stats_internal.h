#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

// Raw counters, bumped from driver context with relaxed atomics. 32-bit so
// they're cheap on Xtensa; the sampler turns deltas into 64-bit totals.
extern uint32_t g_sd_rd_bytes, g_sd_wr_bytes, g_sd_ops;
extern uint32_t g_net_rx_bytes, g_net_tx_bytes, g_net_rx_pkts, g_net_tx_pkts;

// Set by the attach functions; read by the sampler
extern esp_netif_t *g_stats_netif;
unsigned char stats_sd_pdrv(void);

static inline void stats_add(uint32_t *c, uint32_t n)
{
	__atomic_fetch_add(c, n, __ATOMIC_RELAXED);
}

#define HISTORY_LEN 120

typedef struct {
	int16_t cpu[2];      // per-mille busy per core, -1 if unavailable
	uint32_t net_rx, net_tx; // bytes/s
	uint32_t sd_rd, sd_wr;   // bytes/s
} stats_sample_t;

typedef struct {
	int64_t uptime_us;
	stats_sample_t now;
	uint64_t net_rx_total, net_tx_total, net_rx_pkts, net_tx_pkts;
	uint64_t sd_rd_total, sd_wr_total, sd_ops;
	uint32_t int_free, int_total, int_min, int_largest;
	uint32_t ps_free, ps_total, ps_min, ps_largest;
	bool sd_present;
	uint64_t sd_used, sd_size;
	bool net_up;
	int rssi;
	char ip[16];
	unsigned tasks;
} stats_snapshot_t;

// Copies the latest snapshot / history (oldest first) under a lock.
void stats_get(stats_snapshot_t *out);
unsigned stats_get_history(stats_sample_t *out, unsigned max);
