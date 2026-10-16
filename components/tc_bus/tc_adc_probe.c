/* Diagnostic analogique de la ligne RX (CONFIG_TC_BUS_ADC_PROBE) : remplace la reception RMT
 * et logge la tension min / max vue sur la broche RX, pour mesurer l'amplitude reelle du bus
 * (le bus ne descend pas forcement a 0 V). Mode de mise au point uniquement. */
#include "sdkconfig.h"
#if CONFIG_TC_BUS_ADC_PROBE
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tc_proto.h"

static const char *TAG = "tc_adc";

#define WINDOW_US        250000    /* fenetre de mesure */
#define ACTIVITY_MV      150       /* ecart min/max au-dela duquel on logge la fenetre */
#define IDLE_LOG_US      30000000  /* une ligne de repos toutes les 30 s */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

static int to_mv(int raw)
{
    int mv;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return raw * 3100 / 4095;              /* approximation sans calibration */
}

/* Seuils avec hysteresis (mV sur la broche) : repos ~2950 mV, bas mesure ~860 mV. */
#define THR_LOW_MV       1700      /* passe a "bas" (mark) sous ce seuil */
#define THR_HIGH_MV      2100      /* repasse a "haut" (space) au-dessus */
#define FRAME_END_US     7000      /* repos plus long : fin de trame */
#define MAX_SEG          TC_MAX_SEGMENTS

static void log_frame(tc_segment_t *segs, size_t n)
{
    size_t k = tc_proto_merge_glitches(segs, n, CONFIG_TC_BUS_GLITCH_US);
    k = tc_proto_strip_trailing_idle(segs, k);
    tc_telegram_t t;
    tc_decode_error_t err;
    if (tc_proto_decode(segs, k, &t, &err)) {
        tc_proto_classify(&t);
        ESP_LOGI(TAG, "TC ADC %s raw=0x%0*lX len=%u serial=0x%05lX addr=%u chk=ok%s",
                 tc_type_name(t.type), t.is_long ? 8 : 4, (unsigned long)t.raw,
                 t.is_long ? 32u : (t.type == TC_TYPE_ACK ? 4u : 16u),
                 (unsigned long)t.serial, t.address, t.is_response ? " rsp" : "");
        return;
    }
    /* echec : durees brutes pour l'analyse */
    char line[300];
    int pos = snprintf(line, sizeof line, "TC ADC ERR %s n=%u :", tc_decode_error_name(err), (unsigned)n);
    for (size_t i = 0; i < n && pos < (int)sizeof line - 8; i++) {
        pos += snprintf(line + pos, sizeof line - pos, " %u%c", segs[i].duration_us, segs[i].mark ? 'b' : 'h');
    }
    ESP_LOGW(TAG, "%s", line);
}

static void probe_task(void *arg)
{
    volatile bool *running = arg;
    static tc_segment_t segs[MAX_SEG];
    size_t n = 0;
    bool low = false;                      /* niveau courant apres hysteresis */
    int64_t edge_us = esp_timer_get_time();
    int64_t win_end = edge_us + WINDOW_US, last_idle_log = 0;
    int rmin = 4095, rmax = 0, cnt = 0;
    *running = true;
    for (;;) {
        int raw;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (raw < rmin) rmin = raw;
        if (raw > rmax) rmax = raw;
        cnt++;
        int mv = to_mv(raw);
        bool new_low = low ? (mv < THR_HIGH_MV) : (mv < THR_LOW_MV);
        if (new_low != low) {
            uint32_t d = (uint32_t)(now - edge_us);
            if (n < MAX_SEG) {
                segs[n].duration_us = d > 0xFFFF ? 0xFFFF : (uint16_t)d;
                segs[n].mark = low;        /* le segment qui se termine */
                n++;
            }
            low = new_low;
            edge_us = now;
        } else if (!low && n > 0 && now - edge_us > FRAME_END_US) {
            log_frame(segs, n);            /* trame terminee (repos > 7 ms) */
            n = 0;
        } else if (!low && n == 0) {
            edge_us = now;                 /* au repos : le premier segment commence au front */
        }
        if (now >= win_end) {
            int mn = to_mv(rmin), mx = to_mv(rmax);
            if (mx - mn >= ACTIVITY_MV) {
                ESP_LOGI(TAG, "ACTIVITE min=%d mV max=%d mV (%d mesures / 250 ms)", mn, mx, cnt);
            } else if (now - last_idle_log >= IDLE_LOG_US) {
                ESP_LOGI(TAG, "repos min=%d mV max=%d mV (%d mesures / 250 ms)", mn, mx, cnt);
                last_idle_log = now;
            }
            rmin = 4095; rmax = 0; cnt = 0;
            win_end = now + WINDOW_US;
        }
    }
}

esp_err_t tc_adc_probe_start(volatile bool *running)
{
    adc_unit_t unit;
    esp_err_t e = adc_oneshot_io_to_channel(CONFIG_TC_BUS_RX_GPIO, &unit, &s_chan);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d n'est pas une entree ADC", CONFIG_TC_BUS_RX_GPIO);
        return e;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    e = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (e != ESP_OK) {
        return e;
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    e = adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    if (e != ESP_OK) {
        return e;
    }
    adc_cali_curve_fitting_config_t cc = { .unit_id = unit, .chan = s_chan,
                                           .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "pas de calibration ADC : tensions approximatives");
    }
    ESP_LOGW(TAG, "MODE DIAGNOSTIC ADC sur GPIO%d : decodage du bus desactive", CONFIG_TC_BUS_RX_GPIO);
    /* priorite 1 sur le coeur 1, boucle sans pause : le build de diagnostic coupe la surveillance d IDLE du coeur 1 */
    return xTaskCreatePinnedToCore(probe_task, "tc_adc", 4096, (void *)running, 1, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}
#endif
