// Counts link-layer traffic on an lwIP netif by wrapping its input and
// linkoutput hooks. For the Wi-Fi STA, wlanif_input() hands every received
// frame to netif->input and lwIP sends every frame through netif->linkoutput.
#include "lwip/netif.h"
#include "esp_netif.h" // must precede esp_netif_net_stack.h
#include "esp_netif_net_stack.h"
#include "status_web.h"
#include "stats_internal.h"

static netif_input_fn s_orig_input;
static netif_linkoutput_fn s_orig_linkoutput;

static err_t cnt_input(struct pbuf *p, struct netif *inp)
{
	stats_add(&g_net_rx_bytes, p->tot_len);
	stats_add(&g_net_rx_pkts, 1);
	return s_orig_input(p, inp);
}

static err_t cnt_linkoutput(struct netif *n, struct pbuf *p)
{
	stats_add(&g_net_tx_bytes, p->tot_len);
	stats_add(&g_net_tx_pkts, 1);
	return s_orig_linkoutput(n, p);
}

// Runs in the TCP/IP thread so no packet is mid-flight through the hooks
static esp_err_t hook(void *ctx)
{
	struct netif *n = ctx;
	if (n->input == cnt_input)
		return ESP_OK; // already attached
	s_orig_input = n->input;
	s_orig_linkoutput = n->linkoutput;
	n->input = cnt_input;
	n->linkoutput = cnt_linkoutput;
	return ESP_OK;
}

esp_err_t stats_attach_netif(esp_netif_t *netif)
{
	struct netif *n = esp_netif_get_netif_impl(netif);
	if (!n || !n->input || !n->linkoutput)
		return ESP_ERR_INVALID_STATE;
	esp_err_t err = esp_netif_tcpip_exec(hook, n);
	if (err == ESP_OK)
		g_stats_netif = netif;
	return err;
}
