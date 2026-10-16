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

_Static_assert(CONFIG_TC_BUS_MY_SERIAL <= 0xFFFFF, "TC_BUS_MY_SERIAL : numero de serie sur 20 bits (<= 0xFFFFF)");
/* tc_proto_rmt_words_to_segments lit les symboles comme des mots de 32 bits */
_Static_assert(sizeof(rmt_symbol_word_t) == sizeof(uint32_t), "rmt_symbol_word_t doit tenir sur 32 bits");

static const char *TAG = "tc_bus";

#define RX_SYMBOLS 48                      /* un bloc memoire RMT (48 mots sur S3) ; 96 segments, sans DMA */
/* Sous httpd (5), auto-test (5) et netlog (4) : du bruit sur le bus ne doit jamais affamer l'acces
   distant. Le RMT bufferise en materiel, la latence de decodage n'a pas d'importance en phase 0. */
#define RX_TASK_PRIO 3
#define ERR_LOG_PERIOD_US 1000000LL        /* au plus une ligne TC RX ERR par seconde */
#define CALL_FOR_ME_WINDOW_US (3LL * 60 * 1000000)   /* un OPEN_DOOR court suit un appel pour moi de < 3 min */

static rmt_channel_handle_t s_rx;
static QueueHandle_t s_queue;
static rmt_symbol_word_t s_symbols[RX_SYMBOLS];
static tc_bus_stats_t s_stats;           /* ecrit par tc_rx, lu par tc_bus_get_stats : sous s_stats_lock */
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
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

static void rx_task(void *arg)
{
    (void)arg;
    size_t nsym;
    tc_segment_t segs[TC_MAX_SEGMENTS];
    int64_t last_err_log_us = -ERR_LOG_PERIOD_US;
    uint32_t err_masked = 0;               /* erreurs non loggees depuis la derniere ligne */
    int64_t last_call_for_me_us = 0;       /* dernier DOOR_CALL (platine de rue) pour moi, 0 si aucun */
    for (;;) {
        if (xQueueReceive(s_queue, &nsym, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (nsym > RX_SYMBOLS) {
            nsym = RX_SYMBOLS;
        }
        size_t k = tc_proto_rmt_words_to_segments(&s_symbols[0].val, nsym, segs, TC_MAX_SEGMENTS);
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
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_stats_lock);
        s_stats.last_rx_us = now;
        taskEXIT_CRITICAL(&s_stats_lock);
        k = tc_proto_merge_glitches(segs, k, CONFIG_TC_BUS_GLITCH_US);   /* d'abord : un parasite dans le repos */
        k = tc_proto_strip_trailing_idle(segs, k);                        /* ensuite : retirer le repos */
        if (k < TC_SEGMENTS_ACK) {
            /* bruit trop court pour une trame : compte sans inonder les logs */
            taskENTER_CRITICAL(&s_stats_lock);
            s_stats.frames_err++;
            taskEXIT_CRITICAL(&s_stats_lock);
            continue;
        }

        tc_telegram_t t;
        tc_decode_error_t err;
        if (tc_proto_decode(segs, k, &t, &err)) {
            tc_proto_classify(&t);
            bool recent = last_call_for_me_us != 0 && now - last_call_for_me_us < CALL_FOR_ME_WINDOW_US;
            bool for_me = tc_proto_is_for_me(&t, CONFIG_TC_BUS_MY_SERIAL, CONFIG_TC_BUS_DOOR_ADDRESS, recent);
            if (for_me && t.type == TC_TYPE_DOOR_CALL) {   /* un appel interne ne concerne pas la porte de rue */
                last_call_for_me_us = now;
            }
            taskENTER_CRITICAL(&s_stats_lock);
            s_stats.frames_ok++;
            if ((unsigned)t.type < TC_TYPE_COUNT) {
                s_stats.by_type[t.type]++;
            }
            if (for_me) {
                s_stats.for_me++;
            }
            s_stats.last_ok_us = now;
            s_stats.last_raw = t.raw;
            s_stats.last_is_long = t.is_long;
            s_stats.last_type = t.type;
            taskEXIT_CRITICAL(&s_stats_lock);
            ESP_LOGI(TAG, "TC RX %s raw=0x%0*lX len=%u serial=0x%05lX addr=%u payload=0x%lX chk=ok%s%s",
                     tc_type_name(t.type), t.is_long ? 8 : 4, (unsigned long)t.raw,
                     t.is_long ? 32u : (t.type == TC_TYPE_ACK ? 4u : 16u),
                     (unsigned long)t.serial, t.address, (unsigned long)t.payload,
                     t.is_response ? " rsp" : "", for_me ? " << POUR MOI" : "");
            if (s_cb) {
                s_cb(&t, s_ctx);
            }
        } else {
            taskENTER_CRITICAL(&s_stats_lock);
            s_stats.frames_err++;
            taskEXIT_CRITICAL(&s_stats_lock);
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

#if CONFIG_TC_BUS_ADC_PROBE
esp_err_t tc_adc_probe_start(volatile bool *running);
#endif

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
#if CONFIG_TC_BUS_ADC_PROBE
    return tc_adc_probe_start(&s_running);
#endif
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
#if CONFIG_TC_BUS_MY_SERIAL != 0
    ESP_LOGI(TAG, "numero de serie configure : 0x%05lX, platine adresse %d",
             (unsigned long)CONFIG_TC_BUS_MY_SERIAL, CONFIG_TC_BUS_DOOR_ADDRESS);
#else
    ESP_LOGW(TAG, "numero de serie non configure : relever un DOOR_CALL (docs/utilisation.md, section 7)");
#endif
    return ESP_OK;
}

bool tc_bus_is_running(void)
{
    return s_running;
}

void tc_bus_get_stats(tc_bus_stats_t *s)
{
    taskENTER_CRITICAL(&s_stats_lock);
    *s = s_stats;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void tc_bus_tx_block(void)
{
    /* no-op en phase 0 : rien n'emet */
}

void tc_bus_tx_unblock(void)
{
}
