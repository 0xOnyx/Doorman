#include "netlog.h"
#include "logbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "netlog";

#define NETLOG_LINE_MAX   256     /* pas LINE_MAX : deja defini par newlib */
#define CHUNK_MAX         512
#define POLL_MS           50

static logbuf_t s_lb;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;
static volatile bool s_listening;
static int s_port;

/* Hook esp_log : console d'origine + tampon circulaire. Ne bloque jamais. */
static int netlog_vprintf(const char *fmt, va_list ap)
{
    char line[NETLOG_LINE_MAX];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);

    int r = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);

    if (n > 0) {
        size_t len = ((size_t)n < sizeof line) ? (size_t)n : sizeof line - 1;
        portENTER_CRITICAL(&s_lock);
        logbuf_write(&s_lb, line, len);
        portEXIT_CRITICAL(&s_lock);
    }
    return r;
}

esp_err_t netlog_init(size_t buffer_bytes)
{
    uint8_t *storage = malloc(buffer_bytes);
    if (storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    logbuf_init(&s_lb, storage, buffer_bytes);
    s_orig_vprintf = esp_log_set_vprintf(netlog_vprintf);
    return ESP_OK;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Refuse poliment un second client pendant qu'un premier est servi. */
static void reject_pending(int lfd)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(lfd, &rf);
    struct timeval tv = { 0, 0 };
    if (select(lfd + 1, &rf, NULL, NULL, &tv) > 0) {
        int c = accept(lfd, NULL, NULL);
        if (c >= 0) {
            static const char busy[] = "busy: un seul client a la fois\n";
            send(c, busy, sizeof busy - 1, MSG_DONTWAIT);
            close(c);
        }
    }
}

static void serve_client(int lfd, int c)
{
    uint8_t chunk[CHUNK_MAX];
    size_t pending = 0, off = 0;
    uint64_t cursor;

    set_nonblock(c);
    portENTER_CRITICAL(&s_lock);
    cursor = s_lb.total - s_lb.len;          /* tout l'historique disponible */
    portEXIT_CRITICAL(&s_lock);

    for (;;) {
        if (pending == 0) {
            uint64_t lost = 0;
            portENTER_CRITICAL(&s_lock);
            size_t n = logbuf_read(&s_lb, &cursor, chunk, sizeof chunk, &lost);
            portEXIT_CRITICAL(&s_lock);
            if (lost > 0) {
                char msg[64];
                int m = snprintf(msg, sizeof msg, "\n[netlog: %llu octets perdus]\n", (unsigned long long)lost);
                send(c, msg, (size_t)m, MSG_DONTWAIT);
            }
            if (n == 0) {
                char tmp[16];
                int r = recv(c, tmp, sizeof tmp, MSG_DONTWAIT);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    return;                   /* client parti */
                }
                reject_pending(lfd);
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                continue;
            }
            pending = n;
            off = 0;
        }
        int w = send(c, chunk + off, pending, MSG_DONTWAIT);
        if (w > 0) {
            off += (size_t)w;
            pending -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        } else {
            return;
        }
    }
}

static void netlog_task(void *arg)
{
    (void)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (lfd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)s_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen: errno %d", errno);
        close(lfd);
        vTaskDelete(NULL);
        return;
    }
    s_listening = true;
    ESP_LOGI(TAG, "serveur de logs sur le port %d", s_port);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "client connecte");
        serve_client(lfd, c);
        close(c);
        ESP_LOGI(TAG, "client deconnecte");
    }
}

esp_err_t netlog_server_start(int port)
{
    s_port = port;
    BaseType_t ok = xTaskCreate(netlog_task, "netlog", 3072, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

bool netlog_server_is_listening(void)
{
    return s_listening;
}
