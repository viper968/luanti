// Tiny web status page for the headless ESP32-S3 Luanti server:
// CPU load per core, RAM, TF card usage and throughput, Wi-Fi throughput.
//
// Needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y for CPU load
// (reported as -1 otherwise).
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the 1 Hz sampler task. Call once, before the attach functions.
esp_err_t stats_start(void);

// Counts TF card traffic by swapping in a counting FatFs disk driver.
// Call after esp_vfs_fat_sdmmc_mount() succeeded.
esp_err_t stats_attach_sd(sdmmc_card_t *card);

// Counts every frame sent/received on this interface (e.g. the Wi-Fi STA).
// Only one interface can be attached.
esp_err_t stats_attach_netif(esp_netif_t *netif);

// Serves the page on "/" and JSON on "/api/stats" and "/api/history".
esp_err_t status_web_start(uint16_t port);

#ifdef __cplusplus
}
#endif
