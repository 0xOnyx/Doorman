#include "tc_bus.h"
#include "tc_bus_priv.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_TC_BUS_TX_ENABLE
#error "L'emission n'est pas implementee en phase 0 : TC_BUS_TX_ENABLE doit rester a n"
#endif

_Static_assert(CONFIG_TC_BUS_MY_SERIAL <= 0xFFFFF, "TC_BUS_MY_SERIAL : numero de serie sur 20 bits (<= 0xFFFFF)");

static const char *TAG = "tc_bus";

#define ERR_LOG_PERIOD_US 1000000LL        /* au plus une ligne TC RX ERR par seconde */
#define CALL_FOR_ME_WINDOW_US (3LL * 60 * 1000000)   /* un OPEN_DOOR court suit un appel pour moi de < 3 min */

static tc_bus_stats_t s_stats;           /* ecrit par la tache de reception, lu par tc_bus_get_stats : sous s_stats_lock */
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_running;
static tc_bus_rx_cb_t s_cb;
static void *s_ctx;

/* Etat propre a la tache de reception (un seul appelant de tc_bus_process_window) */
static int64_t s_last_err_log_us = -ERR_LOG_PERIOD_US;
static uint32_t s_err_masked;            /* erreurs non loggees depuis la derniere ligne */
static int64_t s_last_call_for_me_us;    /* dernier DOOR_CALL (platine de rue) pour moi, 0 si aucun */

static void count_error(void)
{
    taskENTER_CRITICAL(&s_stats_lock);
    s_stats.frames_err++;
    taskEXIT_CRITICAL(&s_stats_lock);
}

static void log_error(tc_decode_error_t err, const tc_segment_t *segs, size_t k, int64_t now)
{
    if (now - s_last_err_log_us < ERR_LOG_PERIOD_US) {
        s_err_masked++;
        return;
    }
    s_last_err_log_us = now;
    if (s_err_masked) {
        ESP_LOGW(TAG, "TC RX ERR %s n=%u first=%u last=%u (+%lu masquees)", tc_decode_error_name(err),
                 (unsigned)k, segs[0].duration_us, segs[k - 1].duration_us, (unsigned long)s_err_masked);
    } else {
        ESP_LOGW(TAG, "TC RX ERR %s n=%u first=%u last=%u", tc_decode_error_name(err),
                 (unsigned)k, segs[0].duration_us, segs[k - 1].duration_us);
    }
    s_err_masked = 0;
}

static void handle_telegram(tc_telegram_t *t, int64_t now)
{
    tc_proto_classify(t);
    bool recent = s_last_call_for_me_us != 0 && now - s_last_call_for_me_us < CALL_FOR_ME_WINDOW_US;
    bool for_me = tc_proto_is_for_me(t, CONFIG_TC_BUS_MY_SERIAL, CONFIG_TC_BUS_DOOR_ADDRESS, recent);
    if (for_me && t->type == TC_TYPE_DOOR_CALL) {   /* un appel interne ne concerne pas la porte de rue */
        s_last_call_for_me_us = now;
    }
    taskENTER_CRITICAL(&s_stats_lock);
    s_stats.frames_ok++;
    if ((unsigned)t->type < TC_TYPE_COUNT) {
        s_stats.by_type[t->type]++;
    }
    if (for_me) {
        s_stats.for_me++;
    }
    s_stats.last_ok_us = now;
    s_stats.last_raw = t->raw;
    s_stats.last_is_long = t->is_long;
    s_stats.last_type = t->type;
    taskEXIT_CRITICAL(&s_stats_lock);
    ESP_LOGI(TAG, "TC RX %s raw=0x%0*lX len=%u serial=0x%05lX addr=%u payload=0x%lX chk=ok%s%s",
             tc_type_name(t->type), t->is_long ? 8 : 4, (unsigned long)t->raw,
             t->is_long ? 32u : (t->type == TC_TYPE_ACK ? 4u : 16u),
             (unsigned long)t->serial, t->address, (unsigned long)t->payload,
             t->is_response ? " rsp" : "", for_me ? " << POUR MOI" : "");
    if (s_cb) {
        s_cb(t, s_ctx);
    }
}

void tc_bus_process_window(tc_segment_t *segs, size_t n, int64_t now)
{
    if (n < 2) {
        return;                             /* evenement vide ou parasite isole */
    }
    taskENTER_CRITICAL(&s_stats_lock);
    s_stats.last_rx_us = now;
    taskEXIT_CRITICAL(&s_stats_lock);
    size_t k = tc_proto_merge_glitches(segs, n, CONFIG_TC_BUS_GLITCH_US);   /* d'abord : un parasite dans le repos */
    k = tc_proto_strip_trailing_idle(segs, k);                               /* ensuite : retirer le repos */
    if (k < TC_SEGMENTS_ACK) {
        count_error();                      /* bruit trop court pour une trame : compte sans inonder les logs */
        return;
    }
    /* plusieurs telegrammes peuvent se suivre sans repos de 7 ms (ex. commande puis ACK) */
    size_t pos = 0, st, len;
    bool any = false;
    while (tc_proto_next_frame(segs, k, &pos, &st, &len)) {
        tc_telegram_t t;
        tc_decode_error_t err;
        any = true;
        if (tc_proto_decode(&segs[st], len, &t, &err)) {
            handle_telegram(&t, now);
        } else {
            count_error();
            log_error(err, &segs[st], len, now);
        }
    }
    if (!any) {
        /* aucun depart reconnu : l'erreur du decodage de la fenetre entiere dit pourquoi */
        tc_telegram_t t;
        tc_decode_error_t err = TC_ERR_START;
        (void)tc_proto_decode(segs, k, &t, &err);
        count_error();
        log_error(err, segs, k, now);
    }
}

void tc_bus_set_running(bool running)
{
    s_running = running;
}

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    esp_err_t e = tc_rx_start();
    if (e != ESP_OK) {
        return e;
    }
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

int tc_bus_rx_mv(void)
{
    return tc_rx_mv();
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
