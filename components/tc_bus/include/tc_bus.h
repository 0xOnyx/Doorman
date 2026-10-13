#ifndef TC_BUS_H
#define TC_BUS_H
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tc_proto.h"

typedef struct {
    uint32_t frames_ok;
    uint32_t frames_err;
    int64_t  last_rx_us;     /* esp_timer_get_time() de la derniere trame, 0 si aucune */
} tc_bus_stats_t;

typedef void (*tc_bus_rx_cb_t)(const tc_telegram_t *t, void *ctx);

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx);
bool      tc_bus_is_running(void);
void      tc_bus_get_stats(tc_bus_stats_t *s);

/* Emission : no-op en phase 0 (TX non compile), appele par l'OTA. */
void      tc_bus_tx_block(void);
void      tc_bus_tx_unblock(void);

#endif
