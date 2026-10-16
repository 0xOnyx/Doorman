#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netlog.h"
#include "app_status.h"
#include "app_net.h"
#include "app_ota.h"
#include "tc_bus.h"
#include "app_selftest.h"

/* Broches interdites pour le bus (spec 2.1, amendement ESP32-S3) : strapping 0/3/45/46, USB-Serial-JTAG
   19/20, UART0 43/44, flash et PSRAM du boitier 26-32 ; 22-25 n'existent pas. 33-37 restent libres avec
   flash et PSRAM quad (ESP32-S3FH4R2), elles ne sont prises qu'en mode octal. */
#define TC_PIN_OK(p) ((((p) >= 0 && (p) <= 21) || ((p) >= 33 && (p) <= 48)) && \
                      (p) != 0 && (p) != 3 && (p) != 19 && (p) != 20 && \
                      (p) != 43 && (p) != 44 && (p) != 45 && (p) != 46)
_Static_assert(TC_PIN_OK(CONFIG_TC_BUS_TX_GPIO), "TC_BUS_TX_GPIO interdite (strapping, flash, USB ou UART0)");
_Static_assert(TC_PIN_OK(CONFIG_TC_BUS_RX_GPIO), "TC_BUS_RX_GPIO interdite (strapping, flash, USB ou UART0)");
_Static_assert(CONFIG_TC_BUS_TX_GPIO != CONFIG_TC_BUS_RX_GPIO, "RX et TX sur la meme broche");

static const char *TAG = "main";

/* Spec 3 regle 1 : GPIO10 (TX bus) en sortie a 0 avant toute autre chose. */
static void bus_gpio_safe(void)
{
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);
    gpio_config_t tx = {
        .pin_bit_mask = 1ULL << CONFIG_TC_BUS_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx);
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);

    gpio_config_t rx = {
        .pin_bit_mask = 1ULL << CONFIG_TC_BUS_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rx);
}

static void nvs_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS a effacer (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(e));
    }
}

/* Phase 0 : tc_bus logge deja chaque telegramme. Les phases suivantes brancheront HomeKit ici. */
static void on_telegram(const tc_telegram_t *t, void *ctx)
{
    (void)t; (void)ctx;
}

#define HEALTH_PERIOD_S   30                /* periode interne du battement de coeur */
#define NET_DOWN_MAX_S    (15 * 60)         /* ni station ni SoftAP depuis plus longtemps : redemarrage */

#if CONFIG_APP_HEARTBEAT_S > 0
/* Marge de pile restante (octets) d'une tache, -1 si elle n'existe pas. */
static long stack_hwm(const char *name)
{
    TaskHandle_t h = xTaskGetHandle(name);
    return h ? (long)uxTaskGetStackHighWaterMark(h) : -1L;
}

static void heartbeat_log(void)
{
    tc_bus_stats_t s;
    tc_bus_get_stats(&s);
    ESP_LOGI(TAG, "HB up=%llds heap=%" PRIu32 " min_heap=%" PRIu32 " rssi=%d net=%s bus_ok=%" PRIu32 " bus_err=%" PRIu32,
             (long long)(esp_timer_get_time() / 1000000), esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(), app_net_rssi(),
             app_net_recovery_ap_active() ? "recovery" : (app_net_is_connected() ? "sta" : "down"),
             s.frames_ok, s.frames_err);
    ESP_LOGI(TAG, "HB pile libre (octets) sys_evt=%ld tc_rx=%ld netlog=%ld httpd=%ld esp_timer=%ld main=%ld tiT=%ld",
             stack_hwm("sys_evt"), stack_hwm("tc_rx"), stack_hwm("netlog"), stack_hwm("httpd"),
             stack_hwm("esp_timer"), stack_hwm("main"), stack_hwm("tiT"));
}
#endif

/* Image validee (pas en attente d'auto-test) : seules ces images s'auto-reparent par redemarrage ;
 * une image en attente est deja couverte par l'auto-test et son filet. */
static bool image_validated(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run == NULL) {
        return false;
    }
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return true;                        /* pas d'etat (premier flash USB) : image consideree validee */
    }
    return st != ESP_OTA_IMG_PENDING_VERIFY;
}

/* Verifie que l'acces distant tient toujours ; sinon redemarre (renvoie seulement si tout va bien). */
static void health_check(int64_t *net_up_us)
{
    int64_t now = esp_timer_get_time();
    if (app_net_is_connected() || app_net_recovery_ap_active()) {
        *net_up_us = now;
    }
    if (!image_validated()) {
        return;
    }
    const char *reason = NULL;
    if (!app_ota_is_running()) {
        reason = "serveur HTTP absent";
    }
#if CONFIG_APP_NETLOG_ENABLE
    else if (!netlog_server_is_listening()) {
        reason = "serveur de logs absent";
    }
#endif
    else if (now - *net_up_us > (int64_t)NET_DOWN_MAX_S * 1000000LL) {
        reason = "ni station ni SoftAP depuis plus de 15 min";
    }
    if (reason != NULL) {
        ESP_LOGE(TAG, "SANTE : %s, redemarrage", reason);
        vTaskDelay(pdMS_TO_TICKS(1000));    /* laisser partir les logs reseau */
        esp_restart();
    }
}

void app_main(void)
{
    bus_gpio_safe();
#if CONFIG_APP_NETLOG_ENABLE
    if (netlog_init((size_t)CONFIG_APP_NETLOG_BUFFER_KB * 1024) != ESP_OK) {
        ESP_LOGE(TAG, "netlog_init: pas de memoire");
    }
#endif
    nvs_init();
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "interphone-homekit %s | partition %s (%s) | reset %s | heap %" PRIu32,
             app->version, run ? run->label : "?", app_status_partition_state(run),
             app_status_reset_reason_name(esp_reset_reason()), esp_get_free_heap_size());

    app_net_start();
#if CONFIG_APP_NETLOG_ENABLE
    netlog_server_start(CONFIG_APP_NETLOG_PORT);
#endif
    app_ota_start();
    tc_bus_start(on_telegram, NULL);
    /* Diagnostic de boot, apres tc_bus_start pour voir la configuration des broches du bus :
       attendu RX sans pull (analogique avec le recepteur ADC : entree numerique coupee), TX sortie
       a 0. Ecrit sur stdout : console seulement, pas dans les logs reseau. */
    gpio_dump_io_configuration(stdout, (1ULL << CONFIG_TC_BUS_RX_GPIO) | (1ULL << CONFIG_TC_BUS_TX_GPIO));
    app_selftest_start();

    /* Battement de coeur : controles de sante toutes les 30 s, log toutes les APP_HEARTBEAT_S
       (0 = pas de log, les controles tournent quand meme). */
    int64_t net_up_us = esp_timer_get_time();
#if CONFIG_APP_HEARTBEAT_S > 0
    int64_t next_log_us = esp_timer_get_time() + (int64_t)CONFIG_APP_HEARTBEAT_S * 1000000LL;
#endif
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_PERIOD_S * 1000));
        health_check(&net_up_us);
#if CONFIG_APP_HEARTBEAT_S > 0
        if (esp_timer_get_time() >= next_log_us) {
            next_log_us += (int64_t)CONFIG_APP_HEARTBEAT_S * 1000000LL;
            heartbeat_log();
        }
#endif
    }
}
