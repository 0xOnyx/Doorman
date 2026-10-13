#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netlog.h"
#include "app_status.h"
#include "app_net.h"

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
}
