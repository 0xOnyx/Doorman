idf_component_register(
    SRCS "main.c" "app_status.c" "app_net.c" "ota_auth.c" "app_ota.c" "app_selftest.c" "app_config_check.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_app_format app_update nvs_flash esp_timer
                  esp_wifi esp_netif esp_event esp_http_server esp_driver_tsens netlog tc_bus)
