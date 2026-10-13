idf_component_register(
    SRCS "main.c" "app_status.c" "app_net.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_app_format app_update nvs_flash esp_timer
                  esp_wifi esp_netif esp_event netlog tc_bus)
