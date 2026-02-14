idf_component_register(SRCS "wifi_repeater_main.c"
                             "repeater_config.c"
                             "repeater_httpd.c"
                       PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer esp_http_server
                       INCLUDE_DIRS ".")
