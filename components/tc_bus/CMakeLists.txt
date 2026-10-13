idf_component_register(
    SRCS "tc_proto.c" "tc_bus.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_rmt esp_driver_gpio esp_timer)
