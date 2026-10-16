/* Recepteur numerique (RMT) : lit la broche RX comme une entree logique. Ne lit pas le bus
 * installe (les bas a ~0.86 V ne sont pas vus de facon fiable par l'entree numerique) ; garde
 * pour un autre cablage (CONFIG_TC_BUS_RX_RMT). */
#include "sdkconfig.h"
#if CONFIG_TC_BUS_RX_RMT
#include "tc_bus_priv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* tc_proto_rmt_words_to_segments lit les symboles comme des mots de 32 bits */
_Static_assert(sizeof(rmt_symbol_word_t) == sizeof(uint32_t), "rmt_symbol_word_t doit tenir sur 32 bits");

static const char *TAG = "tc_bus";

#define RX_SYMBOLS 48                      /* un bloc memoire RMT (48 mots sur S3) ; 96 segments, sans DMA */

static rmt_channel_handle_t s_rx;
static QueueHandle_t s_queue;
static rmt_symbol_word_t s_symbols[RX_SYMBOLS];

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
            tc_bus_set_running(false);
        }
        tc_bus_process_window(segs, k, esp_timer_get_time());
    }
}

esp_err_t tc_rx_start(void)
{
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
       il remonterait un mark du bus a ~2.4 V, illisible. On le coupe (spec 3 : entree sans pull). */
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
    if (xTaskCreate(rx_task, "tc_rx", TC_RX_TASK_STACK, NULL, TC_RX_TASK_PRIO, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    e = rmt_receive(s_rx, s_symbols, sizeof s_symbols, &s_rx_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive: %s", esp_err_to_name(e));
        return e;
    }
    tc_bus_set_running(true);
    ESP_LOGI(TAG, "reception RMT (numerique) sur GPIO%d, emission NON compilee (TX GPIO%d reste a 0)",
             CONFIG_TC_BUS_RX_GPIO, CONFIG_TC_BUS_TX_GPIO);
    return ESP_OK;
}

int tc_rx_mv(void)
{
    return -1;                             /* pas de mesure analogique avec le RMT */
}
#endif
