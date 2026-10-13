#include "app_selftest.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_net.h"
#include "app_ota.h"
#include "netlog.h"
#include "tc_bus.h"

static const char *TAG = "selftest";

/* Marge laissee a la tache d'auto-test au-dela de son propre delai avant le filet esp_timer. */
#define SELFTEST_GUARD_MARGIN_S 30

static esp_timer_handle_t s_guard_timer;

static const char *missing_condition(void)
{
    if (!(app_net_is_connected() || app_net_recovery_ap_active())) {
        return "pas d'acces reseau (ni station ni SoftAP)";
    }
#if CONFIG_APP_NETLOG_ENABLE
    if (!netlog_server_is_listening()) {
        return "serveur de logs absent";
    }
#endif
    if (!app_ota_is_running()) {
        return "serveur HTTP absent";
    }
    if (!app_ota_token_ok()) {
        return "token OTA inutilisable";     /* double du garde-fou de compilation */
    }
    if (!tc_bus_is_running()) {
        return "tache bus absente";
    }
    return NULL;
}

static void selftest_task(void *arg)
{
    (void)arg;
    int64_t deadline = esp_timer_get_time() + (int64_t)CONFIG_APP_SELFTEST_TIMEOUT_S * 1000000LL;
    const char *reason = NULL;
    bool ok = false;

    while (esp_timer_get_time() < deadline) {
        reason = missing_condition();
        if (reason == NULL) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
#if CONFIG_APP_SELFTEST_FORCE_FAIL
    if (ok) {
        ok = false;
        reason = "echec force par APP_SELFTEST_FORCE_FAIL";
    }
#endif
    if (ok) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "SELFTEST OK, image validee (%s)", esp_err_to_name(e));
    } else {
        ESP_LOGE(TAG, "SELFTEST FAILED apres %d s max (%s), rollback", CONFIG_APP_SELFTEST_TIMEOUT_S,
                 reason ? reason : "timeout");
        /* 10 s : laisser a un client de logs le temps de se reconnecter apres le reboot de l'OTA
           et de voir l'echec avant le rollback */
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();
        /* on ne revient ici que si le rollback est impossible */
        ESP_LOGE(TAG, "rollback impossible (%s) : validation forcee pour garder l'OTA", esp_err_to_name(e));
        e = esp_ota_mark_app_valid_cancel_rollback();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "validation forcee impossible : %s", esp_err_to_name(e));
        }
    }
    vTaskDelete(NULL);
}

/* Filet de securite : la tache esp_timer est plus prioritaire que la tache d'auto-test, donc un
 * auto-test affame ou bloque finit quand meme en rollback (redemarrage d'une image en attente). */
static void guard_cb(void *arg)
{
    (void)arg;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGE(TAG, "auto-test sans verdict apres %d s : redemarrage, le bootloader annulera l'image",
             CONFIG_APP_SELFTEST_TIMEOUT_S + SELFTEST_GUARD_MARGIN_S);
    esp_restart();
}

void app_selftest_start(void)
{
    /* hors APP_BENCH_TEST, app_config_check.c refuse deja de compiler dans ce cas */
    if (CONFIG_APP_SELFTEST_TIMEOUT_S >= CONFIG_APP_RECOVERY_AP_DELAY_S) {
        ESP_LOGW(TAG, "image de test APP_BENCH_TEST : auto-test (%d s) >= SoftAP (%d s), "
                 "une image sans Wi-Fi pourrait etre validee par le SoftAP, NE PAS INSTALLER",
                 CONFIG_APP_SELFTEST_TIMEOUT_S, CONFIG_APP_RECOVERY_AP_DELAY_S);
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "image deja validee, pas d'auto-test");
        return;
    }
    ESP_LOGW(TAG, "nouvelle image en attente : auto-test (%d s max)", CONFIG_APP_SELFTEST_TIMEOUT_S);
    const esp_timer_create_args_t g = { .callback = guard_cb, .name = "selftest_guard" };
    if (esp_timer_create(&g, &s_guard_timer) != ESP_OK ||
        esp_timer_start_once(s_guard_timer,
                             (uint64_t)(CONFIG_APP_SELFTEST_TIMEOUT_S + SELFTEST_GUARD_MARGIN_S) * 1000000ULL) != ESP_OK) {
        ESP_LOGE(TAG, "filet de l'auto-test impossible (timer)");
    }
    if (xTaskCreate(selftest_task, "selftest", 8192, NULL, 5, NULL) != pdPASS) {
        /* sans tache d'auto-test l'image resterait en attente et refuserait toute OTA : on valide */
        ESP_LOGE(TAG, "tache d'auto-test impossible : image validee pour garder l'OTA");
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "validation impossible : %s", esp_err_to_name(e));
        }
    }
}
