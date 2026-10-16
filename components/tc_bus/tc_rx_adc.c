/* Recepteur analogique (CONFIG_TC_BUS_RX_ADC) : la broche RX est echantillonnee par l'ADC1 en mode
 * continu (DMA), sans boucle active. Le bus installe ne descend qu'a ~0.86 V sur la broche (repos
 * ~2.95 V) : l'entree numerique ne voit pas ces bas, le seuil logiciel avec hysteresis si.
 * La tache se bloque dans adc_continuous_read ; le watchdog de taches reste actif. */
#include "sdkconfig.h"
#if CONFIG_TC_BUS_RX_ADC
#include <string.h>
#include "tc_bus_priv.h"
#include "tc_slicer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "tc_rx_adc : format de sortie DMA (TYPE2, 4 octets) et broches ADC1 verifies pour l'ESP32-S3 seulement"
#endif
/* ADC1 sur S3 : GPIO1..GPIO10 (ADC2 est pris par le Wi-Fi et instable en mode continu) */
_Static_assert(CONFIG_TC_BUS_RX_GPIO >= 1 && CONFIG_TC_BUS_RX_GPIO <= 10,
               "TC_BUS_RX_ADC : la broche RX doit etre une entree ADC1 (GPIO1..GPIO10 sur ESP32-S3)");
_Static_assert(CONFIG_TC_BUS_ADC_LOW_MV < CONFIG_TC_BUS_ADC_HIGH_MV,
               "TC_BUS_ADC_LOW_MV doit etre inferieur a TC_BUS_ADC_HIGH_MV (hysteresis)");
_Static_assert(CONFIG_TC_BUS_ADC_SAMPLE_HZ >= SOC_ADC_SAMPLE_FREQ_THRES_LOW &&
               CONFIG_TC_BUS_ADC_SAMPLE_HZ <= SOC_ADC_SAMPLE_FREQ_THRES_HIGH, "TC_BUS_ADC_SAMPLE_HZ hors limites du SoC");
_Static_assert(sizeof(adc_digi_output_data_t) == SOC_ADC_DIGI_RESULT_BYTES, "format de sortie DMA inattendu");

static const char *TAG = "tc_bus";

#define SAMPLE_HZ        CONFIG_TC_BUS_ADC_SAMPLE_HZ
#define PERIOD_NS        (1000000000u / SAMPLE_HZ)
/* Trame de conversion de 5 ms (100 echantillons, 400 octets a 20 kHz) : 200 reveils par seconde */
#define FRAME_SAMPLES    (SAMPLE_HZ / 200)
#define FRAME_BYTES      (FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
/* Reserve du pilote : 20 trames, ~100 ms (8000 octets a 20 kHz) si la tache est retardee */
#define POOL_BYTES       (FRAME_BYTES * 20)
#define READ_TIMEOUT_MS  1000              /* pas d'echantillon pendant 1 s : ADC arrete */
#define FULL_SCALE_MV    3100              /* sans calibration : 12 dB, 12 bits, approximation lineaire */
#define WINDOW_US        (30LL * 1000000)  /* ligne de repos et mesure de la cadence */
#define WARN_PERIOD_US   (10LL * 1000000)  /* au plus un avertissement de debordement par 10 s */
#define RATE_TOL_PCT     20                /* cadence mesuree (exacte, base esp_timer) adoptee a +-20 % du nominal */
#define IDLE_LOG_US      (5LL * 60 * 1000000)  /* ligne "bus repos" au plus toutes les 5 min */
#define RESTART_MIN_US   (5LL * 1000000)       /* redemarrage de l'ADC au plus toutes les 5 s */

_Static_assert(FRAME_BYTES % SOC_ADC_DIGI_DATA_BYTES_PER_CONV == 0, "conv_frame_size multiple de 4");

static adc_continuous_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static tc_slicer_t s_sl;
static uint8_t s_buf[FRAME_BYTES];
static volatile uint32_t s_ovf;          /* debordements de la reserve du pilote (ISR) */
static volatile int s_last_raw = -1;     /* dernier echantillon, -1 avant le premier */

static bool IRAM_ATTR on_pool_ovf(adc_continuous_handle_t h, const adc_continuous_evt_data_t *ed, void *ctx)
{
    (void)h; (void)ed; (void)ctx;
    s_ovf++;                               /* seule la tache lit, un seul ecrivain : pas de verrou */
    return false;
}

