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

/* Broches interdites pour le bus (spec 2.1) : strapping 2/8/9, flash 11-17, USB 18/19, UART0 20/21. */
#define TC_PIN_OK(p) ((p) >= 0 && (p) <= 10 && (p) != 2 && (p) != 8 && (p) != 9)
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

#if CONFIG_APP_HEARTBEAT_S > 0
/* Marge de pile restante (octets) d'une tache, -1 si elle n'existe pas. */
static long stack_hwm(const char *name)
{
    TaskHandle_t h = xTaskGetHandle(name);
    return h ? (long)uxTaskGetStackHighWaterMark(h) : -1L;
}
#endif

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
    app_selftest_start();

#if CONFIG_APP_HEARTBEAT_S > 0
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_HEARTBEAT_S * 1000));
        tc_bus_stats_t s;
        tc_bus_get_stats(&s);
        ESP_LOGI(TAG, "HB up=%llds heap=%" PRIu32 " min_heap=%" PRIu32 " rssi=%d net=%s bus_ok=%" PRIu32 " bus_err=%" PRIu32,
                 (long long)(esp_timer_get_time() / 1000000), esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size(), app_net_rssi(),
                 app_net_recovery_ap_active() ? "recovery" : (app_net_is_connected() ? "sta" : "down"),
                 s.frames_ok, s.frames_err);
        ESP_LOGI(TAG, "HB pile libre (octets) sys_evt=%ld tc_rx=%ld netlog=%ld httpd=%ld",
                 stack_hwm("sys_evt"), stack_hwm("tc_rx"), stack_hwm("netlog"), stack_hwm("httpd"));
    }
#endif
}
