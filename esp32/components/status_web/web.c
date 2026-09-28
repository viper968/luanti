#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "status_web.h"
#include "stats_internal.h"

static const char *TAG = "status_web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t get_index(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	// EMBED_TXTFILES appends a NUL terminator; don't send it
	return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t get_stats(httpd_req_t *req)
{
	stats_snapshot_t s;
	stats_get(&s);
	char buf[900];
	int n = snprintf(buf, sizeof(buf),
		"{\"uptime\":%lld,\"tasks\":%u,"
		"\"cpu\":[%d,%d],"
		"\"ram\":{\"int\":{\"free\":%lu,\"total\":%lu,\"min\":%lu,\"largest\":%lu},"
		"\"psram\":{\"free\":%lu,\"total\":%lu,\"min\":%lu,\"largest\":%lu}},"
		"\"sd\":{\"present\":%s,\"used\":%llu,\"size\":%llu,"
		"\"rd\":%lu,\"wr\":%lu,\"rd_total\":%llu,\"wr_total\":%llu,\"ops\":%llu},"
		"\"net\":{\"up\":%s,\"ip\":\"%s\",\"rssi\":%d,"
		"\"rx\":%lu,\"tx\":%lu,\"rx_total\":%llu,\"tx_total\":%llu,"
		"\"rx_pkts\":%llu,\"tx_pkts\":%llu}}",
		s.uptime_us / 1000000, s.tasks,
		s.now.cpu[0], s.now.cpu[1],
		(unsigned long)s.int_free, (unsigned long)s.int_total,
		(unsigned long)s.int_min, (unsigned long)s.int_largest,
		(unsigned long)s.ps_free, (unsigned long)s.ps_total,
		(unsigned long)s.ps_min, (unsigned long)s.ps_largest,
		s.sd_present ? "true" : "false",
		(unsigned long long)s.sd_used, (unsigned long long)s.sd_size,
		(unsigned long)s.now.sd_rd, (unsigned long)s.now.sd_wr,
		(unsigned long long)s.sd_rd_total, (unsigned long long)s.sd_wr_total,
		(unsigned long long)s.sd_ops,
		s.net_up ? "true" : "false", s.net_up ? s.ip : "", s.rssi,
		(unsigned long)s.now.net_rx, (unsigned long)s.now.net_tx,
		(unsigned long long)s.net_rx_total, (unsigned long long)s.net_tx_total,
		(unsigned long long)s.net_rx_pkts, (unsigned long long)s.net_tx_pkts);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

// [[cpu0,cpu1,net_rx,net_tx,sd_rd,sd_wr], ...] oldest first, streamed in chunks
static esp_err_t get_history(httpd_req_t *req)
{
	static stats_sample_t hist[HISTORY_LEN]; // httpd serves one request at a time
	unsigned n = stats_get_history(hist, HISTORY_LEN);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	char buf[96];
	httpd_resp_send_chunk(req, "[", 1);
	for (unsigned i = 0; i < n; i++) {
		const stats_sample_t *h = &hist[i];
		int len = snprintf(buf, sizeof(buf), "%s[%d,%d,%lu,%lu,%lu,%lu]",
			i ? "," : "", h->cpu[0], h->cpu[1],
			(unsigned long)h->net_rx, (unsigned long)h->net_tx,
			(unsigned long)h->sd_rd, (unsigned long)h->sd_wr);
		if (httpd_resp_send_chunk(req, buf, len) != ESP_OK)
			return ESP_FAIL;
	}
	httpd_resp_send_chunk(req, "]", 1);
	return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t status_web_start(uint16_t port)
{
	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	cfg.server_port = port;
	cfg.ctrl_port = port + 1;
	cfg.task_priority = 2; // below the game server
	cfg.stack_size = 6144;
	cfg.max_open_sockets = 4;
	cfg.lru_purge_enable = true;

	httpd_handle_t server;
	esp_err_t err = httpd_start(&server, &cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
		return err;
	}
	const httpd_uri_t uris[] = {
		{.uri = "/", .method = HTTP_GET, .handler = get_index},
		{.uri = "/api/stats", .method = HTTP_GET, .handler = get_stats},
		{.uri = "/api/history", .method = HTTP_GET, .handler = get_history},
	};
	for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
		httpd_register_uri_handler(server, &uris[i]);
	return ESP_OK;
}