static int raw_to_mv(int raw)
{
    int mv;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return raw * FULL_SCALE_MV / 4095;
}

/* Plus petit brut dont la tension est >= mv (la conversion est croissante) */
static uint16_t mv_to_raw(int mv)
{
    int lo = 0, hi = 4096;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (raw_to_mv(mid) >= mv) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return (uint16_t)(lo > 4095 ? 4095 : lo);
}

static void adc_task(void *arg)
{
    (void)arg;
    uint32_t ovf_seen = 0;
    int64_t last_warn_us = -WARN_PERIOD_US;
    uint32_t ovf_masked = 0;
    int64_t win_start_us = esp_timer_get_time();
    uint32_t win_samples = 0;
    bool win_gap = true;                   /* la premiere fenetre inclut le demarrage : pas de mesure de cadence */
    int win_min = 4095, win_max = 0;
    bool timed_out = false;
    int64_t last_idle_log = -IDLE_LOG_US;
    int64_t last_restart = 0;
    uint32_t last_rate_logged = 0;
    for (;;) {
        uint32_t len = 0;
        esp_err_t e = adc_continuous_read(s_adc, s_buf, sizeof s_buf, &len, READ_TIMEOUT_MS);
        int64_t now = esp_timer_get_time();
        if (e != ESP_OK) {
            if (!timed_out) {
                ESP_LOGE(TAG, "adc_continuous_read: %s, plus d'echantillon", esp_err_to_name(e));
                timed_out = true;
                tc_bus_set_running(false);
                tc_slicer_reset(&s_sl);
            }
            if (e != ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(100));    /* erreur d'etat : ne pas tourner a vide */
            }
            /* plus d'echantillon (DMA arrete, par ex. apres une longue ecriture flash) : relancer le pilote */
            if (now - last_restart >= RESTART_MIN_US) {
                last_restart = now;
                esp_err_t se = adc_continuous_stop(s_adc);
                esp_err_t ss = adc_continuous_start(s_adc);
                ESP_LOGW(TAG, "relance de l'ADC (stop %s, start %s)", esp_err_to_name(se), esp_err_to_name(ss));
            }
            win_gap = true;
            continue;
        }
        if (timed_out) {
            ESP_LOGW(TAG, "echantillons ADC de retour");
            timed_out = false;
            tc_bus_set_running(true);
        }
        uint32_t ovf = s_ovf;
        if (ovf != ovf_seen) {
            /* des echantillons ont ete perdus : la trame en cours n'est plus mesurable */
            ovf_masked += ovf - ovf_seen;
            ovf_seen = ovf;
            tc_slicer_reset(&s_sl);
            win_gap = true;
            if (now - last_warn_us >= WARN_PERIOD_US) {
                ESP_LOGW(TAG, "ADC : reserve pleine, %lu trame(s) de conversion perdue(s)", (unsigned long)ovf_masked);
                ovf_masked = 0;
                last_warn_us = now;
            }
        }
        int raw = -1;
        for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= len; i += SOC_ADC_DIGI_RESULT_BYTES) {
            const adc_digi_output_data_t *d = (const adc_digi_output_data_t *)&s_buf[i];
            if (d->type2.unit != ADC_UNIT_1 || d->type2.channel != s_chan) {
                continue;                  /* resultat invalide */
            }
            raw = d->type2.data;
            if (raw < win_min) win_min = raw;
            if (raw > win_max) win_max = raw;
            win_samples++;
            if (tc_slicer_push(&s_sl, (uint16_t)raw)) {
                tc_bus_process_window(s_sl.segs, s_sl.n, now);
            }
        }
        if (raw >= 0) {
            s_last_raw = raw;
        }
        if (now - win_start_us >= WINDOW_US) {
            uint32_t edges = tc_slicer_take_edges(&s_sl);
            uint32_t rate = (uint32_t)((uint64_t)win_samples * 1000000 / (uint64_t)(now - win_start_us));
            if (!win_gap) {
                if (rate * 100 >= (uint32_t)SAMPLE_HZ * (100 - RATE_TOL_PCT) &&
                    rate * 100 <= (uint32_t)SAMPLE_HZ * (100 + RATE_TOL_PCT)) {
                    tc_slicer_set_period(&s_sl, 1000000000u / rate);
                    if (rate / 100 != last_rate_logged / 100) {
                        ESP_LOGI(TAG, "cadence ADC mesuree %lu Hz adoptee", (unsigned long)rate);
                        last_rate_logged = rate;
                    }
                } else {
                    ESP_LOGW(TAG, "ADC : cadence mesuree %lu Hz loin de %d Hz, duree nominale gardee",
                             (unsigned long)rate, SAMPLE_HZ);
                }
            }
            if (edges == 0 && win_samples > 0 && now - last_idle_log >= IDLE_LOG_US) {
                last_idle_log = now;
                ESP_LOGI(TAG, "bus repos min=%d max=%d mV (%lu Hz)", raw_to_mv(win_min), raw_to_mv(win_max),
                         (unsigned long)rate);
            }
            win_start_us = now;
            win_samples = 0;
            win_gap = false;
            win_min = 4095;
            win_max = 0;
        }
    }
}

