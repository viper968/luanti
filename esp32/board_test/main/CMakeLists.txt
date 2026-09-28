idf_component_register(SRCS "main.c" "board.c" "sd_test.c" "net_test.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_sdmmc
                                     esp_adc fatfs sdmmc esp_psram esp_timer spi_flash
                                     esp_wifi esp_netif esp_event nvs_flash lwip)
