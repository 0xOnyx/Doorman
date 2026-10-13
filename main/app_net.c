#include "app_net.h"
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "mdns.h"
#include "app_ota.h"

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

/* --- SoftAP de secours --- */
#define RECOVERY_AP_SSID "interphone-recovery"

static void schedule_reconnect(void);
static wifi_config_t s_ap_cfg;               /* statique : partage entre boot et tache esp_timer */

/* Mot de passe WPA2 accepte par le pilote : 8 a 63 caracteres, ou exactement 64 chiffres hexa. */
static bool ap_password_valid(const char *pw)
{
    size_t n = strlen(pw);
    if (n >= 8 && n <= 63) {
        return true;
    }
    if (n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = pw[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static void build_ap_config(void)
{
    memset(&s_ap_cfg, 0, sizeof s_ap_cfg);
    strlcpy((char *)s_ap_cfg.ap.ssid, RECOVERY_AP_SSID, sizeof s_ap_cfg.ap.ssid);
    s_ap_cfg.ap.ssid_len = strlen(RECOVERY_AP_SSID);
    strncpy((char *)s_ap_cfg.ap.password, CONFIG_APP_RECOVERY_AP_PASSWORD, sizeof s_ap_cfg.ap.password);
    s_ap_cfg.ap.channel = 1;
    s_ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_ap_cfg.ap.max_connection = 2;
    s_ap_cfg.ap.pmf_cfg.required = false;
}

static void stop_recovery_ap(void);

static void start_recovery_ap(void)
{
    /* la config AP est deja chargee au boot : le passage en APSTA ne montre jamais l'AP ouvert par defaut */
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) {
        e = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP de secours impossible : %s", esp_err_to_name(e));
        esp_wifi_set_mode(WIFI_MODE_STA);    /* jamais d'AP mal configure */
        return;
    }
    s_ap_active = true;
    s_ap_stop_deferred = false;
    ESP_LOGW(TAG, "SoftAP de secours '%s' demarre (192.168.4.1), la station continue d'essayer",
             RECOVERY_AP_SSID);
    /* le changement de mode peut interrompre un essai en cours sans evenement : on en reprogramme un */
    schedule_reconnect();
    /* GOT_IP a pu arriver pendant le demarrage (autre tache) : ne pas laisser l'AP sans raison */
    if (s_connected) {
        stop_recovery_ap();
    }
}

static void stop_recovery_ap(void)
{
    s_ap_stop_deferred = true;               /* pose d'abord : ota_finished le verra quoi qu'il arrive */
    if (app_ota_in_progress()) {
        ESP_LOGI(TAG, "OTA en cours : arret du SoftAP differe");
        return;
    }
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "arret du SoftAP : %s", esp_err_to_name(e));
        return;                              /* etat conserve : l'AP tourne toujours */
    }
    s_ap_active = false;
    s_ap_stop_deferred = false;
    ESP_LOGI(TAG, "SoftAP de secours arrete");
}

/* Programme le prochain essai de connexion avec le delai courant, puis double le delai (max 60 s). */
static void schedule_reconnect(void)
{
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_s * 1000000ULL);
    s_backoff_s = (s_backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : s_backoff_s * 2;
}

/* Un echec synchrone de esp_wifi_connect (scan en cours, etc.) ne produit pas d'evenement
 * DISCONNECTED : il faut reprogrammer l'essai nous-memes, sinon la station reste muette. */
static void try_connect(void)
{
    if (s_connected) {
        return;
    }
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s, nouvel essai dans %d s", esp_err_to_name(e), s_backoff_s);
        schedule_reconnect();
    }
}

static void reconnect_cb(void *arg)
{
    (void)arg;
    try_connect();
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
        try_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        bool was_connected = s_connected;
        s_connected = false;
        ESP_LOGW(TAG, "deconnecte (raison %d), nouvel essai dans %d s", d->reason, s_backoff_s);
        if (was_connected) {
            arm_recovery_timer();
        }
        schedule_reconnect();
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
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_LOST_IP) {
        /* bail DHCP perdu alors qu'on est toujours associe : on n'est plus joignable */
        ESP_LOGW(TAG, "adresse IP perdue");
        if (s_connected) {
            s_connected = false;
            arm_recovery_timer();
        }
        return;
    }
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
    s_ap_enabled = ap_password_valid(CONFIG_APP_RECOVERY_AP_PASSWORD);
    if (!s_ap_enabled) {
        ESP_LOGE(TAG, "APP_RECOVERY_AP_PASSWORD invalide (8 a 63 caracteres, ou 64 hexa) : SoftAP de secours DESACTIVE");
    }
    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "APP_WIFI_SSID vide : aucune connexion possible");
    }

    LOG_IF_ERR(esp_netif_init());
    LOG_IF_ERR(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    LOG_IF_ERR(esp_netif_set_hostname(s_sta_netif, CONFIG_APP_HOSTNAME));   /* 32 caracteres max */

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    LOG_IF_ERR(esp_wifi_init(&icfg));
    LOG_IF_ERR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    LOG_IF_ERR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL));
    LOG_IF_ERR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip, NULL, NULL));

    const esp_timer_create_args_t rc = { .callback = reconnect_cb, .name = "reconnect" };
    const esp_timer_create_args_t rv = { .callback = recovery_cb, .name = "recovery" };
    LOG_IF_ERR(esp_timer_create(&rc, &s_reconnect_timer));
    LOG_IF_ERR(esp_timer_create(&rv, &s_recovery_timer));

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    /* strncpy volontaire : le pilote accepte un SSID de 32 octets et une PSK de 64 octets sans NUL final */
    strncpy((char *)wc.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = strlen(CONFIG_APP_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    LOG_IF_ERR(esp_wifi_set_storage(WIFI_STORAGE_RAM));     /* toujours les identifiants du Kconfig */
    if (s_ap_enabled) {
        /* precharger la config AP avant le demarrage : le mode APSTA ulterieur l'utilise directement */
        build_ap_config();
        LOG_IF_ERR(esp_wifi_set_mode(WIFI_MODE_APSTA));
        LOG_IF_ERR(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
    }
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
    if (s_ap_active && s_connected) {
        stop_recovery_ap();
    }
}
