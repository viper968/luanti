#pragma once
#include <stdbool.h>
#include "esp_netif.h"

// Connects to Wi-Fi (station mode, power save off). Returns false if no SSID
// is configured or the connection fails.
bool net_connect(void);
// The Wi-Fi station interface, or NULL before net_connect()
esp_netif_t *net_netif(void);
// Echoes UDP packets on CONFIG_LBT_UDP_PORT forever. Pair with
// esp32/tools/udp_ping.py on a PC to measure round-trip latency and loss.
void net_udp_echo_forever(void);
