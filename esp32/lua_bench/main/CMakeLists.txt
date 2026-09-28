idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES lua51 esp_timer esp_psram
                       EMBED_TXTFILES "bench.lua")
