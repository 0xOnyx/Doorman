#include "app_ota.h"
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "ota_auth.h"
#include "app_status.h"
#include "app_net.h"
#include "tc_bus.h"

static const char *TAG = "ota";

#define OTA_CHUNK 4096
#define TOKEN_MIN_LEN 32

static httpd_handle_t s_server;
static esp_timer_handle_t s_reboot_timer;
static esp_timer_handle_t s_token_warn_timer;
static volatile bool s_in_progress;
static volatile bool s_running;
static bool s_token_ok;
static uint8_t s_buf[OTA_CHUNK];        /* alloue une fois, statique : pas d'echec possible au boot */

/* Repond et ferme la connexion (ESP_FAIL => httpd ferme le socket, utile quand le corps n'a pas ete lu). */
static esp_err_t reply_close(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

static bool check_auth(httpd_req_t *req)
{
    if (!s_token_ok) {
        reply_close(req, "503 Service Unavailable", "OTA token not configured (min 32 chars)\n");
        return false;
    }
    char hdr[160];
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    bool ok = false;
    if (len > 0 && len < sizeof hdr &&
        httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK) {
        ok = ota_auth_check(hdr, CONFIG_APP_OTA_TOKEN);
    }
    if (!ok) {
        reply_close(req, "401 Unauthorized", "unauthorized\n");
    }
    return ok;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void token_warn_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "APP_OTA_TOKEN trop court (< %d caracteres) : OTA et /info refuses", TOKEN_MIN_LEN);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_FAIL;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    tc_bus_stats_t bs;
    tc_bus_get_stats(&bs);

    char body[512];
    int n = snprintf(body, sizeof body,
        "{\"version\":\"%s\",\"idf\":\"%s\",\"partition\":\"%s\",\"ota_state\":\"%s\","
        "\"other_partition\":\"%s\",\"other_state\":\"%s\",\"uptime_s\":%lld,"
        "\"heap_free\":%" PRIu32 ",\"heap_min\":%" PRIu32 ",\"rssi\":%d,\"reset_reason\":\"%s\","
        "\"net\":\"%s\",\"bus_frames_ok\":%" PRIu32 ",\"bus_frames_err\":%" PRIu32 "}\n",
        app->version, esp_get_idf_version(),
        run ? run->label : "?", app_status_partition_state(run),
        other ? other->label : "?", app_status_partition_state(other),
        (long long)(esp_timer_get_time() / 1000000),
        esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
        app_net_rssi(), app_status_reset_reason_name(esp_reset_reason()),
        app_net_recovery_ap_active() ? "recovery" : "sta",
        bs.frames_ok, bs.frames_err);
    if (n < 0 || n >= (int)sizeof body) {
        return reply_close(req, "500 Internal Server Error", "info too long\n");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "redemarrage demande par /reboot");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\",\"reboot_in_ms\":1000}\n");
    esp_err_t e = esp_timer_start_once(s_reboot_timer, 1000000);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "reboot non programme: %s", esp_err_to_name(e));
    }
    return ESP_OK;
}

/* --- POST /ota : tache 10 --- */

void app_ota_start(void)
{
    s_token_ok = strlen(CONFIG_APP_OTA_TOKEN) >= TOKEN_MIN_LEN;
    const esp_timer_create_args_t rb = { .callback = reboot_cb, .name = "reboot" };
    if (esp_timer_create(&rb, &s_reboot_timer) != ESP_OK) {
        ESP_LOGE(TAG, "creation du timer de reboot impossible");
    }
    if (!s_token_ok) {
        token_warn_cb(NULL);
        const esp_timer_create_args_t tw = { .callback = token_warn_cb, .name = "token_warn" };
        if (esp_timer_create(&tw, &s_token_warn_timer) == ESP_OK) {
            esp_timer_start_periodic(s_token_warn_timer, 60ULL * 1000000ULL);
        }
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_APP_HTTP_PORT;
    cfg.ctrl_port = 32700;               /* distinct de celui du SDK HomeKit (32859) */
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.enable_so_linger = true;         /* un refus (401/413...) avant lecture du corps arrive au client */
    cfg.linger_timeout = 2;              /* au lieu d'un RST qui masquerait la reponse */

    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        return;
    }
    const httpd_uri_t info = { .uri = "/info", .method = HTTP_GET, .handler = info_get };
    const httpd_uri_t reboot = { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post };
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &reboot);
    s_running = true;
    ESP_LOGI(TAG, "serveur HTTP sur le port %d", CONFIG_APP_HTTP_PORT);
}

bool app_ota_is_running(void)
{
    return s_running;
}

bool app_ota_in_progress(void)
{
    return s_in_progress;
}
