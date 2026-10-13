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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_auth.h"
#include "app_status.h"
#include "app_net.h"
#include "tc_bus.h"

static const char *TAG = "ota";

#define OTA_CHUNK 4096
#define TOKEN_MIN_LEN 32
#define OTA_DEADLINE_US (5LL * 60 * 1000000)   /* un transfert complet ne doit pas monopoliser le serveur */

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

/* Fin d'une OTA. reboot=true : la reponse 200 est envoyee et le redemarrage est programme.
 * reboot=false : echec, on libere le bus et on previent app_net (arret differe du SoftAP). */
static esp_err_t ota_finish(httpd_req_t *req, const char *status, const char *msg, bool reboot)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, reboot ? "application/json" : "text/plain");
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "pile httpd libre min : %u octets", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (reboot) {
        ESP_LOGI(TAG, "OTA OK, redemarrage dans 1 s");
        if (esp_timer_start_once(s_reboot_timer, 1000000) != ESP_OK) {
            /* sans timer, on ne doit pas rester bloque en "OTA en cours" : reboot immediat */
            ESP_LOGE(TAG, "timer de reboot indisponible, redemarrage immediat");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        return ESP_OK;
    }
    ESP_LOGW(TAG, "OTA echouee: %s", msg);
    s_in_progress = false;
    tc_bus_tx_unblock();
    app_net_ota_finished();
    return ESP_FAIL;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_FAIL;
    }
    if (s_in_progress) {
        return reply_close(req, "409 Conflict", "ota already in progress\n");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return reply_close(req, "500 Internal Server Error", "no ota partition\n");
    }
    if (!ota_size_ok(req->content_len, part->size)) {
        return reply_close(req, "413 Payload Too Large", "missing or oversized Content-Length\n");
    }

    s_in_progress = true;
    tc_bus_tx_block();
    ESP_LOGI(TAG, "OTA: %u octets vers %s", (unsigned)req->content_len, part->label);

    esp_ota_handle_t h = 0;
    /* Effacer aussi le secteur qui suit l'image : sinon le bloc de signature d'un envoi precedent
       peut y rester et completer une image tronquee (constate sur l'etabli). */
    size_t erase_len = req->content_len + 4096;
    if (erase_len > part->size) {
        erase_len = part->size;
    }
    esp_err_t e = esp_ota_begin(part, erase_len, &h);
    if (e == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return ota_finish(req, "409 Conflict", "self-test pending\n", false);
    }
    if (e != ESP_OK) {
        if (h != 0) {
            esp_ota_abort(h);          /* esp_ota_begin garde l'entree si l'effacement echoue */
        }
        return ota_finish(req, "500 Internal Server Error", esp_err_to_name(e), false);
    }

    size_t remaining = req->content_len;
    bool first = true;
    const int64_t deadline = esp_timer_get_time() + OTA_DEADLINE_US;
    while (remaining > 0) {
        if (esp_timer_get_time() > deadline) {
            esp_ota_abort(h);
            return ota_finish(req, "500 Internal Server Error", "transfer too slow\n", false);
        }
        size_t want = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int r = httpd_req_recv(req, (char *)s_buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            /* 10 s sans un octet : on abandonne, sinon un client bloque monopolise le serveur */
            esp_ota_abort(h);
            return ota_finish(req, "500 Internal Server Error", "receive timeout\n", false);
        }
        if (r <= 0) {
            esp_ota_abort(h);
            return ota_finish(req, "500 Internal Server Error", "receive error\n", false);
        }
        e = esp_ota_write(h, s_buf, (size_t)r);
        if (e != ESP_OK) {
            esp_ota_abort(h);
            bool bad_image = first && e == ESP_ERR_OTA_VALIDATE_FAILED;
            return ota_finish(req, bad_image ? "400 Bad Request" : "500 Internal Server Error",
                              bad_image ? "not a firmware image\n" : esp_err_to_name(e), false);
        }
        first = false;
        remaining -= (size_t)r;
    }

    e = esp_ota_end(h);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        return ota_finish(req, "400 Bad Request", "image invalid or unsigned\n", false);
    }
    if (e != ESP_OK) {
        return ota_finish(req, "500 Internal Server Error", esp_err_to_name(e), false);
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        return ota_finish(req, "500 Internal Server Error", esp_err_to_name(e), false);
    }
    return ota_finish(req, "200 OK", "{\"status\":\"ok\",\"reboot_in_ms\":1000}\n", true);
}

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
    const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(s_server, &ota);
    s_running = true;
    ESP_LOGI(TAG, "serveur HTTP sur le port %d (/info, /ota, /reboot)", CONFIG_APP_HTTP_PORT);
}

bool app_ota_is_running(void)
{
    return s_running;
}

bool app_ota_in_progress(void)
{
    return s_in_progress;
}

bool app_ota_token_ok(void)
{
    return s_token_ok;
}
