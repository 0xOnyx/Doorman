#include "tc_bus.h"
#include <string.h>
#include "sdkconfig.h"

#if CONFIG_TC_BUS_TX_ENABLE
#error "L'emission n'est pas implementee en phase 0 : TC_BUS_TX_ENABLE doit rester a n"
#endif

static tc_bus_stats_t s_stats;
static bool s_running;

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx) { (void)cb; (void)ctx; s_running = true; return ESP_OK; }
bool tc_bus_is_running(void) { return s_running; }
void tc_bus_get_stats(tc_bus_stats_t *s) { *s = s_stats; }
void tc_bus_tx_block(void) { }
void tc_bus_tx_unblock(void) { }
