idf_component_register(SRCS "wifi_repeater_main.c"
                       PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer
                       INCLUDE_DIRS ".")
