#include "tc_bus.h"
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#if CONFIG_TC_BUS_TX_ENABLE
#error "L'emission n'est pas implementee en phase 0 : TC_BUS_TX_ENABLE doit rester a n"
#endif

static const char *TAG = "tc_bus";

#define RX_SYMBOLS 48                      /* un bloc memoire RMT (48 mots sur S3) ; 96 segments, sans DMA */
/* Sous httpd (5), auto-test (5) et netlog (4) : du bruit sur le bus ne doit jamais affamer l'acces
   distant. Le RMT bufferise en materiel, la latence de decodage n'a pas d'importance en phase 0. */
#define RX_TASK_PRIO 3
#define ERR_LOG_PERIOD_US 1000000LL        /* au plus une ligne TC RX ERR par seconde */

static rmt_channel_handle_t s_rx;
static QueueHandle_t s_queue;
static rmt_symbol_word_t s_symbols[RX_SYMBOLS];
static tc_bus_stats_t s_stats;
static volatile bool s_running;
static tc_bus_rx_cb_t s_cb;
static void *s_ctx;

static const rmt_receive_config_t s_rx_cfg = {
    .signal_range_min_ns = 2500,           /* filtre <= 255 ticks de 80 MHz (~3.2 us) ; le vrai filtre est logiciel */
    .signal_range_max_ns = 7000000,        /* > 7 ms au repos = fin de trame */
};

static bool IRAM_ATTR on_recv_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *ctx)
{
    (void)ch; (void)ctx;
    BaseType_t hp = pdFALSE;
    size_t n = ed->num_symbols;
    xQueueSendFromISR(s_queue, &n, &hp);
    return hp == pdTRUE;
}

static size_t symbols_to_segments(size_t nsym, tc_segment_t *segs)
{
    size_t k = 0;
    for (size_t i = 0; i < nsym && k < TC_MAX_SEGMENTS; i++) {
        if (s_symbols[i].duration0 == 0) {
            break;
        }
        segs[k].duration_us = s_symbols[i].duration0;
        segs[k].mark = (s_symbols[i].level0 == 0);      /* bus bas = GPIO RX a 0 */
        k++;
        if (s_symbols[i].duration1 == 0 || k >= TC_MAX_SEGMENTS) {
            break;
        }
        segs[k].duration_us = s_symbols[i].duration1;
        segs[k].mark = (s_symbols[i].level1 == 0);
        k++;
    }
    return k;
}

static void rx_task(void *arg)
{
    (void)arg;
    size_t nsym;
    tc_segment_t segs[TC_MAX_SEGMENTS];
    int64_t last_err_log_us = -ERR_LOG_PERIOD_US;
    uint32_t err_masked = 0;               /* erreurs non loggees depuis la derniere ligne */
    for (;;) {
        if (xQueueReceive(s_queue, &nsym, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t k = symbols_to_segments(nsym, segs);
        /* rearmer tout de suite : les symboles sont copies */
        esp_err_t e = rmt_receive(s_rx, s_symbols, sizeof s_symbols, &s_rx_cfg);
        if (e != ESP_OK) {
            vTaskDelay(1);
            e = rmt_receive(s_rx, s_symbols, sizeof s_symbols, &s_rx_cfg);
        }
        if (e != ESP_OK) {
            /* reception morte : l'auto-test et /info doivent le voir */
            ESP_LOGE(TAG, "rmt_receive: %s, reception arretee", esp_err_to_name(e));
            s_running = false;
        }
        if (k < 2) {
            continue;                       /* evenement vide ou parasite isole */
        }
        s_stats.last_rx_us = esp_timer_get_time();
        k = tc_proto_merge_glitches(segs, k, CONFIG_TC_BUS_GLITCH_US);   /* d'abord : un parasite dans le repos */
        k = tc_proto_strip_trailing_idle(segs, k);                        /* ensuite : retirer le repos */
        if (k < TC_SEGMENTS_ACK) {
            s_stats.frames_err++;           /* bruit trop court pour une trame : compte sans inonder les logs */
            continue;
        }

        tc_telegram_t t;
        tc_decode_error_t err;
        if (tc_proto_decode(segs, k, &t, &err)) {
            tc_proto_classify(&t);
            s_stats.frames_ok++;
            ESP_LOGI(TAG, "TC RX %s raw=0x%0*lX len=%u serial=0x%05lX addr=%u payload=0x%lX chk=ok%s",
                     tc_type_name(t.type), t.is_long ? 8 : 4, (unsigned long)t.raw,
                     t.is_long ? 32u : (t.type == TC_TYPE_ACK ? 4u : 16u),
                     (unsigned long)t.serial, t.address, (unsigned long)t.payload,
                     t.is_response ? " rsp" : "");
            if (s_cb) {
                s_cb(&t, s_ctx);
            }
        } else {
            s_stats.frames_err++;
            int64_t now = esp_timer_get_time();
            if (now - last_err_log_us < ERR_LOG_PERIOD_US) {
                err_masked++;
                continue;
            }
            last_err_log_us = now;
            if (err_masked) {
                ESP_LOGW(TAG, "TC RX ERR %s n=%u first=%u last=%u (+%lu masquees)", tc_decode_error_name(err),
                         (unsigned)k, segs[0].duration_us, segs[k - 1].duration_us, (unsigned long)err_masked);
            } else {
                ESP_LOGW(TAG, "TC RX ERR %s n=%u first=%u last=%u", tc_decode_error_name(err),
                         (unsigned)k, segs[0].duration_us, segs[k - 1].duration_us);
            }
            err_masked = 0;
        }
    }
}

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    s_queue = xQueueCreate(4, sizeof(size_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rmt_rx_channel_config_t cfg = {
        .gpio_num = CONFIG_TC_BUS_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,          /* 1 tick = 1 us */
        .mem_block_symbols = RX_SYMBOLS,
    };
    esp_err_t e = rmt_new_rx_channel(&cfg, &s_rx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel: %s", esp_err_to_name(e));
        return e;
    }
    /* rmt_new_rx_channel active le pull-up interne (~45 kOhm, rmt_rx.c) : sur le diviseur 1 M / 147 k
       il remonterait un mark du bus a ~2.4 V, illisible. On le coupe (spec 3 : GPIO4 sans pull). */
    gpio_pullup_dis(CONFIG_TC_BUS_RX_GPIO);
    gpio_pulldown_dis(CONFIG_TC_BUS_RX_GPIO);
    const rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv_done };
    e = rmt_rx_register_event_callbacks(s_rx, &cbs, NULL);
    if (e == ESP_OK) {
        e = rmt_enable(s_rx);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt init: %s", esp_err_to_name(e));
        return e;
    }
    if (xTaskCreate(rx_task, "tc_rx", 4096, NULL, RX_TASK_PRIO, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    e = rmt_receive(s_rx, s_symbols, sizeof s_symbols, &s_rx_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive: %s", esp_err_to_name(e));
        return e;
    }
    s_running = true;
    ESP_LOGI(TAG, "reception RMT sur GPIO%d, emission NON compilee (TX GPIO%d reste a 0)",
             CONFIG_TC_BUS_RX_GPIO, CONFIG_TC_BUS_TX_GPIO);
    return ESP_OK;
}

bool tc_bus_is_running(void)
{
    return s_running;
}

void tc_bus_get_stats(tc_bus_stats_t *s)
{
    *s = s_stats;
}

void tc_bus_tx_block(void)
{
    /* no-op en phase 0 : rien n'emet */
}

void tc_bus_tx_unblock(void)
{
}
