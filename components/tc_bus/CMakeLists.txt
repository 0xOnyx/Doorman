idf_component_register(
    SRCS "tc_proto.c" "tc_slicer.c" "tc_bus.c" "tc_rx_adc.c" "tc_rx_rmt.c"
         "tc_tx_policy.c" "tc_tx.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_rmt esp_driver_gpio esp_timer esp_adc)
