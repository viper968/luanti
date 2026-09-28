idf_component_register(SRCS "stats.c" "sd_counting.c" "net_counting.c" "web.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_netif sdmmc fatfs
                       PRIV_REQUIRES esp_timer esp_wifi lwip heap freertos
                       EMBED_TXTFILES "www/index.html")
