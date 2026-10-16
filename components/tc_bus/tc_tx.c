/* Emetteur du bus (spec phase 2, section 3), compile seulement avec CONFIG_TC_BUS_TX_ENABLE.
 * RMT TX sur TX GPIO (base du transistor : 1 = bus tire a 0 V), 1 MHz, sans porteuse, repos a 0.
 * Un seul telegramme peut sortir : l'ouverture configuree dans menuconfig, jamais un raw venu du
 * reseau. Chaque emission passe par tc_tx_check (liste blanche, 200 ms, 5/min, bus calme, blocage).
 *
 * Absence de front montant parasite sur TX GPIO (verifie dans ESP-IDF v5.5.5, esp_driver_rmt) :
 * - rmt_new_tx_channel fixe le niveau de repos du canal (rmt_ll_tx_fix_idle_level, init_level = 0)
 *   AVANT de router le signal RMT vers la broche (esp_rom_gpio_connect_out_signal) ; avant ce
 *   routage la broche reste la sortie GPIO a 0 de bus_gpio_safe() (main.c).
 * - rmt_enable ne demarre aucune emission (pas de transaction en attente) ; rmt_transmit refixe le
 *   repos a eot_level (0) juste avant rmt_ll_tx_start.
 * - fin d'emission : chaque symbole finit a 0, puis le marqueur de fin ajoute par le pilote laisse
 *   la sortie au repos (eot_level = 0).
 * - en cas d'erreur on ne supprime JAMAIS le canal (rmt_del_tx_channel couperait la sortie : broche
 *   flottante) : la broche est rendue au GPIO a 0 (gpio_set_direction -> gpio_output_enable remet
 *   la matrice par defaut)
 *   et l'emission est coupee jusqu'au redemarrage. */
#include "sdkconfig.h"
#if CONFIG_TC_BUS_TX_ENABLE
#include <string.h>
#include "tc_bus.h"
#include "tc_bus_priv.h"
#include "tc_tx_policy.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_TC_BUS_OPEN_FORM_LONG
_Static_assert(CONFIG_TC_BUS_MY_SERIAL != 0,
               "TC_BUS_OPEN_FORM_LONG : TC_BUS_MY_SERIAL doit etre configure (non nul) pour emettre");
#define OPEN_LONG true
#else
#define OPEN_LONG false
#endif
_Static_assert(sizeof(rmt_symbol_word_t) == sizeof(uint32_t), "rmt_symbol_word_t doit tenir sur 32 bits");
/* un seul bloc memoire RMT (48 mots sur S3) : pas de remplissage par interruption pendant l'emission */
#define TX_SYM_MAX ((TC_SEGMENTS_LONG + 1) / 2)          /* 18 symboles pour un telegramme long */
_Static_assert(TX_SYM_MAX + 1 <= 48, "telegramme + marqueur de fin dans un bloc RMT");

static const char *TAG = "tc_bus";

#define TX_RES_HZ          1000000       /* 1 tick = 1 us */
#define TX_MEM_SYMBOLS     48
#define TX_DONE_TIMEOUT_MS 500           /* un telegramme long dure ~150 ms */
#define QUIET_WAIT_US      (2LL * 1000000)
#define SAMPLE_FRESH_US    50000LL       /* recepteur ADC en retard de plus : bus considere actif */
#define MUTEX_WAIT_MS      100
#define POLL_MS            10

static rmt_channel_handle_t s_ch;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t s_mutex;
static tc_tx_limiter_t s_lim;            /* sous s_mutex */
static rmt_symbol_word_t s_sym[TX_SYM_MAX];   /* lu par le pilote jusqu'a la fin : statique */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_block;                 /* blocages en cours (OTA), sous s_lock */
static tc_bus_tx_inhibit_fn_t s_inhibit;
static volatile bool s_dead;             /* emetteur en echec : plus aucune emission jusqu'au reboot */

/* Broche rendue au GPIO, sortie a 0 : registre de sortie a 0 d'abord, puis gpio_set_direction
 * (gpio_output_enable : matrice remise par defaut, le signal RMT ne commande plus la broche). */
static void pin_safe_low(void)
{
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);
    gpio_set_direction(CONFIG_TC_BUS_TX_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);
}

