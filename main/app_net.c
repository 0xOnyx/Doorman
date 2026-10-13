#include "app_net.h"
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "mdns.h"

static const char *TAG = "net";

#define BACKOFF_MAX_S 60

#define LOG_IF_ERR(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { \
    ESP_LOGE(TAG, "%s: %s", #x, esp_err_to_name(_e)); } } while (0)

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_recovery_timer;
static int s_backoff_s = 1;
static volatile bool s_connected;
static volatile bool s_ap_active;
static volatile bool s_ap_stop_deferred;
static bool s_ap_enabled;

/* --- SoftAP de secours : complete a la tache 11 --- */
static void start_recovery_ap(void) { }
static void stop_recovery_ap(void) { }

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void recovery_cb(void *arg)
{
    (void)arg;
    if (s_connected || s_ap_active) {
        return;
    }
    start_recovery_ap();
}

/* Armee au boot et quand une connexion etablie est perdue, jamais rearmee si elle tourne deja. */
static void arm_recovery_timer(void)
{
    if (!s_ap_enabled || s_ap_active || esp_timer_is_active(s_recovery_timer)) {
        return;
    }
    esp_timer_start_once(s_recovery_timer, (uint64_t)CONFIG_APP_RECOVERY_AP_DELAY_S * 1000000ULL);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        bool was_connected = s_connected;
        s_connected = false;
        ESP_LOGW(TAG, "deconnecte (raison %d), nouvel essai dans %d s", d->reason, s_backoff_s);
        if (was_connected) {
            arm_recovery_timer();
        }
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_s * 1000000ULL);
        s_backoff_s = (s_backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : s_backoff_s * 2;
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client connecte au SoftAP de secours");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "client parti du SoftAP de secours");
        break;
    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = data;
    s_connected = true;
    s_backoff_s = 1;
    esp_timer_stop(s_recovery_timer);
    ESP_LOGI(TAG, "connecte, IP " IPSTR ", rssi %d", IP2STR(&e->ip_info.ip), app_net_rssi());
    if (s_ap_active) {
        stop_recovery_ap();
    }
}

void app_net_start(void)
{
    s_ap_enabled = strlen(CONFIG_APP_RECOVERY_AP_PASSWORD) >= 8;
    if (!s_ap_enabled) {
        ESP_LOGE(TAG, "APP_RECOVERY_AP_PASSWORD trop court (< 8) : SoftAP de secours DESACTIVE");
    }
    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "APP_WIFI_SSID vide : aucune connexion possible");
    }

    LOG_IF_ERR(esp_netif_init());
    LOG_IF_ERR(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, CONFIG_APP_HOSTNAME);

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    LOG_IF_ERR(esp_wifi_init(&icfg));
    LOG_IF_ERR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    LOG_IF_ERR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL));

    const esp_timer_create_args_t rc = { .callback = reconnect_cb, .name = "reconnect" };
    const esp_timer_create_args_t rv = { .callback = recovery_cb, .name = "recovery" };
    LOG_IF_ERR(esp_timer_create(&rc, &s_reconnect_timer));
    LOG_IF_ERR(esp_timer_create(&rv, &s_recovery_timer));

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strlcpy((char *)wc.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = strlen(CONFIG_APP_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    LOG_IF_ERR(esp_wifi_set_storage(WIFI_STORAGE_RAM));     /* toujours les identifiants du Kconfig */
    LOG_IF_ERR(esp_wifi_set_mode(WIFI_MODE_STA));
    LOG_IF_ERR(esp_wifi_set_config(WIFI_IF_STA, &wc));
    LOG_IF_ERR(esp_wifi_start());
    LOG_IF_ERR(esp_wifi_set_max_tx_power(CONFIG_APP_WIFI_TX_POWER));
    ESP_LOGI(TAG, "station demarree, ssid '%s', puissance max %d", CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_TX_POWER);

    arm_recovery_timer();

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_APP_HOSTNAME);
        mdns_instance_name_set("Interphone");
        ESP_LOGI(TAG, "mDNS %s.local", CONFIG_APP_HOSTNAME);
    } else {
        ESP_LOGE(TAG, "mdns_init a echoue");
    }
}

bool app_net_is_connected(void)
{
    return s_connected;
}

bool app_net_recovery_ap_active(void)
{
    return s_ap_active;
}

int app_net_rssi(void)
{
    wifi_ap_record_t r;
    if (s_connected && esp_wifi_sta_get_ap_info(&r) == ESP_OK) {
        return r.rssi;
    }
    return 0;
}

void app_net_ota_finished(void)
{
    if (s_ap_stop_deferred && s_connected) {
        stop_recovery_ap();
    }
}
