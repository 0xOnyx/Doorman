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

static void probe_task(void *arg)
{
    volatile bool *running = arg;
    int64_t last_idle_log = 0;
    *running = true;
    for (;;) {
        int rmin = 4095, rmax = 0, n = 0;
        int64_t end = esp_timer_get_time() + WINDOW_US;
        while (esp_timer_get_time() < end) {
            int raw;
            if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) {
                if (raw < rmin) rmin = raw;
                if (raw > rmax) rmax = raw;
                n++;
            }
        }
        int mn = to_mv(rmin), mx = to_mv(rmax);
        int64_t now = esp_timer_get_time();
        if (mx - mn >= ACTIVITY_MV) {
            ESP_LOGI(TAG, "ACTIVITE min=%d mV max=%d mV ecart=%d mV (%d mesures)", mn, mx, mx - mn, n);
        } else if (now - last_idle_log >= IDLE_LOG_US) {
            ESP_LOGI(TAG, "repos min=%d mV max=%d mV (%d mesures / %d ms)", mn, mx, n, WINDOW_US / 1000);
            last_idle_log = now;
        }
        vTaskDelay(1);                     /* laisser respirer IDLE (chien de garde) */
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
    return xTaskCreatePinnedToCore(probe_task, "tc_adc", 3072, (void *)running, 1, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}
#endif
