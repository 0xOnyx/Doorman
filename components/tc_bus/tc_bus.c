#include "tc_bus.h"
#include <stdio.h>
#include <string.h>
#include "tc_bus_priv.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static tc_tx_watch_t s_watch;            /* echo et reponse d'une emission : sous s_stats_lock */

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
    /* durees brutes (b = bus bas, h = haut) pour analyser les trames inconnues ; tampon statique :
       une seule tache de reception appelle cette fonction */
    static char line[256];
    int pos = snprintf(line, sizeof line, "TC RX ERR %s n=%u", tc_decode_error_name(err), (unsigned)k);
    if (s_err_masked && pos < (int)sizeof line) {
        pos += snprintf(line + pos, sizeof line - pos, " (+%lu masquees)", (unsigned long)s_err_masked);
    }
    if (pos < (int)sizeof line) {
        pos += snprintf(line + pos, sizeof line - pos, " (x100 us) :");   /* compact : tient dans une ligne netlog de 256 */
    }
    for (size_t i = 0; i < k && pos < (int)sizeof line - 8; i++) {
        pos += snprintf(line + pos, sizeof line - pos, " %u%c", (unsigned)((segs[i].duration_us + 50) / 100),
                        segs[i].mark ? 'b' : 'h');
    }
    ESP_LOGW(TAG, "%s", line);
    s_err_masked = 0;
}

/* Echo de notre emission, ou reponse de la centrale qui le suit (sous s_stats_lock). */
static void watch_match(const tc_telegram_t *t, int64_t now, bool *echo, bool *reply)
{
    *echo = false;
    *reply = false;
    if (!s_watch.armed) {
        return;
    }
    if (!s_watch.echo) {
        bool in_window = s_watch.end_us == 0 || now - s_watch.end_us <= TC_TX_ECHO_WINDOW_US;
        if (in_window && !t->is_response && t->raw == s_watch.raw && t->is_long == s_watch.is_long) {
            s_watch.echo = true;
            s_watch.echo_us = now;
            s_stats.tx_echo++;
            *echo = true;
        }
        return;                             /* avant l'echo : pas de reponse attendue */
    }
    if (!s_watch.reply && now - s_watch.echo_us <= TC_TX_REPLY_WINDOW_US) {
        s_watch.reply = true;
        s_watch.reply_raw = t->raw;
        s_watch.reply_is_long = t->is_long;
        s_stats.tx_reply++;
        *reply = true;
    }
}

static void handle_telegram(tc_telegram_t *t, int64_t now)
{
    tc_proto_classify(t);
    bool echo, reply;
    taskENTER_CRITICAL(&s_stats_lock);
    watch_match(t, now, &echo, &reply);
    taskEXIT_CRITICAL(&s_stats_lock);
    if (echo) {
        /* notre propre telegramme : ni "pour moi", ni rappel (spec phase 2, section 4) */
        taskENTER_CRITICAL(&s_stats_lock);
        s_stats.frames_ok++;
        if ((unsigned)t->type < TC_TYPE_COUNT) {
            s_stats.by_type[t->type]++;
        }
        s_stats.last_ok_us = now;
        s_stats.last_raw = t->raw;
        s_stats.last_is_long = t->is_long;
        s_stats.last_type = t->type;
        taskEXIT_CRITICAL(&s_stats_lock);
        ESP_LOGI(TAG, "TC RX %s raw=0x%0*lX (echo)", tc_type_name(t->type), t->is_long ? 8 : 4,
                 (unsigned long)t->raw);
        return;
    }
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
    ESP_LOGI(TAG, "TC RX %s raw=0x%0*lX len=%u serial=0x%05lX addr=%u payload=0x%lX chk=ok%s%s%s",
             tc_type_name(t->type), t->is_long ? 8 : 4, (unsigned long)t->raw,
             t->is_long ? 32u : (t->type == TC_TYPE_ACK ? 4u : 16u),
             (unsigned long)t->serial, t->address, (unsigned long)t->payload,
             t->is_response ? " rsp" : "", for_me ? " << POUR MOI" : "", reply ? " (reponse a notre emission)" : "");
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
#if CONFIG_TC_BUS_TX_ENABLE
    /* apres le recepteur : l'emetteur a besoin de lui pour le bus calme et l'echo. Un echec laisse
       TX GPIO a 0 et l'emission coupee, la reception continue. */
    if (tc_tx_start() != ESP_OK) {
        ESP_LOGE(TAG, "emetteur indisponible : emission coupee, TX GPIO%d a 0", CONFIG_TC_BUS_TX_GPIO);
    }
#endif
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

int64_t tc_bus_last_edge_us(void)
{
    return tc_rx_last_edge_us();
}

void tc_bus_watch_arm(uint32_t raw, bool is_long)
{
    taskENTER_CRITICAL(&s_stats_lock);
    memset(&s_watch, 0, sizeof s_watch);
    s_watch.armed = true;
    s_watch.raw = raw;
    s_watch.is_long = is_long;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void tc_bus_watch_sent(int64_t end_us)
{
    taskENTER_CRITICAL(&s_stats_lock);
    s_watch.end_us = end_us != 0 ? end_us : 1;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void tc_bus_watch_get(tc_tx_watch_t *w)
{
    taskENTER_CRITICAL(&s_stats_lock);
    *w = s_watch;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void tc_bus_watch_disarm(void)
{
    taskENTER_CRITICAL(&s_stats_lock);
    s_watch.armed = false;
    taskEXIT_CRITICAL(&s_stats_lock);
}

void tc_bus_count_tx(bool sent, int64_t now_us)
{
    taskENTER_CRITICAL(&s_stats_lock);
    if (sent) {
        s_stats.tx_sent++;
        s_stats.last_tx_us = now_us;
    } else {
        s_stats.tx_refused++;
    }
    taskEXIT_CRITICAL(&s_stats_lock);
}

#if !CONFIG_TC_BUS_TX_ENABLE
/* Emission non compilee : rien a bloquer (tc_tx.c fournit les vraies fonctions). */
void tc_bus_tx_block(void)
{
}

void tc_bus_tx_unblock(void)
{
}

void tc_bus_tx_set_inhibit(tc_bus_tx_inhibit_fn_t fn)
{
    (void)fn;
}
#endif