static void tx_kill(const char *why, esp_err_t e)
{
    s_dead = true;
    pin_safe_low();                      /* d'abord la broche : elle ne depend plus du RMT */
    if (s_ch != NULL) {
        rmt_disable(s_ch);               /* arrete une emission en cours ; le canal n'est pas supprime */
    }
    ESP_LOGE(TAG, "emetteur coupe (%s : %s), TX GPIO%d rendu au GPIO a 0 jusqu'au redemarrage",
             why, esp_err_to_name(e), CONFIG_TC_BUS_TX_GPIO);
}

uint32_t tc_bus_open_door_raw(bool *is_long)
{
    return tc_tx_open_raw(OPEN_LONG, CONFIG_TC_BUS_MY_SERIAL, CONFIG_TC_BUS_DOOR_ADDRESS, is_long);
}

void tc_bus_tx_block(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_block++;
    taskEXIT_CRITICAL(&s_lock);
}

void tc_bus_tx_unblock(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_block > 0) {
        s_block--;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void tc_bus_tx_set_inhibit(tc_bus_tx_inhibit_fn_t fn)
{
    s_inhibit = fn;
}

static bool tx_blocked(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool b = s_block > 0;
    taskEXIT_CRITICAL(&s_lock);
    tc_bus_tx_inhibit_fn_t inh = s_inhibit;
    /* sans recepteur, ni bus calme ni echo ne sont observables */
    return b || s_dead || s_ch == NULL || !tc_bus_is_running() || (inh != NULL && inh());
}

/* Dernier front, ou maintenant si le recepteur n'a pas traite d'echantillon recent. */
static int64_t bus_quiet_since(int64_t now)
{
    if (now - tc_rx_last_sample_us() > SAMPLE_FRESH_US) {
        return now;
    }
    return tc_rx_last_edge_us();
}

esp_err_t tc_tx_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        tx_kill("mutex", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    tc_tx_limiter_init(&s_lim);

    tc_telegram_t t = { 0 };
    t.raw = tc_bus_open_door_raw(&t.is_long);
    tc_proto_classify(&t);
    if (!tc_proto_tx_allowed(&t)) {      /* impossible par construction : ceinture et bretelles */
        tx_kill("telegramme d'ouverture hors liste blanche", ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    /* registre de sortie GPIO a 0 : c'est le niveau repris si la broche revient au GPIO */
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);
    rmt_tx_channel_config_t cfg = {
        .gpio_num = CONFIG_TC_BUS_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = TX_RES_HZ,
        .mem_block_symbols = TX_MEM_SYMBOLS,
        .trans_queue_depth = 1,
        .flags = {
            .invert_out = 0,
            .with_dma = 0,
            .init_level = 0,             /* repos a 0 fixe avant le routage vers la broche */
        },
    };
    esp_err_t e = rmt_new_tx_channel(&cfg, &s_ch);
    if (e != ESP_OK) {
        s_ch = NULL;
        tx_kill("rmt_new_tx_channel", e);
        return e;
    }
    /* pas de rmt_apply_carrier : le canal est cree sans modulation */
    const rmt_copy_encoder_config_t ec = {};
    e = rmt_new_copy_encoder(&ec, &s_enc);
    if (e != ESP_OK) {
        tx_kill("rmt_new_copy_encoder", e);
        return e;
    }
    e = rmt_enable(s_ch);
    if (e != ESP_OK) {
        tx_kill("rmt_enable", e);
        return e;
    }
    ESP_LOGW(TAG, "emission ACTIVE sur GPIO%d (RMT 1 MHz, repos 0) : seul %s raw=0x%0*lX peut etre emis "
             "(200 ms min, %d/min, bus calme %d ms, bloque pendant OTA et SoftAP)",
             CONFIG_TC_BUS_TX_GPIO, tc_type_name(t.type), t.is_long ? 8 : 4, (unsigned long)t.raw,
             TC_TX_RATE_MAX, (int)(TC_TX_QUIET_US / 1000));
    return ESP_OK;
}

static void log_raw(char *buf, size_t len, bool ok, uint32_t raw, bool is_long)
{
    if (ok) {
        snprintf(buf, len, "0x%0*lX", is_long ? 8 : 4, (unsigned long)raw);
    } else {
        snprintf(buf, len, "aucune");
    }
}

esp_err_t tc_bus_open_door(tc_bus_tx_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->err = ESP_OK;
    tc_telegram_t t = { 0 };
    t.raw = tc_bus_open_door_raw(&t.is_long);
    tc_proto_classify(&t);
    r->raw = t.raw;
    r->is_long = t.is_long;
    r->type = t.type;

    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        /* emetteur absent, ou une emission (avec son attente d'echo) est en cours */
        r->verdict = s_mutex == NULL ? TC_TX_BLOCKED : TC_TX_TOO_SOON;
        tc_bus_count_tx(false, 0);
        ESP_LOGW(TAG, "TX %s refuse : %s", tc_type_name(t.type), tc_tx_verdict_name(r->verdict));
        return ESP_ERR_INVALID_STATE;
    }

    /* politique ; bus occupe : on attend jusqu'a 2 s un bus calme depuis 200 ms */
    const int64_t t0 = esp_timer_get_time();
    tc_tx_verdict_t v;
    int64_t now;
    for (;;) {
        now = esp_timer_get_time();
        v = tc_tx_check(&s_lim, &t, now, bus_quiet_since(now), tx_blocked());
        if (v != TC_TX_BUS_BUSY || now - t0 >= QUIET_WAIT_US) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    r->verdict = v;
    if (v != TC_TX_OK) {
        tc_bus_count_tx(false, 0);
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "TX %s raw=0x%0*lX refuse : %s", tc_type_name(t.type), t.is_long ? 8 : 4,
                 (unsigned long)t.raw, tc_tx_verdict_name(v));
        return ESP_ERR_INVALID_STATE;
    }

    tc_segment_t segs[TC_MAX_SEGMENTS];
    size_t nseg = tc_proto_encode(t.raw, t.is_long, false, segs, TC_MAX_SEGMENTS);
    size_t nsym = tc_tx_segments_to_symbols(segs, nseg, &s_sym[0].val, TX_SYM_MAX);
    if (nsym == 0) {
        r->err = ESP_ERR_INVALID_SIZE;   /* rien n'est emis */
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "TX %s : encodage impossible, rien emis", tc_type_name(t.type));
        return r->err;
    }

    /* compte avant d'emettre : un echec RMT consomme aussi le quota (prudence) */
    const int64_t tx_start = now;
    tc_tx_record(&s_lim, now);
    tc_bus_count_tx(true, now);
    tc_bus_watch_arm(t.raw, t.is_long);
    const rmt_transmit_config_t tc = { .loop_count = 0, .flags = { .eot_level = 0 } };
    esp_err_t e = rmt_transmit(s_ch, s_enc, s_sym, nsym * sizeof(rmt_symbol_word_t), &tc);
    if (e == ESP_OK) {
        e = rmt_tx_wait_all_done(s_ch, TX_DONE_TIMEOUT_MS);
    }
    if (e != ESP_OK) {
        tx_kill("emission", e);
        tc_bus_watch_disarm();
        r->err = e;
        xSemaphoreGive(s_mutex);
        return e;
    }
    const int64_t end = esp_timer_get_time();
    tc_bus_watch_sent(end);

    /* echo dans la seconde, puis reponse dans les 500 ms qui suivent l'echo (1,5 s au plus) */
    tc_tx_watch_t w;
    for (;;) {
        tc_bus_watch_get(&w);
        now = esp_timer_get_time();
        if (w.reply) {
            break;
        }
        if (!w.echo && now - end > TC_TX_ECHO_WINDOW_US) {
            break;
        }
        if (w.echo && now - w.echo_us > TC_TX_REPLY_WINDOW_US) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    tc_bus_watch_disarm();
    tc_bus_watch_get(&w);                /* une reponse arrivee entre-temps compte encore */
    xSemaphoreGive(s_mutex);

    r->echo = w.echo;
    r->reply = w.reply;
    r->reply_raw = w.reply_raw;
    r->reply_is_long = w.reply_is_long;
    char rep[16];
    log_raw(rep, sizeof rep, w.reply, w.reply_raw, w.reply_is_long);
    ESP_LOGI(TAG, "TX %s raw=0x%0*lX -> echo=%s reply=%s (emission %lld ms, attente bus %lld ms)",
             tc_type_name(t.type), t.is_long ? 8 : 4, (unsigned long)t.raw, w.echo ? "oui" : "non", rep,
             (long long)((end - tx_start) / 1000), (long long)((tx_start - t0) / 1000));
    return ESP_OK;
}
#endif