esp_err_t tc_rx_start(void)
{
    adc_unit_t unit;
    esp_err_t e = adc_continuous_io_to_channel(CONFIG_TC_BUS_RX_GPIO, &unit, &s_chan);
    if (e != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d n'est pas une entree ADC1", CONFIG_TC_BUS_RX_GPIO);
        return e != ESP_OK ? e : ESP_ERR_INVALID_ARG;
    }
    const adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = POOL_BYTES,
        .conv_frame_size = FRAME_BYTES,
    };
    e = adc_continuous_new_handle(&hcfg, &s_adc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle: %s", esp_err_to_name(e));
        return e;
    }
    adc_digi_pattern_config_t pat = {
        .atten = ADC_ATTEN_DB_12,
        .channel = (uint8_t)s_chan,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    const adc_continuous_config_t ccfg = {
        .pattern_num = 1,
        .adc_pattern = &pat,
        .sample_freq_hz = SAMPLE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    /* met aussi la broche en mode analogique (entree numerique et pulls coupes) */
    e = adc_continuous_config(s_adc, &ccfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config: %s", esp_err_to_name(e));
        return e;
    }
    const adc_continuous_evt_cbs_t cbs = { .on_pool_ovf = on_pool_ovf };
    e = adc_continuous_register_event_callbacks(s_adc, &cbs, NULL);
    if (e != ESP_OK) {
        return e;
    }
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "pas de calibration ADC : seuils approximatifs (%d mV pleine echelle)", FULL_SCALE_MV);
    }
    /* seuils convertis une fois : la tache compare des valeurs brutes */
    uint16_t low_raw = mv_to_raw(CONFIG_TC_BUS_ADC_LOW_MV);
    uint16_t high_raw = mv_to_raw(CONFIG_TC_BUS_ADC_HIGH_MV);
    tc_slicer_init(&s_sl, low_raw, high_raw, PERIOD_NS);
    /* demarrer avant la tache : adc_continuous_read refuse un pilote arrete ; la reserve absorbe l'attente */
    e = adc_continuous_start(s_adc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start: %s", esp_err_to_name(e));
        return e;
    }
    if (xTaskCreate(adc_task, "tc_rx", TC_RX_TASK_STACK, NULL, TC_RX_TASK_PRIO, NULL) != pdPASS) {
        adc_continuous_stop(s_adc);
        return ESP_FAIL;
    }
    tc_bus_set_running(true);
    ESP_LOGI(TAG, "reception ADC sur GPIO%d (ADC1 canal %d, %d Hz) : bas < %d mV (brut %u), haut >= %d mV (brut %u)%s ; "
             "emission NON compilee (TX GPIO%d reste a 0)",
             CONFIG_TC_BUS_RX_GPIO, (int)s_chan, SAMPLE_HZ, CONFIG_TC_BUS_ADC_LOW_MV, low_raw,
             CONFIG_TC_BUS_ADC_HIGH_MV, high_raw, s_cali ? "" : " sans calibration", CONFIG_TC_BUS_TX_GPIO);
    return ESP_OK;
}

int tc_rx_mv(void)
{
    int raw = s_last_raw;
    return raw < 0 ? -1 : raw_to_mv(raw);
}
#endif
