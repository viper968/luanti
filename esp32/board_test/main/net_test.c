#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "net_test.h"

#define GOT_IP BIT0
#define FAILED BIT1
#define MAX_RETRIES 5

static EventGroupHandle_t s_events;
static int s_retries;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		if (++s_retries <= MAX_RETRIES)
			esp_wifi_connect();
		else
			xEventGroupSetBits(s_events, FAILED);
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *ev = data;
		printf("Wi-Fi: got IP " IPSTR "\n", IP2STR(&ev->ip_info.ip));
		xEventGroupSetBits(s_events, GOT_IP);
	}
}

bool net_connect(void)
{
	if (strlen(CONFIG_LBT_WIFI_SSID) == 0) {
		printf("Wi-Fi: no SSID set (idf.py menuconfig -> Luanti board test), skipping\n");
		return false;
	}

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs_flash_init();
	}
	esp_netif_init();
	esp_event_loop_create_default();
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&icfg);

	s_events = xEventGroupCreate();
	esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
	esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);

	wifi_config_t wcfg = {0};
	strncpy((char *)wcfg.sta.ssid, CONFIG_LBT_WIFI_SSID, sizeof(wcfg.sta.ssid));
	strncpy((char *)wcfg.sta.password, CONFIG_LBT_WIFI_PASSWORD, sizeof(wcfg.sta.password));
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_config(WIFI_IF_STA, &wcfg);
	esp_wifi_start();
	// A game server wants low latency, not low power
	esp_wifi_set_ps(WIFI_PS_NONE);

	EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP | FAILED,
		pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
	if (!(bits & GOT_IP)) {
		printf("Wi-Fi: could not connect to \"%s\"\n", CONFIG_LBT_WIFI_SSID);
		return false;
	}
	wifi_ap_record_t ap;
	if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
		printf("Wi-Fi: RSSI %d dBm, channel %d\n", ap.rssi, ap.primary);
	return true;
}

void net_udp_echo_forever(void)
{
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_LBT_UDP_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("UDP: bind to port %d failed\n", CONFIG_LBT_UDP_PORT);
		return;
	}
	printf("UDP: echoing on port %d\n", CONFIG_LBT_UDP_PORT);

	// Luanti packets are at most ~512 bytes on the wire
	static char buf[1500];
	unsigned long count = 0;
	for (;;) {
		struct sockaddr_storage from;
		socklen_t fromlen = sizeof(from);
		int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
		if (n < 0)
			continue;
		sendto(sock, buf, n, 0, (struct sockaddr *)&from, fromlen);
		if (++count % 1000 == 0)
			printf("UDP: %lu packets echoed\n", count);
	}
}
