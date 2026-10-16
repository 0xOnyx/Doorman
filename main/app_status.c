#include "app_status.h"
#include "driver/temperature_sensor.h"

const char *app_status_reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

const char *app_status_ota_state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:                         return "undefined";
    }
}

const char *app_status_partition_state(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (p == NULL) {
        return "none";
    }
    esp_err_t e = esp_ota_get_state_partition(p, &st);
    if (e == ESP_ERR_NOT_FOUND) {
        return "none";
    }
    if (e != ESP_OK) {
        return "error";
    }
    return app_status_ota_state_name(st);
}

int app_status_chip_temp_c(void)
{
    static temperature_sensor_handle_t s_tsens;
    static bool s_tried;
    if (!s_tried) {
        s_tried = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens = NULL;
        }
    }
    float c;
    if (s_tsens == NULL || temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return -1000;
    }
    return (int)(c + (c >= 0 ? 0.5f : -0.5f));
}
